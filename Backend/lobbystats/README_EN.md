# lobbystats

A Go-based backend microservice that polls the **RabbitMQ Management API** to compute backend queue state, and broadcasts it to game clients in real time via the **STOMP protocol**. It's the server-side counterpart that generates the `ResLobbyStats` event subscribed to by `IcarusLobbyConnectionComponent` in the Unreal Engine client's `OnlineSubsystemIcarus`.

---

## Overview

`lobbystats` is a small service with a single responsibility: it periodically queries the number of waiting entries and the processing rate accumulated in a specific RabbitMQ queue (`lobby`), reshapes that data into a form game clients can consume, and republishes it back to the same RabbitMQ instance via STOMP.

```
RabbitMQ Management HTTP API  ──(polling)──▶  lobbystats
                                                   │
                                                   ▼
                                        Build model.ResLobbyStats
                                                   │
                                                   ▼
RabbitMQ (STOMP) ◀──(WsMessage encoding)── lobbystats
       │
       ▼
Game client (Unreal OSS Lobby Connection Component)
```

The key design point is that client and server share the **same frame format** (`EventName\nheader:value\n\nBody`); `parser.WsMessage` handles this encoding and is a wire-compatible counterpart to the client-side `FIcarusWSFrame`.

---

## Architecture

### `main.go` — Entry Point
A minimal entry point that calls only `lobbystats.Run()`, with all the actual logic encapsulated in the `lobbystats` package.

### `lobbystats.go` — Core Service Logic
- **`connectToRabbitMQStomp`**: Handles establishing a TLS-based STOMP connection, retrying indefinitely (every 2 seconds) on connection failure. The `usetls` flag allows switching to a plain, TLS-free TCP connection in local development environments.
- **`lobbyConnector`**: Drives the service's core workflow as a goroutine.
  1. Polls the RabbitMQ Management HTTP API at an interval set by `conf.LobbyUpdateInterval`.
  2. Unmarshals the response JSON into `model.LobbyStats`, extracting queue depth (`messages`) and delivery rate (`deliver_get_details.rate`).
  3. Reassembles this into `model.ResLobbyStats`, merging in the maintenance state configured (start/end time, notice message).
  4. Encodes it with `parser.WsMessage` and broadcasts it to the configured destination via STOMP `Send`.
- **`Run`**: Loads config, initializes the `mux` router, starts the lobby connector goroutine, and runs an HTTP server (`:7071`) that includes a `/health` health-check endpoint.

The core loop of poll → transform → STOMP broadcast looks like this:

```go
go func() {
    for {
        time.Sleep(time.Duration(conf.LobbyUpdateInterval) * time.Second)

        resp, err := http.Get(url)
        if err != nil {
            logger.LogNonFatalError(err)
            continue
        }

        body, err := ioutil.ReadAll(resp.Body)
        if err != nil {
            logger.LogNonFatalError(err)
            continue
        }

        var lobbyStats model.LobbyStats
        err = json.Unmarshal(body, &lobbyStats)
        if err != nil {
            continue
        }

        resLobbyStats := model.ResLobbyStats{QueueSize: lobbyStats.Messages, MessagesReadyRate: lobbyStats.Stats.DeliverDetails.Rate}
        resLobbyStats.Maintenance.IsMaintenance = conf.IsMaintenance
        resLobbyStats.Maintenance.StartTime = conf.MaintenanceStart
        resLobbyStats.Maintenance.EndTime = conf.MaintenanceEnd
        resLobbyStats.Maintenance.Message = conf.MaintenanceMessage

        bytes, err := json.Marshal(resLobbyStats)
        if err != nil {
            continue
        }

        returnMsg := model.QueueMessage{Message: bytes, EventName: "ResLobbyStats"}
        wsMsg := parser.WsMessage{EventName: returnMsg.EventName, Headers: returnMsg.Headers, Event: returnMsg.Message}
        event := wsMsg.Encode()

        err = lobbyConn.Send(conf.LobbyDestination, "application/json", event, nil)
        if err != nil {
            logger.LogNonFatalError(err)
        }
    }
}()
```

Establishing a STOMP connection is split into TLS and plain paths, each with its own independent infinite retry loop:

