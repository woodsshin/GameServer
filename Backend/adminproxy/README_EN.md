# admin-proxy

A game backend admin microservice reachable **only on the internal network**. It converts HTTP requests into RabbitMQ-based RPC calls, routing them to several backend services such as Player Service, Session Manager, and Gateway, and acts as a **request/response bridge** that turns asynchronous message-queue responses back into synchronous HTTP responses. It's operated so it's never exposed externally and is accessible only from the internal network, serving as the backend for the admin tool operations staff use to update game data, look up/modify accounts, and manage maintenance state.

---

## Overview

This service exposes two broad kinds of endpoints.

1. **Fire-and-forget admin endpoints** (`/update`, `/reset`, `/version`) — publish a broadcast/round-robin message to RabbitMQ without waiting for a response, then immediately return `200 OK`.
2. **Synchronous RPC endpoint** (`/admin`) — forwards an arbitrary admin RPC to a service across RabbitMQ, keeps the HTTP connection open until that service's response comes back, and passes the response body straight through to the client.

```
Admin Client (internal network)
        │  HTTP
        ▼
   admin-proxy  ──── RabbitMQ Exchange (PL / SM / GW / AP) ────▶  Player Service / Session Manager / Gateway
        ▲                                                                    │
        │  context.Value response matched by frameIdx                       │
        └────────────── RabbitMQ return queue (exclusive) ◀───────────────────┘
```

The core design challenge is **bridging HTTP's synchronous request/response model naturally onto RabbitMQ's asynchronous pub/sub model**, which is solved via a `frameIdx`-based `context.Context` correlation mechanism.

---

## Architecture

### `main.go` — Entry Point
A minimal entry point that calls `admin_proxy.Run(usetls)`. Since TLS termination is handled at a separate layer (e.g. a reverse proxy), the default is hardcoded to `false`.

### `admin_proxy.go` — HTTP Router & RabbitMQ Connection Lifecycle
- **`connectToRabbitMQ` / `rabbitConnector`**: A supervisor pattern that detects connection-close events (`NotifyClose`) via a channel and automatically reconnects. On every reconnect, it redeclares the `PL`/`GW` exchanges, creates a fresh exclusive/durable return queue, and registers a consumer.
- **Response consumption loop**: Every message arriving on the return queue is unmarshaled into a `model.QueueMessage` and delegated to `handler.HandleResponse`, with explicit completion declared via a manual `Ack`.

**Code: RabbitMQ reconnection supervisor + consume loop** (`admin_proxy.go`)

```go
func rabbitConnector(uri string) {
	var rabbitErr *amqp.Error
	for {
		rabbitErr = <-rabbitCloseError
		if rabbitErr != nil {
			log.Printf("Connecting to RabbitMQ")
			conn = connectToRabbitMQ(uri)
			rabbitCloseError = make(chan *amqp.Error)
			conn.NotifyClose(rabbitCloseError)
			ch, err := conn.Channel()
			mqChannel = ch

			logger.FailOnError(err)

			err = ch.ExchangeDeclare(model.PLExchange, amqp.ExchangeDirect, true, false, false, false, nil)
			logger.FailOnError(err)

			err = ch.ExchangeDeclare(model.GWExchange, amqp.ExchangeDirect, true, false, false, false, nil)
			logger.FailOnError(err)

			returnQueue, err = ch.QueueDeclare(
				"",    // name
				true,  // durable
				false, // delete when unused
				true,  // exclusive
				false, // no-wait
				nil,   // arguments
			)
			logger.FailOnError(err)

			queueName = returnQueue.Name

			delivery, err := ch.Consume(
				returnQueue.Name,
				"",
				false,
				false,
				false,
				false,
				nil,
			)
			logger.FailOnError(err)

			// Response consumption loop: return queue → model.QueueMessage → handler.HandleResponse
			go func() {
				for mqMessage := range delivery {
					var queueMsg model.QueueMessage
					err := json.Unmarshal(mqMessage.Body, &queueMsg)
					logger.LogNonFatalError(err)

					handler.HandleResponse(&queueMsg, ch)
					mqMessage.Ack(false)
				}
			}()
		}
	}
}
```

- **`/update`**: Depending on a query parameter (`gw`/`pl`), delivers a `ReqUpdateGameData` message either as a broadcast to the Gateway (delivered to every binding of the direct exchange) or as round-robin to the Player Service (handled by a single consumer).
- **`/reset`**, **`/version`**: Fire-and-forget admin endpoints for, respectively, resetting all talents and updating the minimum client version.
- **`adminHandler` (`/admin`)**: A general-purpose admin RPC gateway.
  1. Extracts `EventName`/`UserID` from the HTTP headers.
  2. Issues a unique frame index per request via a thread-safe `getFrameIdx()` (protected by `sync.RWMutex`).
  3. Forwards the request body to RabbitMQ as a raw payload.
  4. Creates a wait context via `context.WithTimeout` (10 seconds) and delegates to `handler.HandleAdminRequest`.
  5. Polls every 10ms to detect either context cancellation (response arrived) or deadline exceeded (timeout), and completes the HTTP response accordingly.

