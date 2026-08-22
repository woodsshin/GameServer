# Portfolio

Network Programming과 Game Engine Middleware를 중심으로 한 프로젝트 모음입니다. 각 프로젝트는 독립된 Repository처럼 구성되어 있으며, 하단 링크에서 세부 구현과 검증 결과를 확인할 수 있습니다.

| Project | 요약 | Stack |
|---|---|---|
| [GameServerSample](./GameServerSample/README.md) | epoll 기반 비동기 Game Server. DB Connection Pool과 Thread 분리 Architecture로 대규모 동시 접속을 처리 | C++, epoll, MySQL, pthread |
| [UnrealPlugins/OnlineSubsystemEOS](./UnrealPlugins/OnlineSubsystemEOS/README.md) | Epic Online Services를 Unreal Engine의 표준 `OnlineSubsystem` Interface로 wrapping한 Native Code Plugin | Unreal Engine, C++, EOS SDK |
| [UnrealPlugins/SimpleUPNP](./UnrealPlugins/SimpleUPNP/README.md) | UPnP IGD Protocol로 Router에 Port Forwarding을 자동 등록하는 Native Code Plugin | Unreal Engine, C++, SSDP/SOAP |

---

## 관점 — 왜 이 세 프로젝트인가

세 프로젝트는 모두 **"Client 간 통신 경로를 어떻게 확보할 것인가"**라는 동일한 문제를 서로 다른 계층에서 다룹니다.

```
                    ┌─────────────────────────────────────────┐
                    │        Multiplayer Session 성립          │
                    └─────────────────────────────────────────┘
                                      │
        ┌─────────────────────────────┼─────────────────────────────┐
        ▼                             ▼                             ▼
  Server Infrastructure        Platform Backend              Network Transport
  (자체 구현)                   Integration                   (NAT Traversal)
        │                             │                             │
GameServerSample              OnlineSubsystemEOS              SimpleUPNP
epoll 비동기 I/O,              EOS SDK를 UE                    UPnP IGD로 Router에
DB 연동, 동시성 제어            OnlineSubsystem으로 wrapping,   Port Mapping을 자동 등록,
                               EOS P2P NAT Traversal 통합       Relay 서버 없이 P2P 성립
```

- **GameServerSample**은 Server Side에서 대규모 동시 접속을 어떻게 비동기적으로, 안전하게 처리할지에 대한 답입니다. Custom Protocol과 Custom Infrastructure를 밑바닥부터 설계했습니다.
- **OnlineSubsystemEOS**는 Unreal Engine 생태계 안에서 Epic의 Backend Service(인증, Session, matchmaking, P2P)를 엔진 표준 Interface로 통합하는 Middleware Layer입니다. EOS 자체의 P2P NAT Traversal과 Relay Fallback을 활용합니다.
- **SimpleUPNP**는 Backend Service 없이 순수 Protocol(UPnP)만으로 Client의 Router에 직접 Port를 열어, Relay Server 없는 완전한 P2P 경로를 확보하는 더 근본적인(low-level) 해법입니다.

세 프로젝트를 통해 Custom Server Infrastructure 설계 역량과, Game Engine Middleware/Network Protocol Level의 문제 해결 역량을 함께 보이는 것을 목표로 했습니다.

---

## GameServerSample

epoll 기반 비동기 I/O, DB Connection Pool, Thread 분리 Architecture로 구현한 소규모 Game Server입니다. 계정 생성, 로그인, Chat Broadcast, 대규모 동시 접속 처리를 지원합니다.

**핵심 설계**
- **Accept Thread / IoWorker Thread 분리**: Accept 전용 Thread가 신규 연결을 전담하고, 각 IoWorker는 독립된 epoll Instance를 보유해 CPU Core 수에 비례한 수평 확장이 가능합니다.
- **DB 비동기 처리**: Blocking 방식의 MySQL Query를 IoWorker Thread에서 직접 호출하면 Head-of-Line Blocking이 발생하므로, DBTask를 Queue에 Enqueue하고 별도의 DB Worker Pool이 처리하는 Pipeline을 구성했습니다.
- **길이 기반 Binary Protocol**: `[4B TotalSize][2B PacketType][Body]` 구조로 TCP Stream의 부분 수신/결합 수신을 처리합니다.
- **동시성 제어**: `shared_mutex`로 읽기 위주 연산(Broadcast)의 동시 진입을 허용하고, Snapshot 후 즉시 Lock을 해제하는 패턴으로 Lock 보유 시간을 최소화했습니다.
- **보안**: salt + SHA-256 Stretching(10,000 round) 기반 비밀번호 해싱, Prepared Statement로 SQL Injection 차단.

