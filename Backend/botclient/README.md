# botclient

게임 백엔드 **부하 테스트(load test)**를 위한 Go 기반 시뮬레이션 클라이언트입니다. 실제 Unreal Engine 게임 클라이언트가 수행하는 로비 진입 → 인증 → gateway 연결 → RPC 시퀀스 실행 전 과정을 다수의 가상 봇으로 동시에 재현하여, `OnlineSubsystemIcarus`가 통신하는 백엔드 스택(Gateway, Lobby, Player Service 등) 전체를 end-to-end로 부하 검증합니다.

---

## Overview

`botclient`는 JSON으로 정의된 시나리오(scenario)를 읽어, 설정된 수만큼의 독립적인 가상 유저를 생성하고 각자 자체적인 상태 머신(state machine)으로 요청 시퀀스를 진행시킵니다. 각 봇은 실제 게임 클라이언트와 동일하게 로비 대기열 → JWT 인증 → WebSocket gateway 연결 → RPC 요청/응답 사이클이라는 전체 흐름을 그대로 밟습니다.

```
botrequest.json (시나리오 정의)
        │
        ▼
   Run() ── botcount개의 goroutine 생성
        │
        ▼
┌───────────────────────────────────────────────────┐
│  봇 1개당 lifecycle                                  │
│                                                     │
│  Lobby(STOMP) 연결 ──▶ ReqLobbyMessage ──▶ JWT 수신   │
│         │                                          │
│         ▼                                          │
│  Gateway(WebSocket) 연결 (jwttoken 헤더)              │
│         │                                          │
│         ▼                                          │
│  Scenario Requests 순차 실행                         │
│  (ReqIdx 증가, RecvAck로 다음 요청 gate)               │
│         │                                          │
│         ▼                                          │
│  반복(Repeat) 종료 → NeedReset → Lobby부터 재시작        │
└───────────────────────────────────────────────────┘
        │
        ▼
   중앙 tick loop (TickInSec 주기) — 모든 봇의 다음 요청 전송 트리거
```

---

## Architecture

### `main.go` / `botclient.go` — Bot Lifecycle & Orchestration
- **`Run()`**: 설정 로딩과 유효성 검증(botcount, scenario repeat) 후, `LobbyEnable` 설정에 따라 `createLobbyConnectors()` 또는 `createSockets()`로 봇 집단을 기동. 이후 `TickInSec` 기반 `time.Ticker`로 전체 봇의 다음 요청 전송을 중앙에서 스케줄링.
- **`lobbyConnector`**: 봇별로 독립적인 STOMP connection을 열어 `ReqLobbyMessage`를 전송하고 개인 릴레이 큐(userID 기준)를 구독. 로비로부터 성공 응답과 JWT를 수신하면 그 토큰으로 곧바로 gateway WebSocket 연결(`createSocket`)로 전환 — 실제 클라이언트의 로비 대기 → 게이트웨이 진입 흐름을 그대로 모사.
- **`generateRequest`**: 시나리오의 `EventName`을 실제 RPC request struct(`model.ReqXxx`)로 매핑하는 대규모 switch 문. 캐릭터 생성, 인벤토리 조회, 워크숍 아이템 구매, 드롭십 구성, Prospect(탐사) 생성/갱신/정산 등 게임의 핵심 도메인 이벤트를 폭넓게 커버.
- **`generateProspect`**: 매 반복마다 payload 크기를 누적 증가시키는 더미 바이너리 데이터를 생성하고 SHA-1 해시 계산, zlib 압축, base64 인코딩까지 거쳐 `ReqUpdateProspect`를 구성 — 실제 세이브 데이터 동기화 트래픽의 크기·빈도 특성을 재현하기 위한 합성 부하(synthetic load) 생성 로직.
- **`proceedNextMessage`**: 매 tick마다 모든 봇을 순회하며 상태를 전진시키는 핵심 스케줄러.
  - `RecvAck`(이전 요청 응답 수신 여부)와 `ReqIdx`(현재 진행 인덱스)를 확인하여, 응답을 받은 봇만 다음 요청을 전송 — **요청 오버랩 없이 gateway 응답 속도에 맞춰 자연스럽게 스로틀링**되는 구조.
  - 각 이벤트에 설정된 `Delay`를 경과 시간과 비교해 지연 후 발송을 구현.
  - 시나리오를 모두 소진하면 `Repeat` 카운트에 따라 처음부터 재시작하거나, 완전히 종료 시 `NeedReset` 플래그로 로비 재접속(새 세션 시뮬레이션)을 트리거.
  - 활성 요청이 없는 유휴 상태에서는 20초 간격 `ReqPing`으로 연결 유지.
- **`ReadMessage`**: WebSocket 수신 루프. 연결 종료 감지 시 자동으로 `reSetConnection`을 호출해 세션을 처음부터(로비 또는 직접 연결) 재기동 — 장시간 실행되는 부하 테스트가 개별 연결 끊김에도 중단 없이 지속되도록 보장.

### `handler.go` / `handler_custom.go` — Response Dispatch
- Event name 기반 handler registry 패턴으로, `ResUserTicket`, `ResPong`, `ResTokenIssued`/`Expired`/`Invalid` 등 인증·연결 관련 핵심 응답을 처리.
- **`OnResTokenExpired`/`Invalid`/`NotSupplied`**: 토큰 문제 발생 시 `resetConnection`으로 해당 봇을 초기화 상태로 되돌려 다음 tick에 새 세션을 시작하도록 유도.

