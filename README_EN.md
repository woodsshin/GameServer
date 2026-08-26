# Portfolio

A collection of projects centered on Network Programming and Game Engine Middleware. Each project is organized like an independent repository, and detailed implementation notes and verification results can be found via the links below.

**GameServerSample** is a pure C++ Game Server example written with the help of Claude Code, independent of Unreal Engine. The three projects under **UnrealPlugins** (OnlineSubsystemEOS, OnlineSubsystemIcarus, SimpleUPNP) are all Native Code Plugins/Modules that run on an Unreal Engine Dedicated Server (and Client) that I implemented myself. **Backend** is likewise described with a focus on the microservices I personally developed; microservices co-developed with full-stack engineers are not included.

| Project | Summary | Stack |
|---|---|---|
| [GameServerSample](./GameServerSample/README_EN.md) | An epoll-based asynchronous Game Server. Handles large-scale concurrent connections via a DB Connection Pool and a thread-separated architecture | C++, epoll, MySQL, pthread |
| [UnrealPlugins/OnlineSubsystemIcarus](./UnrealPlugins/OnlineSubsystemIcarus/README_EN.md) | A Native Code Plugin that integrates a custom Game Backend (Icarus) with UE's `OnlineSubsystem` interface. Implements WebSocket RPC and STOMP-based Lobby Messaging from scratch | Unreal Engine, C++, WebSocket, STOMP |
| [Backend](./Backend/README_EN.md) | A collection of Go-based microservices making up the Icarus game backend. An independently deployed structure using RabbitMQ as a shared message bus | Go, RabbitMQ (AMQP/STOMP), Kubernetes, Redis, MySQL |
| [UnrealPlugins/OnlineSubsystemEOS](./UnrealPlugins/OnlineSubsystemEOS/README_EN.md) | A Native Code Plugin that wraps Epic Online Services in Unreal Engine's standard `OnlineSubsystem` interface | Unreal Engine, C++, EOS SDK |
| [UnrealPlugins/SimpleUPNP](./UnrealPlugins/SimpleUPNP/README_EN.md) | A Native Code Plugin that automatically registers port forwarding on a router via the UPnP IGD protocol | Unreal Engine, C++, SSDP/SOAP |

---

## Perspective — Why These Five Projects

All five projects address the same underlying problem — **how to establish a communication path between clients** — but at different layers.

```
                    ┌─────────────────────────────────────────────────────────┐
                    │              Establishing a Multiplayer Session           │
                    └─────────────────────────────────────────────────────────┘
                                      │
        ┌─────────────────┬──────────┴──────────┬─────────────────────┐
        ▼                 ▼                     ▼                     ▼
  Server Infrastructure  Custom Backend    Platform Backend      Network Transport
  (Built from scratch)    Integration      Integration            (NAT Traversal)
        │             (Custom Protocol)   (Epic Service            │
        │              ┌────┴────┐         Integration)             │
        ▼              ▼         ▼              ▼                     ▼
GameServerSample    Backend  OnlineSubsystem OnlineSubsystemEOS   SimpleUPNP
epoll async I/O,     Go       Icarus         Wraps EOS SDK        Auto-registers
DB integration,      Microservices, WebSocket RPC +  in UE's        Port Mapping via
concurrency control  RabbitMQ-  STOMP Lobby   OnlineSubsystem,    UPnP IGD, achieving
                     based       (UE Plugin    integrates EOS      P2P without a
                     message bus  Client)      P2P NAT Traversal   Relay server
```

