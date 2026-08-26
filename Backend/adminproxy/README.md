# admin-proxy

**Internal network 전용** 게임 백엔드 관리 microservice입니다. HTTP request를 RabbitMQ 기반 RPC로 변환하여 Player Service, Session Manager, Gateway 등 여러 백엔드 서비스에 라우팅하고, 비동기 message queue 응답을 동기적인 HTTP response로 되돌려주는 **request/response bridge** 역할을 수행합니다. 외부에 노출되지 않고 internal network에서만 접근 가능하도록 운영되며, 운영진이 게임 데이터 갱신, 계정 조회/수정, 유지보수 상태 관리 등을 수행하는 admin tool의 backend로 사용됩니다.

---

## Overview

이 서비스는 크게 두 종류의 endpoint를 제공합니다.

1. **Fire-and-forget 방식 관리 endpoint** (`/update`, `/reset`, `/version`) — 응답을 기다리지 않고 RabbitMQ로 broadcast/round-robin message를 발행한 뒤 즉시 `200 OK`를 반환.
2. **동기 RPC endpoint** (`/admin`) — 임의의 admin RPC를 RabbitMQ 너머의 서비스로 전달하고, 해당 서비스의 응답이 돌아올 때까지 HTTP connection을 유지한 뒤 응답 body를 그대로 client에 전달.

```
Admin Client (internal network)
        │  HTTP
        ▼
   admin-proxy  ──── RabbitMQ Exchange (PL / SM / GW / AP) ────▶  Player Service / Session Manager / Gateway
        ▲                                                                    │
        │  frameIdx로 매칭된 context.Value 응답                                │
        └────────────── RabbitMQ return queue (exclusive) ◀───────────────────┘
```

핵심 설계 과제는 **RabbitMQ의 비동기 pub/sub 모델 위에 HTTP의 동기 request/response 모델을 자연스럽게 연동하는 것**이며, 이는 `frameIdx` 기반 `context.Context` correlation 메커니즘으로 해결됩니다.

---

## Architecture

### `main.go` — Entry Point
`admin_proxy.Run(usetls)`를 호출하는 minimal entry point. TLS termination이 별도 계층(예: reverse proxy)에서 처리되므로 기본값은 `false`로 고정되어 있습니다.

### `admin_proxy.go` — HTTP Router & RabbitMQ Connection Lifecycle
- **`connectToRabbitMQ` / `rabbitConnector`**: connection 종료 이벤트(`NotifyClose`)를 channel로 감지하여 자동 재연결하는 supervisor 패턴. 재연결 시마다 `PL`/`GW` exchange를 재선언하고, exclusive/durable return queue를 새로 생성한 뒤 consumer를 등록.
- **응답 소비 loop**: return queue에 도착하는 모든 message를 `model.QueueMessage`로 unmarshal하여 `handler.HandleResponse`로 위임하고, 수동 `Ack`로 명시적 처리 완료를 선언.

**Code: RabbitMQ 재연결 supervisor + consume loop** (`admin_proxy.go`)

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

			// 응답 소비 loop: return queue → model.QueueMessage → handler.HandleResponse
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

- **`/update`**: query parameter(`gw`/`pl`)에 따라 Gateway에는 broadcast(direct exchange의 모든 binding에 전달), Player Service에는 round-robin(단일 consumer가 처리) 방식으로 `ReqUpdateGameData` 메시지를 전달.
- **`/reset`**, **`/version`**: 각각 전체 talent 초기화, 최소 클라이언트 버전 갱신을 위한 fire-and-forget 관리 endpoint.
- **`adminHandler` (`/admin`)**: 범용 admin RPC gateway.
  1. `EventName`/`UserID`를 HTTP header에서 추출.
  2. thread-safe `getFrameIdx()`(`sync.RWMutex` 보호)로 요청마다 고유한 frame index를 발급.
  3. request body를 raw payload로 RabbitMQ에 전달.
  4. `context.WithTimeout`(10초)으로 응답 대기 context를 생성하고, `handler.HandleAdminRequest`에 위임.
  5. 10ms 간격 polling으로 context 취소(응답 도착) 또는 deadline 초과(timeout)를 감지하여 HTTP 응답을 완료.

