# botclient

A Go-based simulation client for game backend **load testing**. It reproduces the entire flow performed by a real Unreal Engine game client — lobby entry → authentication → gateway connection → RPC sequence execution — concurrently across a large number of virtual bots, in order to validate the full backend stack that `OnlineSubsystemIcarus` communicates with (Gateway, Lobby, Player Service, etc.) end-to-end under load.

---

## Overview

`botclient` reads a scenario defined in JSON, spawns as many independent virtual users as configured, and drives each one through its own request sequence via its own state machine. Each bot follows exactly the same overall flow as a real game client: lobby queue → JWT authentication → WebSocket gateway connection → RPC request/response cycle.

```
botrequest.json (scenario definition)
        │
        ▼
   Run() ── spawns botcount goroutines
        │
        ▼
┌───────────────────────────────────────────────────┐
│  Per-bot lifecycle                                 │
│                                                     │
│  Connect Lobby(STOMP) ──▶ ReqLobbyMessage ──▶ Receive JWT│
│         │                                          │
│         ▼                                          │
│  Connect Gateway(WebSocket) (jwttoken header)       │
│         │                                          │
│         ▼                                          │
│  Execute Scenario Requests sequentially            │
│  (ReqIdx increments, RecvAck gates the next request)│
│         │                                          │
│         ▼                                          │
│  Repeat ends → NeedReset → restart from Lobby      │
└───────────────────────────────────────────────────┘
        │
        ▼
   Central tick loop (TickInSec interval) — triggers the next request send for all bots
```

---

## Architecture

### `main.go` / `botclient.go` — Bot Lifecycle & Orchestration
- **`Run()`**: After loading configuration and validating it (botcount, scenario repeat), it launches the bot population via `createLobbyConnectors()` or `createSockets()` depending on the `LobbyEnable` setting. It then centrally schedules the next request send for all bots via a `TickInSec`-based `time.Ticker`.
- **`lobbyConnector`**: Opens an independent STOMP connection per bot, sends `ReqLobbyMessage`, and subscribes to a personal relay queue (keyed by userID). Once it receives a success response and a JWT from the lobby, it immediately transitions to a gateway WebSocket connection (`createSocket`) using that token — faithfully mirroring the real client's lobby-wait → gateway-entry flow.

  ```go
  for {
      msg := <-newRelayToQueue.C
      if msg == nil || msg.Body == nil {
          break
      }

      var message parser.WsMessage
      message.Decode(msg.Body)

      var lobbyMsg model.ResLobbyMessage
      err := json.Unmarshal(message.Event, &lobbyMsg)
      logger.LogNonFatalError(err)

      if lobbyMsg.Success {
          authToken, ok := message.Headers["jwttoken"]
          if ok {
              createSocket(userID, authToken)
              break
          }
      }
      requestLobbyMessage(userID, newLobbyConn)
  }
  ```

- **`generateRequest`**: A large switch statement that maps the scenario's `EventName` onto the actual RPC request struct (`model.ReqXxx`). It broadly covers the game's core domain events — character creation, inventory lookup, workshop item purchase, dropship configuration, Prospect (exploration) creation/update/settlement, and more.
- **`generateProspect`**: Generates dummy binary data whose payload size keeps growing with each iteration, then computes a SHA-1 hash, applies zlib compression, and base64-encodes it to build a `ReqUpdateProspect` — synthetic load-generation logic designed to reproduce the size and frequency characteristics of real save-data sync traffic.

  ```go
  // keep increasing prospect data
  if additionalDataSize <= 0 {
      additionalDataSize = 128
  }

  delta := make([]byte, additionalDataSize)
  rand.Read(delta)
  botProfile.ProspectData = append(botProfile.ProspectData, delta...)
  zippedProspect, err := compress.ZipSkipMarshal(botProfile.ProspectData)

  hash := sha1.New()
  hash.Write(botProfile.ProspectData)

  reqUpdateProspect.ProspectBlob.Hash = hex.EncodeToString(hash.Sum(nil))
  reqUpdateProspect.ProspectBlob.BinaryBlob = base64.StdEncoding.EncodeToString(zippedProspect)
  reqUpdateProspect.ProspectBlob.TotalLength = int32(len(zippedProspect))
  reqUpdateProspect.ProspectBlob.UncompressedLength = int32(len(botProfile.ProspectData))
  ```