**실측 검증**: 자체 제작 비동기 Load Test 도구로 최대 1,000명 동시 접속을 실측했습니다. 접속·Broadcast 계층은 병목 없이 100% 성공률을 유지했으나, 회원가입/로그인 처리량이 27.5 req/sec로 고정되는 현상을 발견하고 원인이 SHA-256 Stretching에 따른 CPU 포화임을 대조군 테스트로 교차 검증했습니다(Stretching Round 축소 시 성공률 92.3% → 100%, 지연 p50 약 14배 개선). 방법론과 전체 수치는 하위 [`report/README.md`](./GameServerSample/report/README.md)에 정리되어 있습니다.

→ 자세한 내용은 [GameServerSample/README.md](./GameServerSample/README.md) 참고.

---

## UnrealPlugins/OnlineSubsystemEOS

Epic Online Services(EOS)를 UE의 표준 `OnlineSubsystem` Interface로 wrapping한 Native Code Plugin입니다. `DefaultPlatformService=EOS` 설정 하나로 기존에 다른 Platform을 대상으로 구현된 온라인 로직(로그인, Session/matchmaking, 업적, 친구 등)을 코드 수정 없이 EOS로 교체할 수 있습니다.

**핵심 설계**
- **EOS SDK 초기화**: Module Load 시점에 Platform별 EOS SDK Binary를 동적 Load하고, `OnlineSubsystem` Module에 Factory를 등록합니다.
- **EOS P2P 기반 Network Layer**: UE의 `Sockets`/`SocketSubsystem`/`NetDriver`/`NetConnection` 계층을 EOS Version으로 구현했습니다. `FInternetAddrEOS`는 IP 대신 EOS ID + Channel을 담는 주소 체계이며, `UEOSNetDriver`는 접속 URL의 `eos.` Prefix 여부로 EOS 경로와 일반 IP 경로(passthrough)를 자동 분기합니다.
- **NAT Traversal & Relay Fallback**: `FSocketEOS::SendTo`/`RecvFrom`은 `EOS_P2P_SendPacket`/`ReceivePacket`을 호출하며, NAT Traversal이 실패하는 환경에서는 `bAllowP2PPacketRelay` 설정에 따라 EOS Relay Server로 자동 Fallback합니다.
- **연결 수명 관리**: `FSocketSubsystemEOS`가 살아있는 연결과 종료 대기 중인 연결(Dead Connection)을 분리 관리하고, Linger Timeout을 둬서 재접속 경쟁 상태(Race Condition)로 인한 오작동을 방지합니다.
- **Session ↔ P2P 주소 결합**: EOS Sessions가 반환하는 `P2PADDRESS`(`eos.<EOSID>:<Channel>` 형식)를 그대로 `ClientTravel` URL로 사용하면 위 Network Layer를 통해 NAT Traversal P2P 접속이 성립합니다.

→ 자세한 내용은 [UnrealPlugins/OnlineSubsystemEOS/README.md](./UnrealPlugins/OnlineSubsystemEOS/README.md) 참고.

---

## UnrealPlugins/SimpleUPNP

Unreal Engine Marketplace에 등록된 Native Code Plugin으로, 게임 Client가 실행 중인 PC의 Router(NAT)에 Port Forwarding을 자동으로 등록해서 Relay Server 없이도 외부 인터넷에서 접속 가능한 P2P(Listen 서버) Play를 가능하게 합니다.

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
└── UnrealPlugins/
    ├── OnlineSubsystemEOS/
    │   ├── README.md
    │   └── Source/ ...
    └── SimpleUPNP/
        ├── README.md
        └── Source/ ...
```