### `handler.go` — RPC Dispatch Core
- `messageHandlerFn` 타입의 handler registry(`messageHandlers`)로 event name → 처리 함수 매핑을 관리.
- **`HandleAdminRequest`**: event name을 `adminPLRPCs`/`adminSMRPCs` 화이트리스트와 대조하여 목적지 exchange(Player Service 또는 Session Manager)를 결정하고, 미등록 RPC는 명시적으로 거부. 이후 `frameIdx`를 key로 `context.Context`와 `context.CancelFunc`를 각각 저장하여, 비동기로 도착할 응답과 현재 대기 중인 HTTP 요청을 연결할 수 있는 상태를 마련.
- **`HandleResponse`**: 수신한 message의 event name으로 등록된 handler를 조회해 실행하는 단순 dispatcher.
- **`sendMqMsg`**: 모든 발행 message에 `Persistent` delivery mode를 지정하여 RabbitMQ 재시작 시에도 유실 없이 보존.

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

`exchange`와 `key`는 라우팅 전략에 따라 달리 조합됩니다: Gateway broadcast는 `(model.GWExchange, model.APExchange)`, Player Service round-robin은 `(model.PLExchange, model.PLExchange)`처럼 목적지 exchange와 binding key를 동일하게 맞춰 단일 consumer에게 전달합니다.

### `handler_custom.go` — RPC Whitelist & Response Handlers
- `registerCustom()`에서 Profile, Prospect, Notification, Challenges, Inventory, Workshop, Talents, Drop Loadout 등 도메인별로 방대한 admin RPC 목록(`ReqXxx` → `ResXxx`)을 화이트리스트에 등록. 이 목록이 사실상 admin tool이 호출 가능한 API surface의 명세 역할을 함.
- **`onResAdminRequest`**: 화이트리스트에 등록된 모든 RPC의 공통 응답 처리기. `frameIdx`를 header에서 복원하여 해당 요청의 대기 context를 찾고, 에러 header(`error`/`errorMessage`)가 있으면 표준 에러 payload로, 아니면 원본 message body를 그대로 context value에 주입한 뒤 `CancelFunc`를 호출해 대기 중인 HTTP handler의 polling loop를 즉시 깨움.
- **`onResUpdateGameData`** / **`onResResetAllTalents`**: fire-and-forget 계열 요청 전용 응답 처리기로, 성공 여부를 전역 `responded` flag에 반영. 실패 시 Zapier webhook(`sendErrorNotification`)으로 외부 알림을 전송.

### `handler_gen.go`
코드 생성기가 생성한 RPC 등록을 위한 확장 지점. 현재는 비어 있으며, `Register()`가 `registerCustom()`과 `registerGenerated()`를 함께 호출하는 조합 지점 역할.

### `model.go` / `shared.go`
- `model.go`: 이 서비스에 국한된 응답 struct(`ResUpdateGameData`, `ResResetAllTalents`)와 message envelope(`QueueMessage`) 정의.
- `shared.go`: Icarus 백엔드의 모든 Go 서비스가 공유하는 exchange 이름 상수(`PL`, `MM`, `SM`, `EA`, `GW`, `AP`) — 서비스 간 topology 계약의 단일 소스.

### `adminrpc.go`
Admin RPC 전용 header key(`frameidx`)와 parameter 이름 상수, 표준 에러 응답 struct(`ResAdminError`) 정의.

