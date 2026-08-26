# lobbystats

**RabbitMQ Management API**를 polling하여 백엔드 대기열 상태를 계산하고, **STOMP protocol**로 게임 클라이언트에 실시간 broadcast하는 Go 기반 백엔드 microservice입니다. Unreal Engine 클라이언트 측 `OnlineSubsystemIcarus`의 `IcarusLobbyConnectionComponent`가 구독하는 `ResLobbyStats` 이벤트를 생성하는 서버 측 counterpart입니다.

---

## Overview

`lobbystats`는 단일 책임을 갖는 작은 서비스입니다: RabbitMQ의 특정 큐(`lobby`)에 쌓인 대기 인원과 처리 속도를 주기적으로 조회하고, 이를 게임 클라이언트가 소비할 수 있는 형태로 재가공하여 같은 RabbitMQ에 STOMP로 다시 publish합니다.

```
RabbitMQ Management HTTP API  ──(polling)──▶  lobbystats
                                                   │
                                                   ▼
                                        model.ResLobbyStats 생성
                                                   │
                                                   ▼
RabbitMQ (STOMP) ◀──(WsMessage encoding)── lobbystats
       │
       ▼
게임 클라이언트 (Unreal OSS Lobby Connection Component)
```

클라이언트와 서버가 **동일한 프레임 format**(`EventName\nheader:value\n\nBody`)을 공유하는 것이 핵심 설계 포인트로, `parser.WsMessage`가 이 encoding을 담당하며 이는 클라이언트 측 `FIcarusWSFrame`과 wire-compatible한 대응 구현체입니다.

---

## Architecture

### `main.go` — Entry Point
`lobbystats.Run()` 하나만 호출하는 최소 entry point로, 실제 로직은 모두 `lobbystats` 패키지에 캡슐화되어 있습니다.

### `lobbystats.go` — Core Service Logic
- **`connectToRabbitMQStomp`**: TLS 기반 STOMP connection 수립을 담당하며, 연결 실패 시 무한 retry loop(2초 간격)로 재시도합니다. `usetls` 플래그로 로컬 개발 환경에서는 TLS 없는 plain TCP 연결로 전환 가능합니다.
- **`lobbyConnector`**: 서비스의 핵심 워크플로우를 goroutine으로 구동합니다.
  1. `conf.LobbyUpdateInterval` 주기로 RabbitMQ Management HTTP API를 polling.
  2. 응답 JSON을 `model.LobbyStats`로 unmarshal하여 큐 depth(`messages`)와 delivery rate(`deliver_get_details.rate`)를 추출.
  3. 이를 `model.ResLobbyStats`로 재구성하고, config에 설정된 maintenance 상태(시작/종료 시간, 안내 메시지)를 함께 병합.
  4. `parser.WsMessage`로 encoding한 뒤 STOMP `Send`를 통해 지정된 destination으로 broadcast.
- **`Run`**: config 로딩, `mux` router 초기화, lobby connector goroutine 기동, 그리고 `/health` health check endpoint를 포함한 HTTP 서버(`:7071`)를 구동합니다.

폴링 → 변환 → STOMP broadcast로 이어지는 핵심 루프는 다음과 같습니다:

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

STOMP connection 수립은 TLS/plain 두 경로로 나뉘며, 각각 독립적인 무한 retry loop을 가집니다:

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
- `LobbyStats` / `MessageStats` / `MessageDetails` — RabbitMQ Management API의 JSON 응답 스키마를 그대로 반영한 struct로, `message_stats.deliver_get_details.rate` 같은 nested field를 직접 매핑.
- `ResLobbyStats` / `MaintenanceStatus` — 클라이언트에 전송되는 실제 payload. 필드명(`queueSize`, `messagesReadyRate`, `Maintenance`)이 Unreal 클라이언트 측 `FResLobbyStats` UStruct와 1:1로 대응하도록 설계됨.
- `QueueMessage` — STOMP 전송 전 단계에서 사용되는 내부 wrapper.

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
- `WsMessage.Encode()`는 `EventName\nheader:value\n\nBody` 형태의 커스텀 프레임을 byte-level로 직접 조립.
- `WsMessage.Decode()`는 역방향 파싱을 구현하며, 특히 `datalength` header가 존재하는 경우 개행(`\n`) 기준 split이 아닌 **정확한 byte offset 기반 slicing**으로 body를 추출하여, binary-safe하지 않은 순수 라인 파싱의 한계를 보완.
- `Parsable` interface로 `Encode`/`Decode` 계약을 추상화하여 향후 다른 메시지 타입으로의 확장을 고려한 구조.

---

## 주요 설계 세부 사항

- **Polling-to-push bridge 패턴**: RabbitMQ Management API는 pull 방식(HTTP polling)만 제공하지만, 게임 클라이언트는 push 방식(STOMP subscription)의 실시간 업데이트가 필요합니다. `lobbystats`는 이 두 모델 사이의 격차를 메우는 adapter 역할을 수행하며, 폴링 주기(`LobbyUpdateInterval`)를 config로 분리해 서버 부하와 클라이언트 반응성 사이의 trade-off를 운영 중 조정 가능하게 설계.
- **Connection resilience**: TLS handshake와 STOMP connect 각각에 독립적인 retry loop을 두어, 네트워크 계층과 protocol 계층의 실패를 분리해서 처리. 이는 클라이언트 측 `UIcarusConnectionComponentBase`의 exponential backoff reconnect와 대칭을 이루는 서버 측 resilience 전략.
- **Wire protocol symmetry**: `parser.WsMessage`는 Unreal 클라이언트의 `FIcarusWSFrame`과 독립적으로 구현되어 있지만 동일한 frame layout을 준수하도록 설계되어, 두 코드베이스가 별도 언어(Go/C++)와 별도 repository에 있음에도 protocol level에서 상호운용성을 보장.
- **Maintenance 상태 in-band 전달**: 별도의 API call 없이 매 통계 broadcast에 maintenance 정보를 함께 실어보내, 클라이언트가 대기열 정보를 받는 동일한 channel에서 서비스 점검 여부까지 판단할 수 있도록 설계.

---

## Tech Stack

- **Language**: Go
- **Messaging**: RabbitMQ (STOMP protocol via `go-stomp/stomp`), RabbitMQ Management HTTP API
- **HTTP Routing**: `gorilla/mux`
- **Transport Security**: TLS (`crypto/tls`)
- **Serialization**: `encoding/json`, 커스텀 byte-level frame codec