- **GameServerSample** is a pure C++ standalone Game Server example, independent of Unreal Engine, written with the help of Claude Code. It answers the question of how to safely and asynchronously handle large-scale concurrent connections on the server side, with a custom protocol and custom infrastructure designed from the ground up.
- **Backend** and **OnlineSubsystemIcarus** form a matched server/client pair. Backend is the server side, combining several independent Go microservices via a RabbitMQ message bus, while OnlineSubsystemIcarus is the plugin — mounted on an Unreal Engine Dedicated Server/Client — that communicates with that backend over WebSocket (a custom framing protocol) plus STOMP Lobby Messaging. Because the wire protocol was designed and implemented on both the server and client sides, this is a complete example of custom backend integration that doesn't rely on a standard platform SDK.
- **OnlineSubsystemEOS** is a middleware layer mounted on an Unreal Engine Dedicated Server/Client that integrates Epic's backend services (authentication, session, matchmaking, P2P) into the engine's standard interface within the Unreal Engine ecosystem. It leverages EOS's own P2P NAT traversal and relay fallback.
- **SimpleUPNP** is a plugin usable on both the Unreal Engine client and Dedicated Server. Using only a plain protocol (UPnP) — with no backend service — it opens a port directly on the router of the PC it's running on, offering a more fundamental (low-level) solution for establishing a fully relay-free P2P path.

Through these five projects, the goal was to demonstrate the ability to design custom server infrastructure, implement distributed backend services alongside their corresponding client-side protocols, and solve problems at the game engine middleware/network protocol level.

---

## GameServerSample

A pure C++ standalone Game Server example, independent of Unreal Engine, written with the help of Claude Code. Implemented with epoll-based asynchronous I/O, a DB connection pool, and a thread-separated architecture, it supports account creation, login, chat broadcast, and handling of large-scale concurrent connections.

**Key Design**
- **Separation of Accept Thread / IoWorker Thread**: A dedicated Accept thread handles new connections exclusively, while each IoWorker owns its own independent epoll instance, allowing horizontal scaling proportional to CPU core count.
- **Asynchronous DB processing**: Calling blocking MySQL queries directly from an IoWorker thread would cause head-of-line blocking, so DBTasks are enqueued and processed by a separate DB worker pool in a pipeline.
- **Length-based binary protocol**: A `[4B TotalSize][2B PacketType][Body]` structure handles partial and combined reads over a TCP stream.
- **Concurrency control**: A `shared_mutex` allows concurrent entry for read-heavy operations (broadcast), with a pattern of releasing the lock immediately after taking a snapshot to minimize lock hold time.
- **Security**: Password hashing based on salt + SHA-256 stretching (10,000 rounds), and prepared statements to block SQL injection.

**Measured verification**: Using a custom-built asynchronous load-testing tool, up to 1,000 concurrent connections were measured. The connection/broadcast layer maintained a 100% success rate with no bottlenecks, but signup/login throughput was found to plateau at 27.5 req/sec; a control-group test cross-verified that this was caused by CPU saturation from SHA-256 stretching (reducing the stretching round count improved the success rate from 92.3% to 100% and improved p50 latency by roughly 14x). The full methodology and figures are documented in [`report/README_EN.md`](./GameServerSample/report/README_EN.md).

→ See [GameServerSample/README_EN.md](./GameServerSample/README_EN.md) for details.

---

## Backend

A collection of Go-based microservices making up the Icarus game backend. Each service is deployed independently and communicates with the others via RabbitMQ (AMQP/STOMP) as a shared message bus. The `OnlineSubsystemIcarus` on the Unreal Engine client side integrates directly with these services over a WebSocket/STOMP wire protocol.

**Component Services**
- **adminproxy**: An admin-facing gateway that converts HTTP requests into RabbitMQ-based RPC calls, routing them to the Player Service, Session Manager, and Gateway. It bridges RabbitMQ's pub/sub model with HTTP's synchronous model using `frameIdx`-based `context.Context` correlation.
- **botclient**: A load-testing tool that reproduces the exact flow of a real client — entering the lobby, JWT authentication, connecting to the gateway, and executing RPC sequences — across many virtual bots. It's designed with a `RecvAck`-based self-throttling scheduler so the tool itself doesn't become the bottleneck.
- **lobbystats**: A small service that polls the RabbitMQ Management API to compute matchmaking queue statistics and broadcasts them to clients in real time via STOMP.
- **sessionmanager**: A service that stores and retrieves in-game loadouts, ongoing match (Prospect) state, and tracking statistics sent by clients, using a Redis cluster. It guarantees a single host during session reconnection or host migration using `HSetNX`-based distributed locking and timeout-based failover.