- **`proceedNextMessage`**: The core scheduler that iterates over every bot on each tick and advances its state.
  - It checks `RecvAck` (whether the response to the previous request has arrived) and `ReqIdx` (the current progress index), and sends the next request only for bots that have received a response — **a structure that prevents overlapping requests and naturally throttles itself to match the gateway's response speed**.
  - It compares each event's configured `Delay` against the elapsed time to implement delayed sending.
  - Once the scenario is fully exhausted, it either restarts from the beginning based on the `Repeat` count, or, once fully finished, triggers a lobby reconnection (simulating a new session) via the `NeedReset` flag.
  - During idle periods with no active requests, it keeps the connection alive with a `ReqPing` every 20 seconds.

  ```go
  if botProfile.RecvAck && botProfile.ReqIdx != -1 && botProfile.Scenario.Requests != nil {
      if botProfile.ReqIdx >= len(botProfile.Scenario.Requests) {
          if botProfile.Scenario.Repeat > 1 {
              botProfile.Scenario.Repeat--
              botProfile.ReqIdx = 0
          } else {
              // start from lobby
              botProfile.NeedReset = true
              botrequest.SocketIDs.BotClients[ws] = botProfile
              continue
          }
      }

      botEvent := botProfile.Scenario.Requests[botProfile.ReqIdx]
      if botEvent.Delay <= botEvent.Elapsed {
          botEvent.Elapsed = 0
          err := sendMessage(ws, botProfile, botEvent) // gate: only fires after RecvAck
          ...
      } else {
          botEvent.Elapsed += deltaSeconds // still waiting out the delay
      }
  } else {
      // idle: keep-alive ping every 20s
      if time.Duration(20*float64(time.Second)) < elapsed {
          handler.WriteToWs(ws, &queueMsg) // ReqPing
      }
  }
  ```

- **`ReadMessage`**: The WebSocket receive loop. When a connection closure is detected, it automatically calls `reSetConnection` to restart the session from scratch (lobby or direct connection) — ensuring a long-running load test continues uninterrupted even when individual connections drop.

  ```go
  for {
      var msg parser.WsMessage
      _, b, err := ws.ReadMessage()
      if _, ok := err.(*websocket.CloseError); ok {
          go reSetConnection(ws, userID, authKey) // retry
          return
      }
      if err != nil {
          go reSetConnection(ws, userID, authKey) // retry
          return
      }
      if b != nil {
          msg.Decode(b)
          handler.HandleMessage(ws, &queueMsg)
      }
  }
  ```

### `handler.go` / `handler_custom.go` — Response Dispatch
- Uses an event-name-based handler registry pattern to process core authentication/connection responses such as `ResUserTicket`, `ResPong`, and `ResTokenIssued`/`Expired`/`Invalid`.
- **`OnResTokenExpired`/`Invalid`/`NotSupplied`**: When a token problem occurs, `resetConnection` returns the affected bot to its initial state so it starts a new session on the next tick.

  ```go
  func resetConnection(ws *websocket.Conn) {
      // reset connection in the next tick
      botrequest.SocketIDs.Mu.Lock()
      botProfile, ok := botrequest.SocketIDs.BotClients[ws]
      if ok {
          botProfile.ReqIdx = 0
          botProfile.NeedReset = true
          botrequest.SocketIDs.BotClients[ws] = botProfile
      }
      botrequest.SocketIDs.Mu.Unlock()
  }
  ```

