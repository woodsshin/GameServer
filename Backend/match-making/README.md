# GameLift Matchmaking Service

C#/.NET gRPC 기반 매치메이킹 백엔드. 게임 클라이언트와 데디케이티드 서버(Dedicated Server)의 요청을 받아 AWS GameLift를 통해 게임 세션을 검색·배치하고, 플레이어를 슬롯이 비어 있는 세션에 연결합니다.

## 개요

- **역할**: 클라이언트가 매치메이킹을 요청하면, 이미 빈 슬롯이 있는 진행 중인 게임 세션을 우선 탐색하고, 없으면 GameLift Matchmaking(FlexMatch)을 통해 새 세션을 배치합니다.
- **통신 방식**: gRPC (Protocol Buffers)
- **인증**: 클라이언트용 EOS(Epic Online Services) JWT, 데디케이티드 서버용 S2S(Server-to-Server) JWT — 두 개의 JWT Bearer 스킴을 정책 기반으로 분리
- **인프라 연동**: AWS GameLift SDK (`AmazonGameLiftClient`)를 통한 세션 검색, 매치메이킹 티켓 발급/폴링, 플레이어 세션 생성

## 아키텍처

```
┌──────────────┐        EOS JWT         ┌────────────────────────┐        AWS SDK        ┌─────────────┐
│ Game Client  │ ─────────────────────▶ │                        │ ─────────────────────▶│             │
└──────────────┘   MatchMakingSvc       │   MatchMaking Service   │   StartMatchmaking     │ AWS GameLift │
                    (gRPC)              │   (ASP.NET Core /       │   SearchGameSessions    │  (FlexMatch, │
┌──────────────┐        S2S JWT         │    Kestrel + gRPC)      │   CreatePlayerSession   │   Fleets)    │
│ Dedicated    │ ─────────────────────▶ │                        │                        │             │
│ Server       │   MatchMakingSvcDS     └────────────────────────┘                        └─────────────┘
└──────────────┘
```

- `MatchMakingSvc` : 게임 클라이언트가 호출, `EpicUser` 정책(EOS 인증) 적용
- `MatchMakingSvcDS` : 데디케이티드 서버가 호출, `DedicatedServer` 정책(S2S 인증 + Role) 적용
- 두 서비스 모두 하나의 `MatchMakingService` 클래스에 구현되어 있으며, `[Authorize]` 속성과 ASP.NET Core Authorization Policy로 스킴을 구분

## 주요 기능

### 1. 매치메이킹 시작 (`StartMatchMaking`)
1. 클라이언트가 리전별 레이턴시(`LatencyInMs`)와 함께 요청
2. 레이턴시가 가장 낮은 리전을 기준으로 GameLift 클라이언트를 초기화
3. **빈 슬롯 우선 탐색**: `SearchGameSessionAsync`로 `hasAvailablePlayerSessions=true` 필터를 걸어 이미 열려 있는 세션 중 빈 자리가 있는 세션을 검색
4. 빈 세션을 찾으면 바로 `CreatePlayerSession`으로 합류 처리 (신규 배치 비용 절감)
5. 못 찾으면 GameLift `StartMatchmakingAsync`(FlexMatch)를 호출해 새 매치메이킹 티켓 발급 후, 완료될 때까지 폴링 (`WaitMatchmakingAsync`)

### 2. 플레이어 세션 생성 (`CreatePlayerSession`)
- 대상 게임 세션이 `ACTIVE` 상태인지 확인 후 플레이어 세션 발급

### 3. 게임 세션 생성 (`CreateGameSession`)
- `GameDataId` 기준으로 기존 세션을 먼저 검색(재사용), 없으면 신규 세션 생성 후 `ACTIVE` 상태가 될 때까지 대기(`CreateAndWaitGameSessionAsync`)
- `IdempotencyToken`으로 중복 요청 방지

### 4. 데디케이티드 서버 전용 API (`StartMatchMakingDS`, `CreateGameSessionDS`)
- 서버가 리전을 직접 지정해 매치메이킹/세션 생성을 트리거하는 별도 엔드포인트 (S2S 인증)

## 인증 구조