**Key Design**
- **RabbitMQ-centered decoupling**: No service calls another directly — all are coupled solely through exchange/queue bindings, so individual deployment, restarts, or failures don't create direct compile-time or runtime dependencies on other services.
- **Kubernetes (Azure) deployment**: Gateway, SessionManager, PlayerService, Scheduler, and AdminProxy are all connected through RabbitMQ, using Redis (SessionManager) and MySQL (PlayerService/Scheduler) as data stores.
- **Connection resilience**: `adminproxy`, `lobbystats`, `botclient`, and `sessionmanager` each independently implement a supervisor pattern that detects connection loss and automatically reconnects, rebuilding exchange/queue state on reconnection.
- **Multi-language reimplementation of a custom wire protocol**: The same frame format (`EventName\nheader:value\n\nBody`) as the Unreal client's `FIcarusWSFrame` (C++) is independently reimplemented in both `lobbystats` and `botclient`; this protocol-level contract is the sole point of interoperability across different languages and repositories.

→ See [Backend/README_EN.md](./Backend/README_EN.md) for details.

---

## UnrealPlugins/OnlineSubsystemIcarus

A Native Code Plugin (Online Subsystem, OSS) that runs on both the Unreal Engine Dedicated Server and the client. It is directly paired with the Backend above, connecting into UE's `IOnlineSubsystem` abstraction layer via WebSocket (STOMP-like framing) and RabbitMQ/STOMP Lobby Messaging, and provides Identity, Session/Matchmaking, User Cloud Storage, and Lobby Queue services on top of a custom protocol.

**Key Design**
- **Custom `FIcarusWSFrame` wire protocol**: A `COMMAND\nheader:value\n\nBODY` frame format inspired by STOMP, implemented from scratch via manual byte-buffer parsing. A thread-safe, monotonically increasing `FrameIndex` correlates asynchronous requests and responses, and every request carries a JWT bearer token.
- **Asynchronous WebSocket RPC layer** (`UIcarusConnectionComponentBase`): Handles automatic timeout for unanswered requests via command→response pairing, and on transmission failure, retries and then synthesizes a failure response through the same handler path as a real server response — so callers never need to distinguish between a network failure and an RPC error. Also includes automatic reconnection with exponential backoff.
- **Dedicated heartbeat thread** (`FIcarusConnectionPingManager`): An `FRunnable` worker thread, separate from the game thread, is solely responsible for periodic pings and Prospect heartbeats.
- **STOMP-based parallel lobby client** (`UIcarusLobbyConnectionComponentBase`): Subscribes to the matchmaking queue via a second STOMP-over-RabbitMQ connection, entirely separate from the main gateway connection, and computes an ETA using a rolling average over the last 10 samples to smooth out noise in queue drain rate.
- **Interface hot-swap on mode switch**: Switches between online/offline connection components without reinitializing the subsystem, rebinding the callbacks of all dependent interfaces.

If Backend is the server-side group of RabbitMQ microservices, OnlineSubsystemIcarus is its client-side counterpart — a reimplementation of the same wire protocol used to communicate with that Backend.

→ See [UnrealPlugins/OnlineSubsystemIcarus/README_EN.md](./UnrealPlugins/OnlineSubsystemIcarus/README_EN.md) for details.

---

## UnrealPlugins/OnlineSubsystemEOS

A Native Code Plugin that runs on both the Unreal Engine Dedicated Server and the client, wrapping Epic Online Services (EOS) in UE's standard `OnlineSubsystem` interface. With a single `DefaultPlatformService=EOS` setting, existing online logic written for another platform (login, session/matchmaking, achievements, friends, etc.) can be switched over to EOS without any code changes.

