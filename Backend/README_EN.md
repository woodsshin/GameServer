# icarus-backend-services

A collection of Go-based microservices making up the Icarus game backend. Each service is deployed independently and communicates with the others via RabbitMQ (AMQP/STOMP) as a shared message bus. The `OnlineSubsystemIcarus` on the Unreal Engine client side (a separate repository) integrates directly with these services over a WebSocket/STOMP wire protocol.

```
adminproxy/     Admin-facing HTTP → RabbitMQ RPC gateway (internal network only)
botclient/      A game client simulator for load testing
lobbystats/     Computes matchmaking queue statistics and broadcasts them in real time
sessionmanager/ A session store that saves in-game loadouts and ongoing match (Prospect) state to Redis

match-making/   (Unrelated to Icarus) The AWS GameLift matchmaking service for the Seedworld project
```

---

## Services

### [`adminproxy/`](./adminproxy/README_EN.md) — Admin Gateway
An admin microservice reachable only on the internal network. It acts as a request/response bridge, converting HTTP requests into RabbitMQ-based RPC calls and routing them to the Player Service, Session Manager, and Gateway, then translating the asynchronous responses back into synchronous HTTP responses. It's operated so it's never exposed externally, accessible only from the internal network, and serves as the backend for the admin tool used by operations staff to update game data, look up/modify accounts, and manage maintenance state.

**Primary use**: Backend for the admin tool used by operations staff to update game data, look up and modify accounts/characters, and manage maintenance status.

### [`botclient/`](./botclient/README_EN.md) — Load Testing Simulator
A load-testing tool that reproduces the full flow of a real game client — entering the lobby, JWT authentication, connecting to the gateway, and executing an RPC sequence — concurrently across many virtual bots. It's designed with a `RecvAck`-based self-throttling scheduler so the load-testing tool itself doesn't become the bottleneck, and load patterns can be tuned via a JSON scenario file alone, with no code changes needed.

**Primary use**: Verifying concurrent-connection throughput and bottlenecks across the entire backend stack (Gateway, Lobby, Player Service) before a new build or infrastructure change.

### [`lobbystats/`](./lobbystats/README_EN.md) — Lobby Queue Telemetry
A small microservice that polls the RabbitMQ Management API to compute the size and processing rate of the matchmaking queue, then broadcasts it to game clients in real time via STOMP. It acts as an adapter bridging the gap between RabbitMQ's pull-based management API and the push-based real-time updates that clients need.

**Primary use**: The server-side data source for the "N players waiting, estimated wait time M seconds" display on the client's lobby screen.

### [`sessionmanager/`](./sessionmanager/README_EN.md) — Session & Progress Store
A microservice that stores and retrieves in-game loadouts, ongoing match (Prospect) state, and tracked stats sent by game clients, using a Redis cluster. It receives requests as RabbitMQ messages, processes them, and sends back a response, pairing requests and responses on top of the asynchronous pub/sub model using a `frameidx` correlation header. Its key design is guaranteeing a single host during session reconnection or host migration, using `HSetNX`-based distributed locking and timeout-based failover.

**Primary use**: Looking up and updating character loadouts, managing Prospect (ongoing match) state, and reassigning hosts on session reconnection.

### [`match-making/`](./match-making/README_EN.md) — GameLift Matchmaking (Seedworld, a separate project)
> ⚠️ Unlike the 4 services above (`sessionmanager`, `adminproxy`, `botclient`, `lobbystats`), this does **not** belong to the Icarus backend. It's a backend service for **Seedworld**, a separate project, and rather than the Go/RabbitMQ stack, it's a **C# (ASP.NET Core)-based gRPC service** that integrates directly with AWS GameLift. It's kept in this repository purely for convenience, and is unrelated to the Cross-Service Architecture / Shared Design Patterns sections above.

A service that receives requests from both the game client (EOS authentication) and the dedicated server (S2S authentication), and either searches for a game session with an open slot on AWS GameLift, or, if none exists, starts new matchmaking via FlexMatch and places a session.

**Primary use**: Matchmaking and creating game sessions / player sessions for the Seedworld game client and dedicated server.

---

## Cross-Service Architecture

### Deployment topology

![Icarus backend deployment topology](./architecture/diagram1.png)

This is the actual deployment architecture showing how the entire backend is laid out on a Kubernetes cluster (Microsoft Azure). `Gateway`, `SessionManager`, `PlayerService`, `Scheduler`, and `AdminProxy` are all connected to one another through RabbitMQ, with `SessionManager` using Redis and `PlayerService`/`Scheduler` using MySQL as their data stores. Prometheus + a RabbitMQ Exporter/Adaptor monitor the message broker and service health, Azure Blob Storage serves as the content server, and Azure Key Vault handles secrets management. `AdminProxy` connects to operational notifications and automation workflows via Azure Logic Apps. Clients enter this Kubernetes cluster's `Gateway` by passing through a third-party platform such as Steam or Epic Games.

### Client-to-backend relationship

![Icarus client, backend, and third-party platform relationship](./architecture/diagram2.png)

This is the high-level structure as seen from the client's perspective. The game client connects to two independent backend axes: one is the `Gateway` of the `Icarus Backend` (the custom backend that the services in this repository belong to), and the other is the `Third party platform` (Steam/Epic, etc.) reached via a `Third party OnlineSubsystem` — providing platform services such as Auth, Friends, Messenger, and Leaderboards. `OnlineSubsystemIcarus` (the Unreal client plugin) abstracts both axes and exposes them to game code, and the microservices in this repository correspond to the server-side implementation of the left-hand axis (`Icarus Backend`).

None of the services (`sessionManager`, `playerService`, `adminproxy`, `botclient`, `lobbystats`) call each other directly — they're coupled solely through **exchange/queue bindings mediated by RabbitMQ**, so individual deployment, restarts, or failures don't create direct compile-time or runtime dependencies on other services. The contract shared between services is kept narrow, consisting of just two things: the exchange name constants, and a **byte-level wire protocol** (`EventName\nheader:value\n\nBody`) implemented identically on both the client and server sides. `botclient` reproduces the entire `Gateway` entry path shown in the two diagrams above (lobby → auth → RPC) as an independent simulator to generate load.

---

## Shared Design Patterns

Design principles that recur across multiple services.

- **RabbitMQ connection resilience**: `adminproxy`, `lobbystats`, `botclient`, and `sessionmanager` each independently implement a supervisor pattern that detects connection loss and automatically reconnects, rebuilding exchange/queue state on reconnection so the system recovers to a consistent state even after a broker restart.
- **Multi-language reimplementation of a custom wire protocol**: The same frame format as the Unreal client's `FIcarusWSFrame` (C++) is independently reimplemented in both `lobbystats` and `botclient`, and this protocol-level contract is the sole point of interoperability across different languages and repositories.
- **Config-driven operational tuning**: Values that change frequently during operation — polling interval (`lobbystats`), load patterns (`botclient`), minimum client version (`adminproxy`) — are all separated out so they can be adjusted via config files or API calls without any code changes.

---

## Tech Stack

- **Language**: Go
- **Messaging**: RabbitMQ — AMQP (`streadway/amqp`, `adminproxy`/`sessionmanager`), STOMP (`go-stomp/stomp`, `lobbystats`/`botclient`), Management HTTP API (`lobbystats`)
- **Transport**: WebSocket (`gorilla/websocket`), HTTP (`gorilla/mux`)
- **Storage**: Redis Cluster (`go-redis/v8`, `sessionmanager`)
- **Configuration**: `spf13/viper`, command-line flag override
- **Serialization**: `encoding/json`, custom byte-level frame codec, zlib compression
