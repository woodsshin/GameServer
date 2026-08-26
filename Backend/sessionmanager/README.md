# sessionmanager

Icarus 백엔드의 세션/진행도 저장소 역할을 하는 Go microservice입니다. 게임 클라이언트가 보낸 인게임 로드아웃, 진행 중인 게임(Prospect) 상태, 추적 통계(tracked stats)를 Redis Cluster에 저장·조회하며, RabbitMQ를 통해 다른 서비스와 비동기로 통신합니다.

## 개요

- **역할**: "지금 이 캐릭터가 무엇을 들고 있는지, 어떤 게임(Prospect)에 있는지, 누가 호스트인지"를 담당하는 상태 저장 계층. Gateway를 거쳐 들어온 클라이언트 요청과, Player Service 등 다른 백엔드 서비스의 RPC 요청 양쪽에서 참조됩니다.
- **통신 방식**: RabbitMQ(AMQP) 메시지 브로커를 통한 비동기 request/response
- **데이터 저장소**: Redis Cluster (`go-redis/v8`)
- **Health Check**: `/health` HTTP 엔드포인트를 별도로 노출

## 아키텍처

```
┌────────────┐   MM Exchange     ┌──────────────────┐    HGet/HSet/Pipeline    ┌───────────────┐
│  Gateway /  │ ────────────────▶ │                   │ ───────────────────────▶│               │
│  다른 서비스 │   (RabbitMQ)      │  sessionmanager    │                          │ Redis Cluster │
└────────────┘ ◀──────────────── │  (SM Exchange)     │                          │               │
                응답(QueueMessage) └──────────────────┘                          └───────────────┘
```

- `SM` exchange/queue를 선언·바인딩하고 메시지를 consume, 처리 후 요청에 담긴 `queueName`으로 응답을 되돌려 보냅니다.
- `frameidx` 헤더를 요청→응답 간 그대로 복사해, 호출 측(`adminproxy`류 서비스)이 비동기 응답을 원래 요청과 correlation 할 수 있게 합니다.
- 연결이 끊기면 `rabbitConnector`가 exchange/queue/binding을 처음부터 다시 선언하며 재연결합니다 (broker 재시작에도 상태 일관성 유지).

## 처리하는 이벤트

코드 생성 기반 핸들러 레지스트리(`handler_gen.go`)에 등록된 이벤트들입니다.

| 이벤트 | 설명 |
|---|---|
| `ReqGetCharacterLoadout` / `ReqUpdateCharacterLoadout` | 캐릭터 슬롯별 인게임 로드아웃(장비/백팩/퀵바 구성) 조회·저장 |
| `ReqUpdateTrackedStats` | Prospect 진행 중 누적되는 추적 통계 저장 |
| `ReqGetAllProspects` / `ReqGetProspect` / `ReqUpdateProspect` | 진행 중인 게임(Prospect)의 목록 조회, 단건 조회, 상태(경과시간, 액터 blob 등) 갱신 |
| `ReqResumeProspect` | 세션 재접속 시 호스트 재할당 및 락 처리 후 게임 상태 재개 |
| `ReqHostCandidate` | 호스트 마이그레이션 후보 등록 |
| `ReqBackToHab` | Prospect 종료 후 거점(Hab) 복귀 시 진행도 반영 |

## 핵심 설계

### 1. 코드 생성 기반 3계층 핸들러 구조
- `handler_gen.go` — 이벤트 이름과 핸들러 함수 간의 매핑, 요청 역직렬화/응답 직렬화 로직을 스키마로부터 자동 생성. 시그니처와 디스패치 코드만 담당하며 실제 로직은 별도 파일에 위임합니다.
- `handler_impl.go` — 실제 비즈니스 로직(`on*Impl` 함수들)을 담은 수동 구현 계층으로, 코드 재생성 이후에도 보존됩니다.
- `handler_custom.go` — 생성기가 다루지 않는 예외적 등록·오버라이드를 위한 확장 지점(현재는 빈 훅으로 존재).

