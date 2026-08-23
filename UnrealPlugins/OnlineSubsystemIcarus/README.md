# OnlineSubsystemIcarus

**Unreal Engine Online Subsystem (OSS)** 기반으로 제작된 커스텀 게임 백엔드 연동 plugin입니다. **WebSocket(STOMP 유사 framing)** 과 **RabbitMQ/STOMP 로비 messaging**을 통해 UE의 `IOnlineSubsystem` 추상화 계층에 연결되며, Identity, Session/Matchmaking, User Cloud Storage, Lobby Queue 서비스를 자체 protocol 기반으로 제공합니다.

---

## Overview

`OnlineSubsystemIcarus`는 표준 Unreal OSS plugin 패턴(`FOnlineSubsystemIcarus : public FOnlineSubsystemIcarusGen`)을 따르지만, 실질적인 engineering 포인트는 다음 두 영역에 집중되어 있습니다.

1. **비동기 WebSocket RPC framework** (`IcarusWSFrame` / `IcarusConnectionComponentBase`) — Raw WebSocket transport 위에 요청/응답 semantics, timeout tracking, reconnect/backoff 로직을 layering.
2. **STOMP 기반 병렬 Lobby/Queue client** (`IcarusLobbyConnectionComponentBase`) — RabbitMQ를 통한 matchmaking 대기열 상태 업데이트를 담당하며, 메인 gateway connection과 완전히 분리되어 동작.

서브시스템은 표준 UE interface(`IOnlineIdentity`, `IOnlineSession`, `IOnlineUserCloud`, `IOnlineUser`)를 노출하지만, 내부적으로는 모든 백엔드 통신이 이 두 connection component를 통해 routing됩니다.

---

## Architecture

```
FOnlineSubsystemIcarus (FOnlineSubsystemIcarusGen)
│
├── FOnlineIdentityInterfaceIcarus     — 로그인/로그아웃, 계정/세션 토큰 lifecycle
├── FOnlineSessionIcarus               — matchmaking, 호스트 migration, connection string relay
├── FOnlineUserCloudIcarus             — zlib 압축 + SHA1 해시 기반 세이브 데이터 업/다운로드
├── FOnlineUserInterfaceIcarus         — 캐시된 온라인 유저 조회
├── FOnlineProfileIcarus               — 캐릭터 슬롯 유효성 검증 layer
├── FOnlineLobbyIcarus                 — 로비 RPC dispatch, JWT 토큰 갱신
│
├── UIcarusConnectionComponent (UIcarusConnectionComponentBase)
│   ├── WebSocket transport (ws/wss) — WebSocketsModule 기반
│   ├── FIcarusWSFrame — 커스텀 wire protocol (프레임 encoding/decoding)
│   ├── FIcarusConnectionPingManager — heartbeat/prospect keep-alive 전담 worker thread (FRunnable)
│   └── 프레임 기반 요청/응답 dispatch table (FrameHandler / FrameCommandPairs)
│
└── UIcarusLobbyConnectionComponent (UIcarusLobbyConnectionComponentBase)
    ├── STOMP client (RabbitMQ) — StompModule 기반
    ├── FLobbyWSFrame — IStompMessage를 공용 FIcarusWSFrame body 포맷으로 wrapping하는 adapter
    └── 대기열 위치 ETA 추정 (CalculateTimeLeft)
```

---

## Core Components

### `FIcarusWSFrame` — Wire Protocol
STOMP에서 착안한 자체 프레임 format(`COMMAND\nheader:value\n\nBODY`)을 수동 byte buffer parsing(`ReadValue`, `SkipNewlines`, 구분자 인식 escaping)으로 직접 구현했습니다.