### `handler.go` — RPC Dispatch Core
- Maintains a handler registry (`messageHandlers`) of type `messageHandlerFn`, mapping event names to their processing functions.
- **`HandleAdminRequest`**: Checks the event name against the `adminPLRPCs`/`adminSMRPCs` whitelists to determine the destination exchange (Player Service or Session Manager), explicitly rejecting any unregistered RPC. It then stores the `context.Context` and `context.CancelFunc` keyed by `frameIdx`, setting up the state needed to link an asynchronously arriving response back to the currently waiting HTTP request.
- **`HandleResponse`**: A simple dispatcher that looks up the handler registered for the incoming message's event name and executes it.
- **`sendMqMsg`**: Marks every published message with `Persistent` delivery mode so nothing is lost even if RabbitMQ restarts.

**Code: RabbitMQ publish** (`handler.go`)

```go
func sendMqMsg(mqChannel *amqp.Channel, exchange string, key string, queueMsg *model.QueueMessage) error {
	body, err := json.Marshal(queueMsg)

	if err == nil {
		err = mqChannel.Publish(
			exchange,
			key,
			false,
			false,
			amqp.Publishing{
				DeliveryMode: amqp.Persistent,
				ContentType:  "application/json",
				Body:         body,
			})
	}

	if err == nil {
		event := time.Now()
		log.Printf("%v : [x] MqMsg Sent Id: %q Event: %q to %q", event, queueMsg.SocketID, queueMsg.EventName, exchange)
	}
	logger.LogNonFatalError(err)
	return err
}
```

`exchange` and `key` are combined differently depending on the routing strategy: a Gateway broadcast uses `(model.GWExchange, model.APExchange)`, while Player Service round-robin uses `(model.PLExchange, model.PLExchange)` — matching the destination exchange and binding key so the message reaches a single consumer.

### `handler_custom.go` — RPC Whitelist & Response Handlers
- `registerCustom()` registers an extensive list of admin RPCs (`ReqXxx` → `ResXxx`) to the whitelist, organized by domain: Profile, Prospect, Notification, Challenges, Inventory, Workshop, Talents, Drop Loadout, and more. This list effectively serves as the specification of the API surface the admin tool is allowed to call.
- **`onResAdminRequest`**: The common response handler for every whitelisted RPC. It restores `frameIdx` from the headers to find the corresponding request's wait context, injects either a standard error payload (if error headers `error`/`errorMessage` are present) or the raw message body into the context value, then calls `CancelFunc` to immediately wake the waiting HTTP handler's polling loop.
- **`onResUpdateGameData`** / **`onResResetAllTalents`**: Dedicated response handlers for the fire-and-forget request family, reflecting success/failure into a global `responded` flag. On failure, they send an external notification via a Zapier webhook (`sendErrorNotification`).

### `handler_gen.go`
An extension point for RPC registrations generated by a code generator. Currently empty; `Register()` serves as the composition point that calls both `registerCustom()` and `registerGenerated()`.

### `model.go` / `shared.go`
- `model.go`: Defines response structs specific to this service (`ResUpdateGameData`, `ResResetAllTalents`) and the message envelope (`QueueMessage`).
- `shared.go`: Exchange name constants (`PL`, `MM`, `SM`, `EA`, `GW`, `AP`) shared across every Go service in the Icarus backend — the single source of truth for the inter-service topology contract.

### `adminrpc.go`
Defines the admin-RPC-specific header key (`frameidx`), parameter name constants, and the standard error response struct (`ResAdminError`).