### `handler_gen.go` / `handler_impl.go` / `model_gen.go` — Generated RPC Surface
- 게임 백엔드가 노출하는 거의 모든 RPC(`ReqGetCharacters`, `ReqGetMetaInventory`, `ReqCreateDropship`, `ReqSelectEnvirosuit`, `ReqUpdateProspect` 등)에 대응하는 응답 handler와 struct 정의를 코드 생성기로 대량 생성. 각 handler는 공통적으로 `RecvAck`/`ReqIdx`를 갱신하여 `proceedNextMessage`의 상태 머신이 다음 요청으로 전진할 수 있게 함.
- **`PrintLog` + Slow-request logging**: 요청 전송 시각(`LastSent`)부터 응답 수신까지의 경과 시간을 측정하여, `SlowlogTime` 임계값을 초과하는 요청만 선별 로깅 — 수백~수천 개 동시 요청 환경에서 병목이 되는 RPC를 식별하기 위한 부하 테스트 전용 계측(instrumentation).

### `compress.go` — Payload Compression
`Zip`/`Unzip`/`ZipSkipMarshal`로 zlib 압축을 래핑. `generateProspect`가 생성하는 대용량 Prospect blob 페이로드에 사용되어, 실제 세이브 데이터 전송 시의 압축 오버헤드까지 부하 테스트에 포함.

### `botrequest.go` — Scenario Configuration Model
- **`BotSettings`**: gateway 주소, 로비 접속 정보, 봇 개수(`Botcount`), 계정 접두사/인덱스, 반복 횟수, tick 주기, slow-log 임계값 등 실행 전체를 제어하는 설정 스키마.
- **`BotScenario`** / **`BotEvent`**: 가중치 기반(`Weight`) 랜덤 시나리오 선택(`GetRandomScenario`)을 지원하여, 서로 다른 행동 패턴(예: 여러 종류의 Prospect 플레이 루틴)을 확률적으로 혼합한 현실적인 트래픽 프로파일을 구성 가능.
- **`BotProfile`**: 봇 1개당 유지되는 전체 상태(인증 토큰, 현재 캐릭터/인벤토리/드롭십/Prospect 정보, 요청 진행 인덱스, 마지막 전송/핑 시각 등)를 담는 in-memory 세션 struct.
- **`GetBotRequest`**: `viper` 기반 설정 로딩과 command-line flag(`-botcount`, `-gatewayaddress` 등)를 결합하여, 파일 기본값을 CLI 인자로 override할 수 있는 유연한 실행 옵션 제공.

### `botrequest.json` — Scenario Definition
실행할 시나리오를 선언적으로 정의하는 데이터 파일. `prerequests`(공통 초기화), `scenarios`(가중치 기반 선택 대상 시나리오 목록, 각 이벤트별 개별 `repeat`/`delay` 지정), `postrequests`로 구성되어, **코드 변경 없이 부하 패턴을 조정**할 수 있도록 설계.

---

## Notable Engineering Details

- **응답 기반 self-throttling 스케줄러**: 고정 요청 속도(fixed-rate)로 무작정 요청을 쏟아내는 대신, `RecvAck` 상태를 게이트로 사용해 각 봇이 자신의 이전 요청이 실제로 응답받은 뒤에만 다음 요청을 진행 — 이는 부하 테스트 도구 자신이 병목이 되어 백엔드의 실제 한계보다 낮은 처리량에서 잘못된 결론을 내리는 흔한 함정을 피하는 설계.
- **로비-게이트웨이 이원 connection 재현**: 실제 클라이언트가 STOMP 기반 로비 대기열과 WebSocket 기반 gateway라는 서로 다른 프로토콜의 두 connection을 순차적으로 사용하는 구조를, 부하 테스트 도구에서도 동일하게 재현 — 프로덕션과 이질적인 단축 경로(shortcut)를 통한 부정확한 부하 시뮬레이션을 방지.
- **누적 증가 payload를 통한 현실적 부하 프로파일링**: `generateProspect`가 매 갱신마다 데이터를 계속 추가해나가는 방식은, 실제 플레이 세션이 진행될수록 세이브 데이터가 커지는 게임의 실제 특성을 반영하여, 초기 부하와 장시간 세션 후반부 부하의 차이를 함께 검증 가능하게 함.
- **가중치 기반 시나리오 믹싱**: 단일 고정 스크립트 반복이 아닌 `Weight` 기반 확률적 시나리오 선택으로, 실제 유저 집단의 행동 다양성에 가까운 트래픽 패턴을 소규모 설정 변경만으로 생성.
- **장애 격리와 자동 복구**: 개별 봇의 연결 끊김, 토큰 만료, gateway 오류가 전체 부하 테스트 프로세스를 중단시키지 않고 해당 봇만 독립적으로 재시작되도록 설계되어, 수백~수천 개 동시 세션 환경에서도 안정적으로 장시간 실행 가능.

---

## Tech Stack

- **Language**: Go
- **Transport**: WebSocket (`gorilla/websocket`, gateway 연결), STOMP over RabbitMQ (`go-stomp/stomp`, 로비 연결)
- **Configuration**: `spf13/viper` (JSON 시나리오 파일 + command-line flag override)
- **Serialization**: `encoding/json`, 커스텀 byte-level frame codec, zlib 압축(`compress/zlib`)
- **Concurrency**: goroutine 기반 봇별 독립 실행, `sync.RWMutex`로 보호되는 공유 상태(소켓 맵, 로비 connection 맵)