### `handler_gen.go` / `handler_impl.go` / `model_gen.go` — Generated RPC Surface
- Response handlers and struct definitions corresponding to nearly every RPC the game backend exposes (`ReqGetCharacters`, `ReqGetMetaInventory`, `ReqCreateDropship`, `ReqSelectEnvirosuit`, `ReqUpdateProspect`, etc.) are bulk-generated by a code generator. Each handler commonly updates `RecvAck`/`ReqIdx` so that `proceedNextMessage`'s state machine can advance to the next request.
- **`PrintLog` + Slow-request logging**: Measures the elapsed time from the request send timestamp (`LastSent`) to the response being received, and selectively logs only requests that exceed the `SlowlogTime` threshold — load-test-specific instrumentation for identifying which RPCs become bottlenecks under an environment with hundreds to thousands of concurrent requests.

  ```go
  func PrintLog(eventName string, times int, LastSent time.Time, socketID string) {
      t := time.Now()
      elapsed := t.Sub(LastSent)

      log.Printf("%v : [x] Event: %q Times : %v Elapsed : %v UserID: %q ",
          t, eventName, times, elapsed, socketID)

      if botReq.Settings.Slowlog &&
          time.Duration(botReq.Settings.SlowlogTime*float64(time.Second)) < elapsed {
          Log.Printf("%v : [x] Event: %q Times : %v Elapsed : %v UserID: %q ",
              t, eventName, times, elapsed, socketID)
      }
  }
  ```

### `compress.go` — Payload Compression
Wraps zlib compression via `Zip`/`Unzip`/`ZipSkipMarshal`. Used for the large Prospect blob payloads generated by `generateProspect`, so the compression overhead of real save-data transmission is included in the load test as well.

```go
// ZipSkipMarshal ...
func ZipSkipMarshal(l []byte) ([]byte, error) {
    b := bytes.Buffer{}

    w := zlib.NewWriter(&b)
    _, err := w.Write(l)
    if err != nil {
        return b.Bytes(), err
    }
    err = w.Close()
    if err != nil {
        return b.Bytes(), err
    }
    return b.Bytes(), nil
}
```

### `botrequest.go` — Scenario Configuration Model
- **`BotSettings`**: The configuration schema that controls the entire run — gateway address, lobby connection info, bot count (`Botcount`), account prefix/index, repeat count, tick interval, slow-log threshold, and more.
- **`BotScenario`** / **`BotEvent`**: Supports weight-based (`Weight`) random scenario selection (`GetRandomScenario`), allowing different behavior patterns (e.g., several kinds of Prospect play routines) to be probabilistically mixed together into a realistic traffic profile.

  ```go
  // BotEvent ...
  type BotEvent struct {
      EventName    string `json:"eventname"`
      Repeat       int    `json:"repeat"`
      Delay        int64  `json:"delay"`
      StringParam1 string `json:"stringparam1"`
      IntParam1    int64  `json:"intparam1"`
      Elapsed      int64
      Times        int
  }

  // GetRandomScenario ...
  func GetRandomScenario(scenarios []BotScenario) BotScenario {
      totalWeight := int64(0)
      for _, scenario := range scenarios {
          totalWeight += scenario.Weight
      }

      weight := rand.Int63n(totalWeight)
      for _, scenario := range scenarios {
          weight -= scenario.Weight
          if weight <= 0 {
              return scenario
          }
      }
      return BotScenario{}
  }
  ```

- **`BotProfile`**: An in-memory session struct holding the full state maintained per bot (auth token, current character/inventory/dropship/Prospect info, request progress index, last send/ping timestamps, etc.).

  ```go
  type BotProfile struct {
      IsWaitingLobby bool
      AuthKey        string
      SocketID       string
      ReqIdx         int  // index of requests. incremented when recieve a response from gateway
      RecvAck        bool // false : waiting for gateway response, true : ready to send new request
      IcarusProfile  model.OnlineProfileUser
      Character      model.OnlineProfileCharacter
      InventoryDelta model.InventoryDelta
      Dropship       model.Dropship
      ProspectInfo   model.ProspectInfo
      Scenario       BotScenario // request lists
      LastSent       time.Time
      LastPing       time.Time
      LobbyConn      *stomp.Conn // stomp connection
      NeedReset      bool        // start from lobby
  }
  ```