이 구조는 요청/응답 스키마가 변경될 때 디스패치 및 (역)직렬화 코드를 생성기로 재생성하면서도, 사람이 작성한 비즈니스 로직 계층은 영향받지 않도록 분리한 것이 핵심입니다.

### 2. Redis Hash 기반 상태 모델
- 캐릭터 상태 키: `{userID}_{chrSlot}` 해시에 `bHost`, `joinedProspect`, `loadout`(JSON 직렬화) 등을 필드로 저장
- Prospect 상태 키: `{prospectID}` 해시에 `hostID`, `hostUpdatedTime`, `isExpired`, `elapsedTime`, 압축된 액터 blob(`actorsData`, `actorsHash` 등)을 저장
- 모든 갱신에서 `TxPipeline`으로 관련 필드를 하나의 트랜잭션으로 묶어 처리하고, `context.WithTimeout`으로 Redis 왕복 지연을 방어

```go
// GetCharacterLoadout ...
func GetCharacterLoadout(request model.ReqGetCharacterLoadout) (model.ResGetCharacterLoadout, error) {
	var resLoadout model.ResGetCharacterLoadout

	userChrKey := fmt.Sprintf("%v_%v", request.UserID, request.ChrSlot)

	bLoadoutVal, err := client.HGet(ctx, userChrKey, "loadout").Result()
	if err != nil {
		if err == redis.Nil {
			return model.ResGetCharacterLoadout{Loadout: model.CharacterLoadout{Valid: true}}, nil
		}
		return model.ResGetCharacterLoadout{Loadout: model.CharacterLoadout{Valid: false}}, errors.New("Not available loadout")
	}

	err = json.Unmarshal([]byte(bLoadoutVal), &resLoadout.Loadout)
	if err != nil {
		return model.ResGetCharacterLoadout{Loadout: model.CharacterLoadout{Valid: false}}, errors.New("Invalid loadout")
	}

	return resLoadout, nil
}

// UpdateCharacterLoadout ...
func UpdateCharacterLoadout(socketID string, request model.ReqUpdateCharacterLoadout) error {
	if socketID != request.UserID {
		return errors.New("Invalid userId")
	}

	bLoadout, err := json.Marshal(request.Loadout)
	if err != nil {
		return errors.New("Invalid loadout")
	}

	userChrKey := fmt.Sprintf("%v_%v", request.UserID, request.ChrSlot)

	err = client.HSet(ctx, userChrKey, "loadout", bLoadout).Err()
	if err != nil {
		return errors.New("Failed to update loadout")
	}
	return nil
}
```

Prospect 상태를 갱신하는 경로는 여러 필드를 한 번에 반영해야 하므로 `TxPipeline`을 사용합니다:

```go
pipe := client.TxPipeline()

// ... 유효성 검증 이후 ...

pipe.HSet(ctx, prospectID,
    "hostUpdatedTime", updatedTime,
    "lastClientUpdatedTime", updateProspect.UpdateTime,
)

timeout, cancel := context.WithTimeout(context.Background(), 1*time.Second)
defer cancel()
_, err = pipe.Exec(timeout)

if ctx.Err() == context.Canceled || ctx.Err() == context.DeadlineExceeded {
    return ctx.Err(), model.UpdateProspectFailure_UndefinedID
}
```

### 3. 호스트 마이그레이션 / 재접속 동시성 제어
- `ResumeProspect`는 `HSetNX` 기반 분산 락(`{prospectID}_lock`)으로 동시 재접속 요청 중 하나만 임계 구역에 진입하도록 제어하고, TTL(`Expire`)로 락이 영구히 남지 않도록 보장
- 마지막 호스트 갱신 이후 30초 이상 지났으면 다른 클라이언트가 새 호스트로 승격 가능 — 호스트가 죽었을 때 세션이 멈추지 않도록 하는 타임아웃 기반 페일오버
- `AttemptHostMigration` 플래그가 있으면 사전에 등록된 `candidateID`와 일치하는 사용자만 호스트를 넘겨받을 수 있음

