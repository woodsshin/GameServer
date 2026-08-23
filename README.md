# Portfolio

Network Programming과 Game Engine Middleware를 중심으로 한 프로젝트 모음입니다. 각 프로젝트는 독립된 Repository처럼 구성되어 있으며, 하단 링크에서 세부 구현과 검증 결과를 확인할 수 있습니다.

**GameServerSample**은 Unreal Engine과 무관하게 Claude Code를 활용해 작성한 순수 C++ Game Server 예제이고, **UnrealPlugins** 하위의 세 프로젝트(OnlineSubsystemEOS, OnlineSubsystemIcarus, SimpleUPNP)는 모두 직접 구현한 Unreal Engine Dedicated Server(및 Client)에서 동작하는 Native Code Plugin/Module입니다. **Backend**의 경우도 직접 개발한 microservice를 중심으로 설명하였습니다. 공동으로 개발한 microservice는 포함하지 않았습니다.

| Project | 요약 | Stack |
|---|---|---|
| [GameServerSample](./GameServerSample/README.md) | epoll 기반 비동기 Game Server. DB Connection Pool과 Thread 분리 Architecture로 대규모 동시 접속을 처리 | C++, epoll, MySQL, pthread |
| [UnrealPlugins/OnlineSubsystemIcarus](./UnrealPlugins/OnlineSubsystemIcarus/README.md) | 자체 Game Backend(Icarus)를 UE `OnlineSubsystem` Interface로 연동하는 Native Code Plugin. WebSocket RPC와 STOMP 기반 Lobby Messaging을 직접 구현 | Unreal Engine, C++, WebSocket, STOMP |
| [Backend](./Backend/README.md) | Icarus 게임 백엔드를 구성하는 Go 기반 microservice 모음. RabbitMQ를 공용 message bus로 사용하는 독립 배포 구조 | Go, RabbitMQ(AMQP/STOMP), Kubernetes, Redis, MySQL |
| [UnrealPlugins/OnlineSubsystemEOS](./UnrealPlugins/OnlineSubsystemEOS/README.md) | Epic Online Services를 Unreal Engine의 표준 `OnlineSubsystem` Interface로 wrapping한 Native Code Plugin | Unreal Engine, C++, EOS SDK |
| [UnrealPlugins/SimpleUPNP](./UnrealPlugins/SimpleUPNP/README.md) | UPnP IGD Protocol로 Router에 Port Forwarding을 자동 등록하는 Native Code Plugin | Unreal Engine, C++, SSDP/SOAP |

---

## 관점 — 왜 이 다섯 프로젝트인가

다섯 프로젝트는 모두 **Client 간 통신 경로를 어떻게 확보할 것인가**라는 동일한 문제를 서로 다른 계층에서 다룹니다.

```
                    ┌─────────────────────────────────────────────────────────┐
                    │                Multiplayer Session 성립                  │
                    └─────────────────────────────────────────────────────────┘
                                      │
        ┌─────────────────┬──────────┴──────────┬─────────────────────┐
        ▼                 ▼                     ▼                     ▼
  Server Infrastructure  Custom Backend    Platform Backend      Network Transport
  (자체 구현)              Integration      Integration            (NAT Traversal)
        │             (자체 Protocol)     (Epic Service 통합)          │
        │              ┌────┴────┐              │                     │
        ▼              ▼         ▼              ▼                     ▼
GameServerSample    Backend  OnlineSubsystem OnlineSubsystemEOS   SimpleUPNP
epoll 비동기 I/O,    Go        Icarus         EOS SDK를 UE         UPnP IGD로 Router에
DB 연동,            Microservice, WebSocket RPC +  OnlineSubsystem으로  Port Mapping을 자동
동시성 제어          RabbitMQ    STOMP Lobby      wrapping,           등록, Relay 서버
                    기반         Client(UE       EOS P2P NAT         없이 P2P 성립
                    메시지 버스   Plugin)         Traversal 통합
```