### `parser.go` — STOMP-Style Custom Message Encoder/Decoder
`WsMessage` is a lightweight protocol implementing, from scratch, a text-frame structure similar to [STOMP](https://stomp.github.io/) (`command/event line` → `header lines` → blank line → `body`). Separate from the `model.QueueMessage` (JSON) used over RabbitMQ, this appears to be a frame format used for integration with other transports such as WebSocket.

```
EventName
header1:value1
header2:value2

<raw event payload bytes>
```

- **`Encode()`**: Builds a byte slice by concatenating, in order: event name → list of headers → blank line → payload.
- **`Decode()`**: Parses the first line as the event name, then parses subsequent `key:value` lines into a header map until a blank line ends header parsing. If a `datalength` header is present, it's trusted to slice the payload from the end of the buffer by that exact length (supporting a binary-safe payload that may contain embedded newlines or null bytes); otherwise, the remaining lines are concatenated to form the payload.

**Code: STOMP-style frame encoder/decoder** (`parser.go`)

```go
// WsMessage ...
type WsMessage struct {
	EventName string
	Headers   map[string]string
	Event     []byte
}

// Encode ...
func (s *WsMessage) Encode() []byte {
	b := make([]byte, 0)

	b = append(b, []byte(fmt.Sprintf("%v\n", s.EventName))...)
	for k, v := range s.Headers {
		b = append(b, []byte(fmt.Sprintf("%v:%v\n", k, v))...)
	}
	b = append(b, []byte{'\n'}...)
	b = append(b, s.Event...)

	return b
}

// Decode ...
func (s *WsMessage) Decode(b []byte) error {
	isHeader := false
	byteLines := bytes.Split(b, []byte{'\n'})
	for i, line := range byteLines {
		if i == 0 {
			event := string(line)
			s.EventName = event
			isHeader = true
			continue
		}
		if isHeader {
			if len(s.Headers) == 0 {
				s.Headers = make(map[string]string)
			}
			header := string(line)
			if header == "\n" || len(header) < 1 {
				isHeader = false
				if _, ok := s.Headers["datalength"]; ok {
					break
				}
				continue
			}
			h := strings.Split(header, ":")
			s.Headers[h[0]] = h[1]
			continue
		}
		if len(s.Event) == 0 {
			s.Event = make([]byte, 0)
		}
		if len(line) > 0 {
			if line[len(line)-1] != 0 {
				s.Event = append(s.Event, line...)
			} else {
				s.Event = append(s.Event, line[:len(line)-1]...)
			}
		}
	}
	if _, ok := s.Headers["datalength"]; ok {
		dataLength, err := strconv.Atoi(s.Headers["datalength"])
		if err != nil {
			return err
		}
		// Get event information by splicing []byte based on data length header
		eventBytes := b[len(b)-dataLength:]
		if len(s.Event) == 0 {
			s.Event = make([]byte, 0)
		}
		s.Event = append(s.Event, eventBytes...)
		return nil
	}
	return nil
}
```

> **Note**: None of the code in this repository — `admin_proxy`, `handler`, `handler_custom`, etc. — appears to use `parser.WsMessage` directly. Since `model.QueueMessage` (JSON) is what's actually used as the RabbitMQ message envelope, `parser.go` is presumably either a utility package shared with another service (e.g. Gateway ↔ client WebSocket integration) or code intended for a future/different transport integration.

---

## Key Design Details

- **Using `context.Context` as a message correlation ID**: Repurposes the standard library's `context` package not just for request timeouts but as a **pub/sub correlation mechanism that matches an asynchronous RPC response to a specific HTTP request**. Each request's `Context`/`CancelFunc` is registered in a global map (`frameHandlers`, `contextFuncs`) keyed by `frameIdx`, and the moment the response handler calls `CancelFunc()`, the waiting polling loop detects `context.Canceled` and returns the response — implementing a synchronous/asynchronous bridge using only the standard library, with no condition variables.
- **Separating routing strategy by exchange**: Even for the same `ReqUpdateGameData` event, the broadcast nature of a direct exchange (Gateway, propagated to every instance) is deliberately distinguished from its round-robin nature (Player Service, handled by a single instance) — reflecting the difference in character between stateless Gateway updates and stateful Player Service updates directly in the exchange/queue binding design.
- **Controlling the admin surface via an RPC whitelist**: Any event name not explicitly registered in `adminPLRPCs`/`adminSMRPCs` is immediately rejected at the `HandleAdminRequest` stage, enforcing at the code level that the `/admin` endpoint can't become a channel for indiscriminately invoking arbitrary internal RPCs.
- **Combining connection resilience with topology reconstruction**: When a RabbitMQ connection drops and reconnects, the service doesn't simply reconnect — it also redeclares exchanges and recreates the exclusive return queue, so it recovers to a clean state with no dependency on queue names tied to a previous session, even after a broker restart or network outage.

---

## Tech Stack

- **Language**: Go
- **Messaging**: RabbitMQ (AMQP via `streadway/amqp`)
- **HTTP Routing**: `gorilla/mux`
- **Concurrency**: `context.Context`/`CancelFunc`-based request correlation, `sync.RWMutex`
- **Transport Security**: Optional TLS termination (`http.ListenAndServeTLS`)
- **Notifications**: Zapier webhook integration