- STOMP spec에 준하는 헤더 escape encoding(`\`, `:`, `\n`, `\r`)을 지원하며, 레거시 호환을 위해 `CONNECT` command는 예외 처리.
- Thread-safe(`FCriticalSection`으로 보호)하게 순차적으로 증가하는 `FrameIndex`를 사용해 비동기 요청과 응답을 correlate시킴.
- Heartbeat 프레임(`IcarusHeartbeatCommand`)은 단일 `\n`으로 encoding.
- 모든 요청 프레임에 JWT Bearer 토큰(`WS_HEADER_JWT_TOKEN`)을 자동 주입.
- 로컬/싱글플레이어 fallback 경로를 위한 오프라인 모드 프레임 생성 지원.

### `UIcarusConnectionComponentBase` — Transport & RPC Layer
`IWebSocket` lifecycle을 소유하며 그 위에 요청/응답 프로토콜을 구현합니다.

- **Command→Response pairing**(`FrameCommandPairs`)을 통해 응답 없는 요청에 대한 자동 timeout 처리를 구현. 미응답 요청은 만료 timestamp와 함께 `ReqFrameBuffers`에 저장되고 `FTicker`로 0.2초마다 polling됨.
- **Send-with-retry**: `WriteFrameImpl`은 전송 실패 시 1초 간격으로 최대 60초까지 재시도한 뒤, 실제 서버 응답과 동일한 handler 경로로 "Backend connection lost" 실패 응답을 synthesize함 — 호출부에서 네트워크 장애와 실제 RPC 에러를 구분할 필요가 없도록 설계.
- **지수 backoff 재연결**(`Reconnect()`) — `MaxReconnectTime`으로 상한이 걸려 있으며 전용 `FTicker` delegate로 구동.
- **JWT 토큰 무효화 처리** — `ResTokenExpired` / `ResTokenNotSupplied` / `ResTokenInvalid`는 모두 `InvalidConnectionToken()`으로 수렴되어 캐시된 토큰을 폐기하고 clean 재연결을 강제함.

### `FIcarusConnectionPingManager` — Heartbeat Thread
게임 thread와 분리된 전용 `FRunnable` worker thread입니다.

- 부모 connection이 살아있는 동안 일정 주기로 `ReqPing`을 전송.
- 선택적으로 "prospect"(백엔드 상의 임시/예약 entity)를 `UpdateProspect()`로 heartbeat 처리하며, `FScopeLock`으로 보호되는 timer를 통해 thread 종료 없이 런타임 중 ON/OFF 전환 가능(`SetHeartbeatProspect` / `ClearHeartbeatProspect`).

### `UIcarusLobbyConnectionComponentBase` — Queue/Lobby Messaging
Matchmaking 대기열 telemetry를 위한, 메인 connection과 독립적인 두 번째 STOMP-over-RabbitMQ connection입니다.

- 플레이어별 relay queue(`/queue/{playerId}`)와 broadcast topic(`/topic/notice`)을 구독하며, 두 구독이 모두 완료된 상태(`bSubscribedRelayTo && bSubscribedTopic`)를 확인한 뒤 `OnLobbyConnect`를 trigger.
- `UIcarusLobbyConnectionComponent::CalculateTimeLeft`는 **rolling average 기반 처리율 estimator**(최근 10개 sample)를 구현하여 큐 소진 속도의 노이즈를 완화하고, 현재 큐 깊이와 경과 시간으로부터 ETA를 산출.
- 메인 gateway connection에서 캐시된 JWT(`FIcarusWSFrame::Token`)를 재사용하여 별도 재로그인 없이 인증을 처리, 추가 로그인 round-trip을 회피.

### `IcarusMessageListeners` — Event Fan-out
저수준 connection component delegate(`OnConnect`, `OnMatchUpdate`, `OnChatMessage` 등)를 게임 layer용 multicast delegate로 다시 broadcast하는 UObject 기반의 얇은 pub/sub bridge로, 게임플레이 코드를 OSS interface 내부 구현으로부터 분리시킵니다.

---

## Notable Engineering Details

- **모드 전환 시 interface hot-swap**: `SwitchOnlineMode()`는 `UIcarusConnectionComponent`(온라인)와 `UIcarusOfflineConnectionComponentGen`(오프라인) 사이를 전환하며, `Identity`, `Session`, `UserCloud`, `MessageListeners` 등 종속된 모든 interface의 callback을 다시 bind — 서브시스템 재초기화 없이 온라인/오프라인 fallback을 매끄럽게 지원.
- **통합된 실패 처리 경로**: timeout 만료와 복구 불가능한 전송 실패 모두 `FIcarusWSFrame::SetOfflineModeCommand`를 통해 응답 프레임을 synthesize한 뒤, 실제 서버 응답이 사용하는 것과 *동일한* `FrameHandler` table로 dispatch — RPC 호출부는 원인과 무관하게 단일한 실패 처리 코드 경로만 다루면 됨.
- **파일 업로드 pipeline** (`FOnlineUserCloudIcarus::WriteUserFile`): 무결성 검증을 위한 SHA-1 hashing, zlib 압축(`FCompression::CompressMemory`), 그리고 헤더에 압축/비압축 길이·hash·progress key 등의 metadata를 함께 실어 단일 요청으로 처리.
- **Thread 안전성 경계**: ping manager는 자체 thread에서 동작하며 공유 상태(`ProspectID`, `UpdateProspectPingTimer`)를 `FScopeLock`으로 보호. WebSocket I/O 및 UObject delegate broadcast는 `FTicker`를 통해 게임 thread에 유지.

---

## Tech Stack

- **Engine**: Unreal Engine (Online Subsystem plugin architecture)
- **Transport**: WebSockets (`WebSocketsModule`), STOMP over RabbitMQ (`StompModule`)
- **Serialization**: `FJsonObjectConverter` (UStruct ⇄ JSON)
- **Concurrency**: `FRunnable`/`FRunnableThread`, `FTicker`, `FCriticalSection`/`FScopeLock`
- **Auth**: JWT Bearer 토큰, 플랫폼 native 인증(Steam/EOS)을 upgrade header로 전달