- **`GetBotRequest`**: Combines `viper`-based configuration loading with command-line flags (`-botcount`, `-gatewayaddress`, etc.), providing flexible run options where CLI arguments can override the file's default values.

  ```go
  viper.SetConfigType("json")
  viper.SetConfigFile("./request/botrequest.json")
  err := viper.ReadInConfig()
  ...
  flag.StringVar(&gatewayAddress, "gatewayaddress", "ws://10.150.16.228:6001/ws", "Gateway address to connect.")
  flag.IntVar(&botCount, "botcount", 1, "A number of Bot clients to run.")
  flag.Parse()

  flag.Visit(func(flag *flag.Flag) {
      switch flag.Name {
      case "gatewayaddress":
          botReq.Settings.GatewayAddress = gatewayAddress // CLI overrides file value
      case "botcount":
          botReq.Settings.Botcount = botCount
      }
  })
  ```

### `botrequest.json` — Scenario Definition
A data file that declaratively defines the scenario to run. It's composed of `prerequests` (common initialization), `scenarios` (a list of scenarios subject to weight-based selection, each with its own per-event `repeat`/`delay`), and `postrequests` — designed so that **the load pattern can be adjusted without any code changes**.

An example of specifying a per-event `repeat`/`delay` on `ReqUpdateProspect` to load-test save-sync frequency differently across scenarios:

```json
{
    "eventname" : "ReqGetAvailableProspects",
    "delay" : 400
},
{
    "eventname" : "ReqClaimProspect",
    "stringparam1" : "Olympus_1",
    "intparam1" : 36000
},
{
    "eventname" : "ReqUpdateProspect",
    "repeat" : 20,
    "delay" : 5000
},
{
    "eventname" : "ReqSettleProspect"
}
```

---

## Key Design Details

- **Response-driven self-throttling scheduler**: Rather than blindly firing requests at a fixed rate, each bot uses the `RecvAck` state as a gate, proceeding to the next request only after its previous request has actually received a response — a design that avoids the common pitfall of the load-testing tool itself becoming the bottleneck and producing incorrect conclusions at a throughput lower than the backend's actual limit.
- **Reproducing the dual lobby-gateway connection structure**: The structure where a real client sequentially uses two connections over different protocols — a STOMP-based lobby queue and a WebSocket-based gateway — is reproduced identically in the load-testing tool, preventing inaccurate load simulation via shortcuts that diverge from production.
- **Realistic load profiling via a cumulatively growing payload**: The way `generateProspect` keeps appending data on every update reflects the game's actual characteristic of save data growing larger as a play session progresses, making it possible to validate the difference between early-session load and load later in a long-running session.
- **Weight-based scenario mixing**: Rather than repeating a single fixed script, `Weight`-based probabilistic scenario selection generates a traffic pattern closer to the behavioral diversity of a real user population, with only minor configuration changes.
- **Fault isolation and automatic recovery**: An individual bot's connection drop, token expiration, or gateway error is designed to restart only that bot independently, without halting the entire load-test process, enabling stable long-running execution even with hundreds to thousands of concurrent sessions.

---

## Tech Stack

- **Language**: Go
- **Transport**: WebSocket (`gorilla/websocket`, for the gateway connection), STOMP over RabbitMQ (`go-stomp/stomp`, for the lobby connection)
- **Configuration**: `spf13/viper` (JSON scenario file + command-line flag override)
- **Serialization**: `encoding/json`, a custom byte-level frame codec, zlib compression (`compress/zlib`)
- **Concurrency**: Independent per-bot execution via goroutines, shared state (socket map, lobby connection map) protected by `sync.RWMutex`