```go
// Try and get the lock to the DB, only one request can access the remainder of this call at a time
totalLockTime := 30
var timeSpentLocked int
lockKey := request.ProspectID + "_" + "lock"
for {
	lockSet, err := client.HSetNX(timeout, lockKey, "locked", true).Result()
	if err != nil {
		return response, err
	}
	if lockSet {
		break
	}

	time.Sleep(1 * time.Second)
	timeSpentLocked++
	if timeSpentLocked > totalLockTime {
		return response, errors.New("failed to get lock on redis")
	}
}

err := client.Expire(ctx, lockKey, time.Duration(totalLockTime)*time.Second).Err()
defer func() { client.HDel(ctx, lockKey, "locked") }()
```

락 획득 이후에는 마지막 호스트 갱신 시각과의 경과 시간, 그리고 호스트 마이그레이션 후보 여부로 호스트 승격 대상을 판단합니다:

```go
// Allow a new host to take over if the timeSinceLastUpdate is above a certain threshold
timeSinceLastUpdate := updatedTime - hostUpdatedTime
shouldHost := response.HostID == socketID || timeSinceLastUpdate >= 30

// Check if the user is the host candidate and is trying to migrate host
if request.AttemptHostMigration {
	candidateID, _ = client.HGet(ctx, request.ProspectID, "candidateID").Result()
	shouldHost = candidateID == socketID
}
```

### 4. 데이터 무결성 검증
- `UpdateProspect`에서 클라이언트가 보낸 압축 액터 blob을 `zlib`로 해제한 뒤 SHA-1 해시를 재계산해 요청에 담긴 해시와 대조 — 전송 중 손상/변조된 상태 데이터가 저장되는 것을 차단
- `lastClientUpdatedTime` 비교로 오래된(stale) 업데이트 요청은 무시해 순서가 뒤바뀐 메시지가 최신 상태를 덮어쓰지 않도록 방지

```go
if updateProspect.HasProspectBlob {
	blob, err := compress.Unzip(updateProspect.ProspectBlob.BinaryBlob)
	if err != nil {
		return errors.New("failed to decompress data"), model.UpdateProspectFailure_Other
	}

	hash := sha1.New()
	hash.Write(blob)
	if hex.EncodeToString(hash.Sum(nil)) != updateProspect.ProspectBlob.Hash {
		return errors.New("hashes do not match"), model.UpdateProspectFailure_Other
	}
	// ... 검증을 통과한 경우에만 pipe.HSet으로 저장 ...
}
```

오래된 요청을 걸러내는 부분은 다음과 같습니다:

```go
lastClientUpdatedTimeVal, err := client.HGet(ctx, prospectID, "lastClientUpdatedTime").Result()
if err == nil {
	lastClientUpdatedTime, _ := strconv.ParseInt(lastClientUpdatedTimeVal, 0, 64)
	if lastClientUpdatedTime >= updateProspect.UpdateTime {
		return nil, model.UpdateProspectFailure_NotNewer
	}
}
```

## 기술 스택

- **언어**: Go
- **메시징**: RabbitMQ (`streadway/amqp`), `SM` exchange
- **저장소**: Redis Cluster (`go-redis/v8`), Hash 기반 스키마, 클라이언트 사이드 파이프라이닝
- **압축/무결성**: zlib 압축(`compress/zlib`), SHA-1 해시 검증
- **웹**: `gorilla/mux` (health check 엔드포인트 전용)
- **직렬화**: `encoding/json`

## 설계 포인트

- **메시지 브로커 기반 비동기 RPC**: 모든 요청·응답이 RabbitMQ 메시지로 오가며, `frameidx` correlation 헤더를 통해 비동기 pub/sub 모델 위에서 요청-응답 페어링을 구현
- **분산 락 + 타임아웃 기반 단일 호스트 보장**: 분산 환경에서 여러 클라이언트가 동시에 호스트 권한을 주장할 수 없도록 Redis 락과 시간 기반 failover를 조합
- **코드 생성 기반 유지보수**: 이벤트 스키마가 추가·변경돼도 `handler_gen.go`/`model_gen.go`를 재생성하는 것만으로 디스패치·직렬화 계층에 반영되고, 비즈니스 로직(`handler_impl.go`)은 재생성 대상에서 분리되어 안전하게 유지