- **GameServerSample**은 Unreal Engine과 독립된 순수 C++ Standalone Game Server 예제로, Claude Code를 활용해 작성했습니다. Server Side에서 대규모 동시 접속을 어떻게 비동기적으로, 안전하게 처리할지에 대한 답이며, Custom Protocol과 Custom Infrastructure를 직접 설계했습니다.
- **Backend**와 **OnlineSubsystemIcarus**는 한 쌍의 서버/클라이언트 구현입니다. Backend는 여러 독립 Go microservice를 RabbitMQ 메시지 버스로 결합한 서버 측이고, OnlineSubsystemIcarus는 그 Backend와 WebSocket(자체 프레임 Protocol) + STOMP Lobby Messaging으로 통신하는, Unreal Engine Dedicated Server/Client에 탑재되는 Plugin입니다. Wire Protocol을 서버·클라이언트 양쪽에 직접 설계·구현했다는 점에서, 표준 Platform SDK에 의존하지 않는 완전한 Custom Backend 통합 사례입니다.
- **OnlineSubsystemEOS**는 Unreal Engine Dedicated Server/Client에 탑재되어, Unreal Engine 생태계 안에서 Epic의 Backend Service(인증, Session, matchmaking, P2P)를 엔진 표준 Interface로 통합하는 Middleware Layer입니다. EOS 자체의 P2P NAT Traversal과 Relay Fallback을 활용합니다.
- **SimpleUPNP**는 Unreal Engine Client와 Dedicated Server 양쪽에서 모두 사용 가능한 Plugin으로, Backend Service 없이 순수 Protocol(UPnP)만으로 실행 중인 PC의 Router에 직접 Port를 열어, Relay Server 없는 완전한 P2P 경로를 확보하는 더 근본적인(low-level) 해법입니다.

다섯 프로젝트를 통해 Custom Server Infrastructure 설계 역량, 분산 Backend Service 및 그에 대응하는 Client-side Protocol 구현 역량, Game Engine Middleware/Network Protocol Level의 문제 해결 역량을 함께 보이는 것을 목표로 했습니다.

---

## GameServerSample

Unreal Engine과 무관하게 Claude Code를 활용해 작성한 순수 C++ Standalone Game Server 예제입니다. epoll 기반 비동기 I/O, DB Connection Pool, Thread 분리 Architecture로 구현했으며, 계정 생성, 로그인, Chat Broadcast, 대규모 동시 접속 처리를 지원합니다.

**핵심 설계**
- **Accept Thread / IoWorker Thread 분리**: Accept 전용 Thread가 신규 연결을 전담하고, 각 IoWorker는 독립된 epoll Instance를 보유해 CPU Core 수에 비례한 수평 확장이 가능합니다.
- **DB 비동기 처리**: Blocking 방식의 MySQL Query를 IoWorker Thread에서 직접 호출하면 Head-of-Line Blocking이 발생하므로, DBTask를 Queue에 Enqueue하고 별도의 DB Worker Pool이 처리하는 Pipeline을 구성했습니다.
- **길이 기반 Binary Protocol**: `[4B TotalSize][2B PacketType][Body]` 구조로 TCP Stream의 부분 수신/결합 수신을 처리합니다.
- **동시성 제어**: `shared_mutex`로 읽기 위주 연산(Broadcast)의 동시 진입을 허용하고, Snapshot 후 즉시 Lock을 해제하는 패턴으로 Lock 보유 시간을 최소화했습니다.
- **보안**: salt + SHA-256 Stretching(10,000 round) 기반 비밀번호 해싱, Prepared Statement로 SQL Injection 차단.

**실측 검증**: 자체 제작 비동기 Load Test 도구로 최대 1,000명 동시 접속을 실측했습니다. 접속·Broadcast 계층은 병목 없이 100% 성공률을 유지했으나, 회원가입/로그인 처리량이 27.5 req/sec로 고정되는 현상을 발견하고 원인이 SHA-256 Stretching에 따른 CPU 포화임을 대조군 테스트로 교차 검증했습니다(Stretching Round 축소 시 성공률 92.3% → 100%, 지연 p50 약 14배 개선). 방법론과 전체 수치는 하위 [`report/README.md`](./GameServerSample/report/README.md)에 정리되어 있습니다.

→ 자세한 내용은 [GameServerSample/README.md](./GameServerSample/README.md) 참고.

---

## Backend

Icarus 게임 백엔드를 구성하는 Go 기반 microservice 모음입니다. 각 서비스는 독립적으로 배포되며 RabbitMQ(AMQP/STOMP)를 공용 message bus로 사용해 서로 통신합니다. Unreal Engine Client 측 `OnlineSubsystemIcarus`가 이 서비스들과 WebSocket/STOMP wire protocol로 직접 연동됩니다.

**구성 서비스**
- **adminproxy**: HTTP Request를 RabbitMQ 기반 RPC로 변환해 Player Service, Session Manager, Gateway로 라우팅하는 관리자용 Gateway. `frameIdx` 기반 `context.Context` Correlation으로 RabbitMQ의 Pub/Sub 모델과 HTTP의 동기 모델을 연결합니다.
- **botclient**: 로비 진입 → JWT 인증 → Gateway 연결 → RPC 시퀀스 실행까지 실제 Client와 동일한 흐름을 다수의 가상 Bot으로 재현하는 부하 테스트 도구. `RecvAck` 기반 Self-throttling Scheduler로 도구 자신이 병목이 되지 않도록 설계했습니다.
- **lobbystats**: RabbitMQ Management API를 Polling해 매치메이킹 대기열 통계를 산출하고, STOMP로 Client에 실시간 Broadcast하는 소규모 서비스입니다.