**Key Design**
- **EOS SDK initialization**: Dynamically loads the platform-specific EOS SDK binary at module load time and registers a factory with the `OnlineSubsystem` module.
- **EOS P2P-based network layer**: Implements UE's `Sockets`/`SocketSubsystem`/`NetDriver`/`NetConnection` layers using EOS. `FInternetAddrEOS` is an address scheme carrying an EOS ID + channel instead of an IP, and `UEOSNetDriver` automatically branches between the EOS path and the normal IP path (passthrough) based on whether the connection URL has an `eos.` prefix.
- **NAT traversal & relay fallback**: `FSocketEOS::SendTo`/`RecvFrom` call `EOS_P2P_SendPacket`/`ReceivePacket`, and in environments where NAT traversal fails, automatically fall back to the EOS relay server according to the `bAllowP2PPacketRelay` setting.
- **Connection lifecycle management**: `FSocketSubsystemEOS` separately manages live connections and connections pending termination (dead connections), using a linger timeout to prevent malfunctions caused by race conditions during reconnection.
- **Combining session and P2P addresses**: Using the `P2PADDRESS` returned by EOS Sessions (in the form `eos.<EOSID>:<Channel>`) directly as a `ClientTravel` URL establishes a NAT-traversed P2P connection through the network layer described above.

→ See [UnrealPlugins/OnlineSubsystemEOS/README_EN.md](./UnrealPlugins/OnlineSubsystemEOS/README_EN.md) for details.

---

## UnrealPlugins/SimpleUPNP

A Native Code Plugin published on the Unreal Engine Marketplace, usable on both listen servers and dedicated servers. It automatically registers port forwarding on the router (NAT) of the PC running the game server, enabling P2P play reachable from the public internet without a relay server.

**Key Design**
- **Full implementation of the 4-stage UPnP IGD protocol**: Implements the IGD (Internet Gateway Device) profile from scratch — Discovery (SSDP multicast) → Description (receiving device XML via HTTP GET) → Control (invoking SOAP actions) → applying the result.
- **Lightweight parser**: Instead of a full XML parser, a custom parser (`ExtractMessage`) extracts strings between tags, handling variance in response format across router firmware without any library dependency.
- **State machine & automatic retry**: A `UPNPState` enum tracks the current stage, automatically retrying by moving to the next device on response loss or in multi-IGD environments.
- **Thread separation**: A dedicated `FRunnable`-based worker thread handles SSDP multicast send/receive, operating asynchronously without blocking the game thread.
- **Blueprint support**: A family of `UBlueprintAsyncActionBase`-based callback proxies (e.g. `UAddPortCallbackProxy`) lets a single latent node call handle everything from adding/removing a port through to success/failure callbacks — so designers can use it without needing to understand the UPnP protocol.

→ See [UnrealPlugins/SimpleUPNP/README_EN.md](./UnrealPlugins/SimpleUPNP/README_EN.md) for details.

---

## Directory Structure

```
.
├── README_EN.md                                (this document)
├── GameServerSample/
│   ├── README_EN.md
│   ├── include/ src/ sql/ tools/
│   ├── report/
│   │   └── README_EN.txt
│   └── CMakeLists.txt
├── Backend/
│   ├── README_backend_EN.md
│   ├── architecture/
│   │   ├── diagram1.png
│   │   └── diagram2.png
│   ├── adminproxy/
│   │   └── README_EN.md
│   ├── botclient/
│   │   └── README_EN.md
│   ├── lobbystats/
│   │   └── README_EN.md
│   └── sessionmanager/
│       └── README_EN.md
└── UnrealPlugins/
    ├── OnlineSubsystemEOS/
    │   ├── README_EN.md
    │   └── Source/ ...
    ├── OnlineSubsystemIcarus/
    │   ├── README_EN.md
    │   └── Source/ ...
    └── SimpleUPNP/
        ├── README_EN.md
        └── Source/ ...
```