```csharp
// 두 개의 JWT Bearer 스킴 등록
builder.Services.AddAuthentication(JwtBearerDefaults.AuthenticationScheme)
    .AddJwtBearer("EOS", options => { ... })   // 게임 클라이언트
    .AddJwtBearer("S2S", options => { ... });  // 데디케이티드 서버

// 정책으로 분리
options.AddPolicy(AuthPolicy.EpicUser, policy => {
    policy.AuthenticationSchemes.Add("EOS");
    policy.RequireAuthenticatedUser();
    policy.RequireClaim(ClaimTypes.NameIdentifier);
});
options.AddPolicy(AuthPolicy.DedicatedServer, policy => {
    policy.AuthenticationSchemes.Add("S2S");
    policy.RequireAuthenticatedUser();
    policy.RequireRole("DedicatedServer");
});
```

각 gRPC 서비스 클래스는 `[Authorize(Policy = ...)]`로 어떤 스킴을 요구하는지 명시하여, 클라이언트용 엔드포인트와 서버용 엔드포인트가 서로 다른 신뢰 경계를 갖도록 분리했습니다.

## GameLift 연동 세부사항

- **리전 → Alias ID 매핑**: `GameLiftSettings`에서 리전 코드(`eu-central-1`, `ap-southeast-2` 등)를 GameLift Alias ID로 매핑, 레이턴시가 가장 낮은 리전을 골라 해당 Alias로 라우팅
- **인증 방식**: `IAMRole` 또는 Access Key/Secret Key 두 가지 방식을 설정으로 전환 가능 (`GameLiftSettings.AWSAuthenticationType`)
- **세션 배치 대기**: 세션 생성 후 `DescribeGameSessionsAsync`를 폴링하며 `ACTIVE` 상태를 기다리고, `TIMED_OUT`/`FAILED`/`CANCELLED`/`TERMINATED`는 실패로 처리
- **매치메이킹 티켓 폴링**: `DescribeMatchmakingAsync`로 상태(`COMPLETED`/`SEARCHING`/`FAILED` 등)를 확인하며 완료 또는 실패까지 대기

## 프로토콜 정의 (`match_making.proto`)

| 서비스 | RPC | 설명 |
|---|---|---|
| `MatchMakingSvc` | `StartMatchMaking` | 클라이언트 매치메이킹 시작 |
| | `CreatePlayerSession` | 특정 세션에 플레이어 합류 |
| | `CreateGameSession` | 신규(또는 재사용) 게임 세션 생성 |
| `MatchMakingSvcDS` | `StartMatchMakingDS` | 데디케이티드 서버 매치메이킹 |
| | `CreateGameSessionDS` | 데디케이티드 서버 세션 생성 |

세션 연결 정보(`GameSessionConnectionInfo`), 매치메이킹 티켓(`MatchmakingTicket`), 플레이어 속성(`PlayerAttribute`, latency map 포함) 등 GameLift API 응답 구조를 gRPC 메시지로 그대로 미러링해 클라이언트가 접속 정보를 바로 사용할 수 있도록 설계했습니다.

## 기술 스택

- **언어/프레임워크**: C# / ASP.NET Core (gRPC), .NET
- **클라우드**: AWS GameLift (FlexMatch, Fleets, Aliases)
- **인증**: JWT Bearer (EOS OpenID Connect, S2S OpenID Connect)
- **직렬화**: Protocol Buffers
- **기타**: gRPC Health Checks, Forwarded Headers(리버스 프록시 대응), Kestrel 타임아웃 커스터마이징

## 설계 포인트

- **비용 최적화**: 신규 매치메이킹(FlexMatch) 전에 빈 슬롯이 있는 기존 세션을 먼저 탐색해 불필요한 세션 생성을 줄임
- **리전 인지 라우팅**: 클라이언트가 보낸 리전별 레이턴시 값을 기준으로 가장 가까운 GameLift 리전/Alias를 자동 선택
- **이중 신뢰 경계**: 클라이언트(EOS)와 데디케이티드 서버(S2S)를 인증 스킴·정책 레벨에서 명확히 분리해, 서버 전용 API가 일반 유저 토큰으로 호출되지 않도록 강제
- **비동기 폴링 기반 상태 관리**: GameLift가 세션 배치/매치메이킹을 완료할 때까지 `Task.Delay` 기반 폴링으로 대기(현재는 SNS 미연동 상태이며, 코드 내 TODO로 추후 이벤트 기반 전환 계획 명시)

## 알려진 제한사항 / 개선 예정

- 매치메이킹/세션 배치 상태 확인이 폴링 방식이라 GameLift API 호출량이 많아질 수 있음 → SNS 기반 이벤트 알림으로 전환 예정 (코드 내 TODO 주석 참고)
- `GameDataId` 검색 시 필터 표현식(`AquaBSessionId` vs `GAMEDATAID`)에 대한 검증 필요 (TODO 표시)