**핵심 설계**
- **RabbitMQ 중심 결합 구조**: 모든 서비스는 서로 직접 호출하지 않고 Exchange/Queue 바인딩으로만 결합되어, 개별 배포·재시작·장애가 다른 서비스에 직접적인 컴파일/런타임 의존성을 만들지 않습니다.
- **Kubernetes(Azure) 배포**: `Gateway`, `SessionManager`, `PlayerService`, `Scheduler`, `AdminProxy`가 RabbitMQ를 중심으로 연결되며, Redis(SessionManager)와 MySQL(PlayerService/Scheduler)을 Data Store로 사용합니다.
- **Connection Resilience**: `adminproxy`, `lobbystats`, `botclient` 모두 Connection 종료를 감지해 자동 재연결하는 Supervisor 패턴을 독립적으로 구현하고 있으며, 재연결 시 Exchange/Queue 상태를 함께 재구성합니다.
- **커스텀 Wire Protocol의 다중 언어 재구현**: Unreal Client의 `FIcarusWSFrame`(C++)과 동일한 Frame Format(`EventName\nheader:value\n\nBody`)이 `lobbystats`, `botclient`에 각각 독립적으로 재구현되어 있으며, 이 Protocol-level 계약이 서로 다른 언어·Repository 간 상호운용성의 유일한 접점입니다.

→ 자세한 내용은 [Backend/README_backend.md](./Backend/README_backend.md) 참고.

---

## UnrealPlugins/OnlineSubsystemIcarus

Unreal Engine Dedicated Server와 Client 양쪽에 탑재되어 동작하는 Native Code Plugin(Online Subsystem, OSS)입니다. 위 Backend와 직접 짝을 이루며, WebSocket(STOMP 유사 Framing)과 RabbitMQ/STOMP Lobby Messaging을 통해 UE의 `IOnlineSubsystem` 추상화 계층에 연결되며, Identity, Session/Matchmaking, User Cloud Storage, Lobby Queue 서비스를 자체 Protocol 기반으로 제공합니다.

**핵심 설계**
- **`FIcarusWSFrame` 자체 Wire Protocol**: STOMP에서 착안한 `COMMAND\nheader:value\n\nBODY` 프레임 포맷을 수동 Byte Buffer Parsing으로 직접 구현했습니다. Thread-safe하게 순차적으로 증가하는 `FrameIndex`로 비동기 요청/응답을 Correlate시키고, 모든 요청에 JWT Bearer 토큰을 설정합니다.
- **비동기 WebSocket RPC Layer**(`UIcarusConnectionComponentBase`): Command→Response Pairing으로 미응답 요청의 자동 Timeout을 처리하고, 전송 실패 시 재시도 후 실제 서버 응답과 동일한 Handler 경로로 실패 응답을 합성하여 — 호출부가 네트워크 장애와 RPC 에러를 구분할 필요가 없도록 설계했습니다. Exponential Backoff 기반 자동 재연결도 포함됩니다.
- **전용 Heartbeat Thread**(`FIcarusConnectionPingManager`): 게임 Thread와 분리된 `FRunnable` Worker Thread가 주기적 Ping과 Prospect Heartbeat를 전담합니다.
- **STOMP 기반 병렬 Lobby Client**(`UIcarusLobbyConnectionComponentBase`): 메인 Gateway Connection과 완전히 분리된 두 번째 STOMP-over-RabbitMQ 연결로 매치메이킹 대기열을 구독하며, 최근 10개 Sample 기반 Rolling Average로 대기열 소진 속도의 노이즈를 완화해 ETA를 산출합니다.
- **모드 전환 시 Interface Hot-swap**: 온라인/오프라인 Connection Component 사이를 서브시스템 재초기화 없이 전환하며, 종속된 모든 Interface의 Callback을 다시 bind합니다.

Backend가 서버 측 RabbitMQ Microservice 군이라면, OnlineSubsystemIcarus는 그 Backend와 통신하는 Wire Protocol을 Client 측에서 동일하게 재구현한 대응 짝입니다.

→ 자세한 내용은 [UnrealPlugins/OnlineSubsystemIcarus/README.md](./UnrealPlugins/OnlineSubsystemIcarus/README.md) 참고.

---

## UnrealPlugins/OnlineSubsystemEOS

Unreal Engine Dedicated Server와 Client 양쪽에 탑재되어 동작하는 Native Code Plugin으로, Epic Online Services(EOS)를 UE의 표준 `OnlineSubsystem` Interface로 wrapping합니다. `DefaultPlatformService=EOS` 설정 하나로 기존에 다른 Platform을 대상으로 구현된 온라인 로직(로그인, Session/matchmaking, 업적, 친구 등)을 코드 수정 없이 EOS로 교체할 수 있습니다.