### `parser.go` — STOMP 스타일 커스텀 메시지 인코더/디코더
`WsMessage`는 [STOMP](https://stomp.github.io/)와 유사한 text-frame 구조(`command/event line` → `header lines` → 빈 줄 → `body`)를 직접 구현한 경량 프로토콜입니다. RabbitMQ 위에서 오가는 `model.QueueMessage`(JSON)와는 별개로, WebSocket 등 다른 transport와의 연동을 위해 사용되는 것으로 보이는 frame 포맷입니다.

```
EventName
header1:value1
header2:value2

<raw event payload bytes>
```

- **`Encode()`**: event name → header 목록 → 빈 줄 → payload 순으로 이어붙여 byte slice를 생성.
- **`Decode()`**: 첫 줄을 event name으로, 이어지는 `key:value` 줄들을 header map으로 파싱하다가 빈 줄을 만나면 header 파싱을 종료. `datalength` header가 존재하면 이를 신뢰해 전체 buffer의 뒤에서부터 해당 길이만큼을 payload로 잘라내며(embedded newline이나 null byte를 포함한 binary-safe payload 지원), 없으면 남은 줄들을 이어붙여 payload로 사용.

**Code: STOMP 스타일 frame encoder/decoder** (`parser.go`)

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

> **Note**: `admin_proxy`, `handler`, `handler_custom` 등 이 저장소에 포함된 코드에서는 `parser.WsMessage`를 직접 사용하는 지점이 보이지 않습니다. `model.QueueMessage`(JSON)가 RabbitMQ 메시지 envelope로 사용되고 있어, `parser.go`는 다른 서비스(예: Gateway ↔ client WebSocket 연동)와 공유되는 유틸리티 패키지이거나, 향후/다른 transport 통합을 위한 코드로 추정됩니다.

---

## 주요 설계 세부 사항

- **`context.Context`를 message correlation ID로 활용**: 표준 라이브러리의 `context` 패키지를 request timeout뿐 아니라 **비동기 RPC 응답을 특정 HTTP 요청에 매칭시키는 pub/sub correlation 메커니즘**으로 전용(轉用). `frameIdx`를 key로 하는 global map(`frameHandlers`, `contextFuncs`)에 각 요청의 `Context`/`CancelFunc`를 등록해두고, 응답 handler가 `CancelFunc()`를 호출하는 순간 대기 중이던 polling loop가 `context.Canceled`를 감지해 응답을 반환 — condition variable 없이 표준 라이브러리만으로 동기/비동기 브릿지를 구현.
- **Exchange 단위의 라우팅 전략 분리**: 동일한 `ReqUpdateGameData` 이벤트라도 목적지에 따라 direct exchange의 broadcast 특성(Gateway, 모든 인스턴스에 전파)과 round-robin 특성(Player Service, 단일 인스턴스가 처리)을 의도적으로 구분해서 사용 — 상태를 갖지 않는 Gateway 갱신과 상태를 가진 Player Service 갱신의 성격 차이를 exchange/queue 바인딩 설계로 반영.
- **RPC whitelist를 통한 admin surface 통제**: `adminPLRPCs`/`adminSMRPCs`에 명시적으로 등록되지 않은 event name은 `HandleAdminRequest` 단계에서 즉시 거부되어, `/admin` endpoint가 임의의 내부 RPC를 무분별하게 호출할 수 있는 통로가 되지 않도록 API surface를 코드 레벨에서 강제.
- **Connection resilience와 topology 재구성의 결합**: RabbitMQ connection이 끊어졌다가 재연결될 때 단순 재접속에 그치지 않고 exchange 선언과 exclusive return queue 재생성까지 함께 수행하여, broker 재시작이나 네트워크 단절 이후에도 이전 세션에 종속된 큐 이름 없이 깨끗한 상태로 복구.

---

## Tech Stack

- **Language**: Go
- **Messaging**: RabbitMQ (AMQP via `streadway/amqp`)
- **HTTP Routing**: `gorilla/mux`
- **Concurrency**: `context.Context`/`CancelFunc` 기반 request correlation, `sync.RWMutex`
- **Transport Security**: 선택적 TLS termination (`http.ListenAndServeTLS`)
- **Notifications**: Zapier webhook 연동
