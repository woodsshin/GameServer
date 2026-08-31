# Seedworld — Online & Multiplayer Systems (Unreal Engine 5)

이 문서는 **Seedworld** 프로젝트에서 설계·구현한 온라인/멀티플레이어 백엔드 연동 모듈 중 포트폴리오용으로 발췌한 핵심 코드 샘플을 정리한 README입니다. AWS GameLift 기반 데디케이티드 서버 오케스트레이션, gRPC 기반 매치메이킹 클라이언트, 그리고 리플리케이션 기반 팀 시스템까지 — 클라이언트/서버 양쪽에서 동작하는 실시간 멀티플레이어 인프라를 다룹니다.

> 엔진: Unreal Engine 5 (C++) · 백엔드 연동: AWS GameLift, gRPC(TurboLink), EOS(Epic Online Services), AdvancedSessions Plugin

---

## 목차

1. [아키텍처 개요](#아키텍처-개요)
2. [모듈별 설명 및 코드 샘플](#모듈별-설명-및-코드-샘플)
   - [gRPC 서브시스템 — 서비스 디스커버리 & 토큰 관리](#1-grpc-서브시스템--서비스-디스커버리--토큰-관리)
   - [매치메이킹 콜백 프록시 — 비동기 gRPC 래퍼](#2-매치메이킹-콜백-프록시--비동기-grpc-래퍼)
   - [GameLift 게임모드 — 데디케이티드 서버 세션 오케스트레이션](#3-gamelift-게임모드--데디케이티드-서버-세션-오케스트레이션)
   - [GameLift 서버 오브젝트 — SDK 래핑 및 세션 상태 관리](#4-gamelift-서버-오브젝트--sdk-래핑-및-세션-상태-관리)
   - [온라인 게임모드 — 세션 생성/갱신](#5-온라인-게임모드--세션-생성갱신)
   - [BTK 팀 시스템 — 리플리케이션 기반 팀 관리](#6-btk-팀-시스템--리플리케이션-기반-팀-관리)
3. [설계 포인트 요약](#설계-포인트-요약)

---

## 아키텍처 개요

Seedworld는 두 가지 매치메이킹/서버 배치 경로를 동시에 지원하도록 설계되어 있습니다.

- **P2P / Advanced Sessions 경로**: `ASeedworldOnlineGameModeBase`가 `AdvancedSessionsLibrary`를 통해 온라인 서브시스템(EOS) 세션을 생성·갱신
- **AWS GameLift 경로**: `ASeedworldGameLiftGameMode`가 GameLift SDK(`USeedworldGameLiftServerObject`)와 자체 gRPC 매치메이킹 서비스(`StartMatchMakingDS`, `CreateGameSessionDS`)를 함께 사용해 데디케이티드 서버를 매치에 등록

두 경로 모두 아래 계층 구조를 공유합니다.

```
[Client/Server GameMode]
        │
        ▼
[Callback Proxy (UObject, BlueprintAsyncAction 패턴)]
        │
        ▼
[SeedworldGrpcSubsystem / SeedworldDSGrpcSubsystem]  ← 서비스 디스커버리 + 인증 토큰 관리
        │
        ▼
[TurboLink gRPC Client] → 매치메이킹 / 세션 관리 백엔드
```

클라이언트 전용(`USeedworldGrpcSubsystem`)과 데디케이티드 서버 전용(`USeedworldDSGrpcSubsystem`) 서브시스템은 `ShouldCreateSubsystem`으로 상호 배타적으로 생성되며, 둘 다 동일한 "서비스 디스커버리 → 인증 → gRPC 서비스 커넥트" 흐름을 따르되 인증 방식만 다릅니다(EOS ID 토큰 vs. OAuth client-credentials).

---

## 모듈별 설명 및 코드 샘플

### 1. gRPC 서브시스템 — 서비스 디스커버리 & 토큰 관리

**파일**: `SeedworldGrpcSubsystem.cpp`, `SeedworldDSGrpcSubsystem.cpp`

클라이언트/서버 각각의 게임 인스턴스 서브시스템으로, gRPC 엔드포인트를 런타임에 HTTP로 조회(서비스 디스커버리)하고, 요청받은 서비스들을 지연 커넥트하는 큐잉 구조를 갖습니다. 여러 곳에서 동시에 같은 서비스를 요청해도 `TMultiMap`으로 델리게이트를 모아뒀다가 서비스가 `Ready` 상태가 되는 순간 한 번에 콜백을 발사합니다.

```cpp
void USeedworldGrpcSubsystem::ConnectRequestedServices()
{
    UTurboLinkGrpcManager* TurboLinkManager = UTurboLinkGrpcUtilities::GetTurboLinkGrpcManager(GetWorld());
    auto TokenProviderFunc = [this]() { return GetAuthToken(); };
    TurboLinkManager->SetGlobalAccessTokenProvider(TokenProviderFunc);

    TArray<FString> RequestedServicesNames;
    RequestedServices.GetKeys(RequestedServicesNames);
    for (const FString& ServiceName : RequestedServicesNames)
    {
        if (Services.Find(ServiceName) != nullptr) continue; // 이미 커넥트 요청됨

        TObjectPtr<UGrpcService> RequestedServicePtr = TurboLinkManager->MakeService(ServiceName);
        RequestedServicePtr->SetAccessTokenProvider(TokenProviderFunc);
        Services.Add(ServiceName, RequestedServicePtr);
        RequestedServicePtr->Connect(); // non-blocking 신호

        if (RequestedServicePtr->GetServiceState() == EGrpcServiceState::Ready)
        {
            ProcessConnectedService(ServiceName, RequestedServicePtr);
        }
        RequestedServicePtr->OnServiceStateChanged.AddDynamic(this, &USeedworldGrpcSubsystem::OnServiceStateChanged);
    }
}

void USeedworldGrpcSubsystem::ProcessConnectedService(const FString& ServiceName, TObjectPtr<UGrpcService> Service)
{
    TArray<FRequestServiceDelegate> Delegates;
    RequestedServices.MultiFind(ServiceName, Delegates);
    for (const auto& Delegate : Delegates)
    {
        Delegate.ExecuteIfBound(Service); // 대기 중이던 모든 요청자에게 브로드캐스트
    }
    RequestedServices.Remove(ServiceName);
}
```

데디케이티드 서버 쪽(`SeedworldDSGrpcSubsystem`)은 OAuth 클라이언트 크리덴셜 방식으로 토큰을 주기적으로 갱신하며, 만료 60초 전에 자동 리프레시를 예약합니다.

```cpp
void USeedworldDSGrpcSubsystem::OnRefreshTokenResponseReceived(FHttpRequestPtr Request, FHttpResponsePtr Response, bool bWasSuccessful)
{
    constexpr float ErrorRefreshInterval = 5.0f;
    if (!bWasSuccessful) { ScheduleRefresh(ErrorRefreshInterval); return; }

    if (EHttpResponseCodes::IsOk(Response->GetResponseCode()))
    {
        // JSON 파싱 후 access_token 저장
        float NextRefreshInterval = DefaultRefreshTokenInterval;
        if (JsonObject->HasField(TEXT("expires_in")))
        {
            const int32 ExpireInSeconds = JsonObject->GetNumberField(TEXT("expires_in"));
            NextRefreshInterval = (float)ExpireInSeconds - 60.0f; // 만료 60초 전 선제 갱신
        }
        AccessToken = JsonObject->GetStringField(TEXT("access_token"));
        ScheduleRefresh(NextRefreshInterval);
        ConnectRequestedServices();
    }
    else
    {
        ScheduleRefresh(ErrorRefreshInterval); // 실패 시 5초 후 재시도
    }
}
```

---

### 2. 매치메이킹 콜백 프록시 — 비동기 gRPC 래퍼

**파일**: `StartMatchMakingCallbackProxy.cpp`, `CreatePlayerSessionCallbackProxy.cpp`, `CreateGameSessionCallbackProxy.cpp`, `CreateGameSessionDSCallbackProxy.cpp`, `StartMatchMakingDSCallbackProxy.cpp`

Unreal의 `UBlueprintAsyncActionBase` 패턴을 그대로 따르는 프록시 오브젝트들로, gRPC 비동기 호출을 `OnSuccess`/`OnFail` 멀티캐스트 델리게이트로 감쌉니다. 모든 프록시가 동일한 라이프사이클을 공유합니다 — 서비스 요청 → 클라이언트 생성 → RPC 컨텍스트 초기화 → 응답 바인딩 → 자원 해제(`BeginDestroy`에서 델리게이트 정리).

```cpp
void UStartMatchMakingCallbackProxy::OnGetMatchMakingSvc(UGrpcService* Service)
{
    MatchMakingSvc = Cast<UMatchMakingSvc>(Service);
    if (IsValid(MatchMakingSvc))
    {
        MatchMakingClient = MatchMakingSvc->MakeClient();
        HandleStartMatchMaking = MatchMakingClient->InitStartMatchMaking();
        MatchMakingClient->OnStartMatchMakingResponse.AddUniqueDynamic(this, &UStartMatchMakingCallbackProxy::OnStartMatchMakingResponse);

        FGrpcMatchMakingStartMatchMakingRequest Request;
        Request.PlayerId = ReqStartMatchMaking.PlayerId;
        Request.GameMode = ReqStartMatchMaking.GameMode;
        Request.LatencyInMs = ReqStartMatchMaking.LatencyInMs;
        MatchMakingClient->StartMatchMaking(HandleStartMatchMaking, Request);
    }
    else
    {
        OnFail.Broadcast({});
    }
}

void UStartMatchMakingCallbackProxy::OnStartMatchMakingResponse(FGrpcContextHandle Handle, const FGrpcResult& GrpcResult, const FGrpcMatchMakingStartMatchMakingReponse& GrpcResponse)
{
    ClearDelegate();
    if (GrpcResult.Code == EGrpcResultCode::Ok)
    {
        // 매칭 티켓에서 "나"에 해당하는 플레이어 세션만 골라냄
        for (const auto& PlayerSession : GrpcResponse.Ticket.GameSessionConnectionInfo.MatchedPlayerSessions)
        {
            if (ReqStartMatchMaking.PlayerId.Equals(PlayerSession->PlayerId))
            {
                FResStartMatchMaking Response;
                Response.PlayerSessionId = PlayerSession->PlayerSessionId;
                Response.IpAddress = GrpcResponse.Ticket.GameSessionConnectionInfo.IpAddress;
                Response.Port = GrpcResponse.Ticket.GameSessionConnectionInfo.Port;
                OnSuccess.Broadcast(Response);
                return;
            }
        }
    }
    OnFail.Broadcast({});
}
```

`WITH_EDITOR` 빌드에서는 실제 백엔드 호출 없이 즉시 `OnFail`을 발생시켜, 에디터 PIE 테스트 시 불필요한 네트워크 의존성을 차단하는 패턴이 모든 프록시에 일관되게 적용되어 있습니다.

```cpp
void UCreatePlayerSessionCallbackProxy::Activate()
{
    UGameInstance* GameInstance = ((UGameEngine*)GEngine)->GameInstance;
    if (!GameInstance) { OnFail.Broadcast({}); return; }
#if WITH_EDITOR
    OnFail.Broadcast({});
#else
    auto SeedworldGrpc = GameInstance->GetSubsystem<USeedworldGrpcSubsystem>();
    SeedworldGrpc->RequestService<UMatchMakingSvc>(
        FRequestServiceDelegate::CreateUObject(this, &UCreatePlayerSessionCallbackProxy::OnGetMatchMakingSvc));
#endif
}
```

---

### 3. GameLift 게임모드 — 데디케이티드 서버 세션 오케스트레이션

**파일**: `SeedworldGameLiftGameMode.cpp`

AWS GameLift와 통신하는 데디케이티드 서버의 게임모드. 서버 부팅 시 자체 매치메이킹 백엔드에 "이 서버를 매치에 등록해달라"고 폴링하고, 플레이어 접속 시 GameLift SDK로 세션 검증을 수행합니다.

**서버가 자신을 매치풀에 등록 — 성공할 때까지 반복 폴링:**

```cpp
void ASeedworldGameLiftGameMode::RequestCreateGameSession()
{
    FString Region;
    if (!FParse::Value(FCommandLine::Get(), TEXT("region="), Region, true)) return;

    // 서버 할당 전까지 일정 주기로 재시도
    GetWorld()->GetTimerManager().SetTimer(
        MatchMakingServivceConnectTimerHandle, this,
        &ASeedworldGameLiftGameMode::RequestCreateGameSession,
        MatchMakingDSWaitingTime, true);

    FGrpcMatchMakingCreateGameSessionDSRequest Request;
    Request.CreatorId = TEXT("Dedicated_Server");
    Request.MaximumPlayerSessionCount = MaxPlayers;
    Request.Region = Region;
    Request.GameProperties.Add(TEXT("maxplayers"), FString::Printf(TEXT("%d"), MaxPlayers));
    Request.GameProperties.Add(TEXT("region"), Region);

    UCreateGameSessionDSCallbackProxy* CreateGameSessionDS = UCreateGameSessionDSCallbackProxy::CreateGameSessionDS(Request);
    CreateGameSessionDS->OnSuccess.AddDynamic(this, &ASeedworldGameLiftGameMode::OnCreateGameSessionDSResponse);
    CreateGameSessionDS->OnFail.AddDynamic(this, &ASeedworldGameLiftGameMode::OnCreateGameSessionDSFailed);
    CreateGameSessionDS->Activate();
}
```

**접속 플레이어의 GameLift 세션 티켓 검증 (`PreLogin`에서 인증 게이트 역할):**

```cpp
void ASeedworldGameLiftGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
    Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
    if (!IsEnabledGameLift()) return;

    // 접속 URL: ip:port?playersessionid=psess-af9e4c27-...
    const FString LowerOptions = Options.ToLower();
    FString PlayerSessionId = UGameplayStatics::ParseOption(LowerOptions, TEXT("playersessionid"));

    EGameLiftErrorType ErrorType;
    if (SeedworldGameLiftServerObject->AcceptPlayerSession(PlayerSessionId, ErrorType, ErrorMessage))
    {
        PlayerSessions.Add(UniqueId, PlayerSessionId); // Logout 시 세션 해제용으로 보관
    }
    else
    {
        UE_LOG(SeedworldGameLiftGameModeLog, Error, TEXT("Failed to accept PlayerSession : ... ErrorType %d"), ErrorType);
    }
}
```

플레이어가 나갈 때는 `PlayerState`가 파괴되기 전에 `Logout`에서 먼저 GameLift 세션을 해제하도록 순서를 맞춘 것이 핵심입니다(`UniqueId`를 여전히 조회할 수 있는 시점에 처리).

---

### 4. GameLift 서버 오브젝트 — SDK 래핑 및 세션 상태 관리

**파일**: `SeedworldGameLiftServerObject.cpp`

GameLift 서버 SDK(`UGameLiftServerObject`)를 감싸는 얇은 래퍼로, "세션 시작 요청이 SDK 초기화보다 먼저 도착하는" 레이스 컨디션을 pending 플래그로 흡수합니다.

```cpp
bool USeedworldGameLiftServerObject::StartGameSession(const FServerGameSession& Session)
{
    bAssignedGame = true;
    CachedGameSession = Session;
    bool bSuccess = Super::StartGameSession(Session);
    OnCreateGameSessionlegate.ExecuteIfBound(Session);
    return bSuccess;
}

void USeedworldGameLiftServerObject::CompleteStartSession(bool bSuccess)
{
    if (bSuccess)
    {
        if (bHasPendingCreateGameSession)
        {
            bHasPendingCreateGameSession = false;
            Super::StartGameSession(CachedGameSession); // 지연된 세션 시작을 이제 처리
        }
    }
    else
    {
        DestroyGameLiftServer();
    }
}
```

---

### 5. 온라인 게임모드 — 세션 생성/갱신

**파일**: `SeedworldOnlineGameModeBase.cpp`

P2P/Advanced Sessions 경로의 상위 게임모드. `MaxPlayers`, 맵 이름, 빌드 버전, 리전, 퍼블릭 IP 등 20개 이상의 세션 메타데이터를 `FSessionPropertyKeyPair` 배열로 조립해 EOS 세션에 태그로 붙이는 것이 핵심 로직입니다. 세션 생성과 갱신(`CreateGameSession` / `UpdateGameSession`)이 동일한 속성 조립 로직을 공유하도록 설계되어 중복을 최소화했습니다.

```cpp
void ASeedworldOnlineGameModeBase::CreateGameSession()
{
    if (CreateSessionCallbackProxyAdvanced) return; // 이미 진행 중이면 재진입 방지

    TArray<FSessionPropertyKeyPair> ExtraSettings = GetExtraSessionSettings(); // GAMEMODE 등 파생 클래스별 속성
    FGameDataDescription GameDataDescription = SeedworldHelperSystem->GetGameDataDescription();

    ExtraSettings.Add(UAdvancedSessionsLibrary::MakeLiteralSessionPropertyString(TEXT("MAPNAME"), MapName));
    // ... BUILDVERSION, PUBLICIP, REGION, GAME_SESSION_ID 등 계속 추가 ...

    CreateSessionCallbackProxyAdvanced = UCreateSessionCallbackProxyAdvanced::CreateAdvancedSession(
        this, ExtraSettings, nullptr, ServerInformation.MaxPlayers, /*...*/);
    CreateSessionCallbackProxyAdvanced->OnSuccess.AddDynamic(this, &ASeedworldOnlineGameModeBase::OnCreateSessionSuccess);
    CreateSessionCallbackProxyAdvanced->OnFailure.AddDynamic(this, &ASeedworldOnlineGameModeBase::OnCreateSessionFailed);
    CreateSessionCallbackProxyAdvanced->Activate();
}
```

**퍼블릭 IP 조회 → EOS 로그인으로 이어지는 부팅 시퀀스:**

```cpp
void ASeedworldOnlineGameModeBase::InitGameServer()
{
    ServerInformation.BuildVersion = Version.Version.ToString();
    ServerInformation.ConnectionString.Port = 27115;

    FString PublicIP = UOnlineSubsystemSeedworldFunctionLibrary::GetPublicIP();
    if (PublicIP.IsEmpty())
    {
        RequestGetPublicIP(); // GetPublicIPCallbackProxy → checkip.amazonaws.com
    }
    else
    {
        SetPublicIP(PublicIP);
#if !WITH_EDITOR
        LoginEOS();
#endif
    }
}
```

퍼블릭 IP는 별도 프록시(`UGetPublicIPCallbackProxy`)가 `https://checkip.amazonaws.com/`을 호출해 조회하며, 캐시가 있으면 스킵하고 바로 로그인 단계로 진입합니다.

---

### 6. BTK 팀 시스템 — 리플리케이션 기반 팀 관리

**파일**: `SeedworldBTKGameStateBase.cpp`, `SeedworldBTKPlayerState.cpp`, `SeedworldBTKTeamSubsystem.cpp`

서버 권한(Server RPC) 기반 팀 생성/초대/역할 관리 시스템. `GameStateBase`가 팀 배열(`BTKTeams`)의 진실 공급원(source of truth) 역할을 하고, `PlayerState`는 자신의 팀 참조를 리플리케이트하며, 로컬 클라이언트 서브시스템(`USeedworldBTKTeamSubsystem`)이 UI에 이벤트를 브로드캐스트합니다.

**서버: 팀 생성 요청 검증 (중복 소속/중복 이름 체크 후 스폰):**

```cpp
void ASeedworldBTKGameStateBase::Server_CreateNewTeam_Implementation(const FString& BTKTeamsName, APlayerState* Creator)
{
    ASeedworldBTKPlayerState* BTKPlayerState = Cast<ASeedworldBTKPlayerState>(Creator);
    FString UniqueIDString = BTKPlayerState->GetUniqueId()->ToString();

    TObjectPtr<ABTKTeam> BTKTeam;
    if (FindTeamByUniqueID(UniqueIDString, BTKTeam, &MemberInfo))
    {
        BTKPlayerController->Client_CreateTeamResult(EBTKTeamResult::Already_Has_Team, nullptr);
        return;
    }
    if (FindTeamByTeamName(BTKTeamsName, BTKTeam))
    {
        BTKPlayerController->Client_CreateTeamResult(EBTKTeamResult::Existing_TeamName, nullptr);
        return;
    }

    ABTKTeam* NewBTKTeam = GetWorld()->SpawnActor<ABTKTeam>();
    NewBTKTeam->CreateTeam(BTKTeamsName, Creator);
    BTKTeams.Add(NewBTKTeam);
    OnRep_BTKTeams();
    ForceNetUpdate();

    BTKPlayerController->Client_CreateTeamResult(EBTKTeamResult::OK, NewBTKTeam);
}
```

**클라이언트: `OnRep_Team`에서 로컬 플레이어 여부를 판별해 UI 서브시스템에만 이벤트 전파** (다른 플레이어의 팀 변경으로 인한 오브캐스트 노이즈 방지):

```cpp
void ASeedworldBTKPlayerState::OnRep_Team()
{
    if (IsLocalPlayerState())
    {
        auto SeedworldBTKTeamSubsystem = GetWorld()->GetGameInstance()->GetSubsystem<USeedworldBTKTeamSubsystem>();
        if (Team)
            SeedworldBTKTeamSubsystem->OnTeamJoined.Broadcast(EBTKTeamResult::OK, Team);
        else
            SeedworldBTKTeamSubsystem->OnTeamLeft.Broadcast();
    }
}

bool ASeedworldBTKPlayerState::IsLocalPlayerState()
{
    APlayerController* LocalPlayerController = GetWorld()->GetFirstPlayerController();
    return LocalPlayerController && LocalPlayerController->PlayerState == this;
}
```

**클라이언트 서브시스템: 팀원 Pawn 조회 예시** (팀 인터페이스를 통한 느슨한 결합):

```cpp
TArray<APawn*> USeedworldBTKTeamSubsystem::GetTeamMemberPawns()
{
    TArray<APawn*> TeamPawns;
    ABTKTeam* MyTeam;
    if (!GetMyTeam(MyTeam) || !IsValid(MyTeam)) return TeamPawns;

    int32 LocalTeamID = MyTeam->GetTeamID();
    for (APlayerState* PlayerState : GetWorld()->GetGameState()->PlayerArray)
    {
        ASeedworldBTKPlayerState* OtherPlayerState = Cast<ASeedworldBTKPlayerState>(PlayerState);
        if (OtherPlayerState && ISeedworldBTKTeamInterface::Execute_GetTeamID(OtherPlayerState) == LocalTeamID)
        {
            if (APawn* PlayerPawn = PlayerState->GetPawn())
                TeamPawns.Add(PlayerPawn);
        }
    }
    return TeamPawns;
}
```

---

## 설계 포인트 요약

| 영역 | 설계 포인트 |
|---|---|
| **비동기 통신 패턴** | 모든 gRPC/HTTP 호출을 `Activate()` + `OnSuccess`/`OnFail` 델리게이트를 가진 `UObject` 프록시로 통일 → Blueprint 노출 및 재사용성 확보 |
| **서비스 디스커버리** | 엔드포인트를 하드코딩하지 않고 부팅 시 HTTP로 조회, 실패해도 프로젝트 설정의 기본값으로 폴백 |
| **인증 분리** | 클라이언트는 EOS ID 토큰, 데디케이티드 서버는 OAuth client-credentials — 만료 60초 전 자동 갱신 |
| **에디터 안전장치** | `WITH_EDITOR` 매크로로 PIE 테스트 시 실제 백엔드 호출을 원천 차단 |
| **레이스 컨디션 방어** | GameLift `StartGameSession`이 SDK 초기화보다 먼저 오는 경우 pending 플래그로 흡수 후 재생 |
| **재시도 전략** | 매치메이킹/세션 생성 실패 시 타이머 기반 폴링으로 자동 재시도 (`MatchMakingDSWaitingTime` 간격) |
| **네트워크 권한 분리** | 팀 시스템은 Server RPC로만 상태를 변경하고, `OnRep_*`를 통해 클라이언트가 결과만 반영 — 로컬 플레이어 여부 체크로 불필요한 브로드캐스트 방지 |
| **리소스 정리** | 모든 콜백 프록시가 `BeginDestroy()`에서 델리게이트를 명시적으로 해제해 댕글링 바인딩 방지 |

---

*본 문서는 실제 프로덕션 코드베이스에서 발췌한 스니펫으로 구성되어 있으며, 포트폴리오 열람 목적의 요약본입니다.*