**핵심 설계**
- **EOS SDK 초기화**: Module Load 시점에 Platform별 EOS SDK Binary를 동적 Load하고, `OnlineSubsystem` Module에 Factory를 등록합니다.
- **EOS P2P 기반 Network Layer**: UE의 `Sockets`/`SocketSubsystem`/`NetDriver`/`NetConnection` 계층을 EOS Version으로 구현했습니다. `FInternetAddrEOS`는 IP 대신 EOS ID + Channel을 담는 주소 체계이며, `UEOSNetDriver`는 접속 URL의 `eos.` Prefix 여부로 EOS 경로와 일반 IP 경로(passthrough)를 자동 분기합니다.
- **NAT Traversal & Relay Fallback**: `FSocketEOS::SendTo`/`RecvFrom`은 `EOS_P2P_SendPacket`/`ReceivePacket`을 호출하며, NAT Traversal이 실패하는 환경에서는 `bAllowP2PPacketRelay` 설정에 따라 EOS Relay Server로 자동 Fallback합니다.
- **연결 수명 관리**: `FSocketSubsystemEOS`가 살아있는 연결과 종료 대기 중인 연결(Dead Connection)을 분리 관리하고, Linger Timeout을 둬서 재접속 경쟁 상태(Race Condition)로 인한 오작동을 방지합니다.
- **Session ↔ P2P 주소 결합**: EOS Sessions가 반환하는 `P2PADDRESS`(`eos.<EOSID>:<Channel>` 형식)를 그대로 `ClientTravel` URL로 사용하면 위 Network Layer를 통해 NAT Traversal P2P 접속이 성립합니다.

→ 자세한 내용은 [UnrealPlugins/OnlineSubsystemEOS/README.md](./UnrealPlugins/OnlineSubsystemEOS/README.md) 참고.

---

## UnrealPlugins/SimpleUPNP

Unreal Engine Marketplace에 등록된 Native Code Plugin으로, Listen 서버와 Dedicated Server 양쪽 모두에서 사용 가능합니다. 게임 서버가 실행 중인 PC의 Router(NAT)에 Port Forwarding을 자동으로 등록해서 Relay Server 없이도 외부 인터넷에서 접속 가능한 P2P Play를 가능하게 합니다.

**핵심 설계**
- **UPnP IGD 4단계 Protocol 구현**: Discovery(SSDP Multicast) → Description(HTTP GET으로 기기 XML 수신) → Control(SOAP Action 호출) → 결과 반영까지, IGD(Internet Gateway Device) Profile을 직접 구현했습니다.
- **경량 Parser**: 정규 XML Parser 대신 Tag 사이 문자열을 추출하는 자체 Parser(`ExtractMessage`)로 Library 의존성 없이 Router Firmware별 Response Format 편차에 대응합니다.
- **State Machine & 자동 Retry**: `UPNPState` Enum으로 현재 단계를 추적하며, 응답 유실이나 다중 IGD 환경에서 실패 시 다음 Device로 넘어가며 자동 Retry합니다.
- **Thread 분리**: `FRunnable` 기반 전용 Worker Thread가 SSDP Multicast 송수신을 전담해, Game Thread Blocking 없이 비동기로 동작합니다.
- **Blueprint 완전 지원**: `UBlueprintAsyncActionBase` 기반 CallbackProxy 계열(`UAddPortCallbackProxy` 등)로 Latent Node 한 번 호출로 Port 추가/삭제부터 성공/실패 Callback까지 이어지도록 설계해, Designer가 UPnP Protocol을 몰라도 사용할 수 있습니다.

→ 자세한 내용은 [UnrealPlugins/SimpleUPNP/README.md](./UnrealPlugins/SimpleUPNP/README.md) 참고.

---

## Directory 구조

```
.
├── README.md                                   (본 문서)
├── GameServerSample/
│   ├── README.md
│   ├── include/ src/ sql/ tools/
│   ├── report/
│   │   └── README.txt
│   └── CMakeLists.txt
├── Backend/
│   ├── README_backend.md
│   ├── architecture/
│   │   ├── diagram1.png
│   │   └── diagram2.png
│   ├── adminproxy/
│   │   └── README.md
│   ├── botclient/
│   │   └── README.md
│   └── lobbystats/
│       └── README.md
└── UnrealPlugins/
    ├── OnlineSubsystemEOS/
    │   ├── README.md
    │   └── Source/ ...
    ├── OnlineSubsystemIcarus/
    │   ├── README.md
    │   └── Source/ ...
    └── SimpleUPNP/
        ├── README.md
        └── Source/ ...
```