```go
func connectToRabbitMQStomp(uri string) *stomp.Conn {
    for {
        if usetls {
            netConn, err := tls.Dial("tcp", uri, &tls.Config{})
            if err != nil {
                logger.LogNonFatalError(err)
                time.Sleep(2 * time.Second)
                continue
            }

            lobbyConn, err := stomp.Connect(
                netConn,
                stomp.ConnOpt.Host("icarus"),
                stomp.ConnOpt.Login(conf.LobbyUser, conf.LobbyPassword),
            )

            if err == nil {
                return lobbyConn
            }

            logger.LogNonFatalError(err)
            time.Sleep(2 * time.Second)
        } else {
            lobbyConn, err := stomp.Dial(
                "tcp", uri,
                stomp.ConnOpt.Host("/icarus"),
                stomp.ConnOpt.Login(conf.LobbyUser, conf.LobbyPassword),
            )
            if err == nil {
                return lobbyConn
            }
            logger.LogNonFatalError(err)
            time.Sleep(5 * time.Second)
        }
    }
}
```

### `model.go` — Wire-Level Data Contracts
- `LobbyStats` / `MessageStats` / `MessageDetails` — Structs that directly mirror the JSON response schema of the RabbitMQ Management API, mapping nested fields like `message_stats.deliver_get_details.rate` directly.
- `ResLobbyStats` / `MaintenanceStatus` — The actual payload sent to clients. Field names (`queueSize`, `messagesReadyRate`, `Maintenance`) are designed to map 1:1 to the `FResLobbyStats` UStruct on the Unreal client side.
- `QueueMessage` — An internal wrapper used at the pre-STOMP-transmission stage.

```go
// Rabbitmq queue stats in json format
type LobbyStats struct {
    Stats     MessageStats `json:"message_stats"`
    Messages  int          `json:"messages"`
    QueueName string       `json:"name"`
}

type MessageStats struct {
    DeliverDetails MessageDetails `json:"deliver_get_details"`
    PublishDetails MessageDetails `json:"publish_details"`
}

type MessageDetails struct {
    Rate float64 `json:"rate"`
}

// Return lobby stats message to game client
type ResLobbyStats struct {
    QueueSize         int               `json:"queueSize"`
    MessagesReadyRate float64           `json:"messagesReadyRate"`
    Maintenance       MaintenanceStatus `json:"Maintenance"`
}

type MaintenanceStatus struct {
    IsMaintenance bool   `json:"isMaintenance"`
    StartTime     int    `json:"startTime"`
    EndTime       int    `json:"endTime"`
    Message       string `json:"message"`
}
```

### `parser.go` — Custom Wire Protocol Codec
- `WsMessage.Encode()` assembles a custom frame in the form `EventName\nheader:value\n\nBody` directly at the byte level.
- `WsMessage.Decode()` implements the reverse parse, and in particular, when a `datalength` header is present, extracts the body via **precise byte-offset-based slicing** rather than newline (`\n`)-based splitting — compensating for the limitations of plain line-based parsing, which isn't binary-safe.
- The `Parsable` interface abstracts the `Encode`/`Decode` contract, structured with future extension to other message types in mind.

---

## Key Design Details

- **Polling-to-push bridge pattern**: The RabbitMQ Management API offers only a pull model (HTTP polling), but game clients need push-model (STOMP subscription) real-time updates. `lobbystats` acts as the adapter bridging the gap between these two models, and separates out the polling interval (`LobbyUpdateInterval`) into config so the trade-off between server load and client responsiveness can be tuned during operation.
- **Connection resilience**: TLS handshake and STOMP connect each have their own independent retry loop, handling failures at the network layer and the protocol layer separately. This forms a symmetric server-side resilience strategy to the exponential-backoff reconnect used by `UIcarusConnectionComponentBase` on the client side.
- **Wire protocol symmetry**: `parser.WsMessage` is implemented independently of the Unreal client's `FIcarusWSFrame`, but is designed to conform to the same frame layout — guaranteeing protocol-level interoperability even though the two codebases live in separate languages (Go/C++) and separate repositories.
- **In-band delivery of maintenance status**: Maintenance information is carried along with every stats broadcast rather than requiring a separate API call, so the client can determine whether the service is under maintenance through the very same channel it receives queue information on.

---

## Tech Stack

- **Language**: Go
- **Messaging**: RabbitMQ (STOMP protocol via `go-stomp/stomp`), RabbitMQ Management HTTP API
- **HTTP Routing**: `gorilla/mux`
- **Transport Security**: TLS (`crypto/tls`)
- **Serialization**: `encoding/json`, custom byte-level frame codec
