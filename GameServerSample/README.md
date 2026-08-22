# C++ 비동기 게임 서버 (Network / Async / MySQL 포트폴리오)

epoll 기반 비동기 I/O, DB 커넥션 풀, 스레드 분리 아키텍처로 구현한 소규모 게임 서버입니다.
계정 생성, 로그인, 채팅 브로드캐스트, 대규모 동시 접속 처리를 지원하는 서버 아키텍처를
직접 설계 및 구현하여 네트워크 프로그래밍, 비동기 처리, 데이터베이스 연동 역량을
증명하는 것을 목표로 합니다.

## 핵심 설계 포인트

### 1. Accept 스레드 / IO 워커 스레드 분리
```
[Accept Thread]  --accept()--> 라운드로빈 분배 --> [IoWorker 0] [IoWorker 1] ... [IoWorker N]
                                                       (epoll)      (epoll)         (epoll)
```
- Accept 전용 스레드가 `listen` 소켓만 전담하여 감시합니다. 워커 스레드가 트래픽 처리로 부하가 높은 상태에서도 신규 연결 수락(accept)이 지연되지 않습니다.
- 각 IoWorker는 **독립된 epoll 인스턴스**를 보유합니다. 단일 epoll 인스턴스에 전체 파일 디스크립터를 모아 처리하는 구조와 비교해 락 경합이 발생하지 않으며, CPU 코어 수에 비례한 수평 확장(`std::thread::hardware_concurrency()` 기준 워커 수 결정)이 가능합니다.
- 신규 소켓 등록, 쓰기 요청 등 스레드 경계를 넘나드는 통지는 `eventfd`와 `epoll_wait`를 조합해 처리하여, 워커 스레드가 불필요한 폴링 없이 이벤트 기반으로만 활성화됩니다.

### 2. DB 비동기 처리 (Head-of-Line Blocking 회피)
IoWorker 스레드가 로그인/회원가입 처리 중 블로킹 방식의 MySQL 쿼리를 직접 호출할 경우, 해당 스레드에 연결된 **다른 모든 소켓**의 이벤트 처리가 쿼리 응답 대기 시간만큼 지연됩니다(Head-of-Line Blocking). 이를 방지하기 위해 다음과 같은 파이프라인을 구성했습니다.

```
IoWorker 스레드: 패킷 파싱 → DBTask(람다) 생성 → DBWorkerPool 큐에 Enqueue → 즉시 다음 이벤트 처리
DB 워커 스레드:   큐에서 Task Pop → 커넥션 풀에서 커넥션 대여 → 쿼리 실행 → NetServer::SendPacket으로 응답
```
- `DBConnectionPool`: `MYSQL*` 커넥션 N개를 사전에 생성해두는 풀과, RAII 가드(`ScopedConnection`)를 통한 반납 누락 방지 로직으로 구성됩니다.
- `DBWorkerPool`: `ThreadSafeQueue<DBTask>` 기반의 작업 큐와 워커 스레드 풀로 구성되며, Prepared Statement를 사용해 SQL Injection을 차단합니다.
- `NetServer::SendPacket`은 호출 스레드에 관계없이 안전하게 동작합니다. 세션의 송신 큐는 mutex로 보호되며, 대상 세션이 속한 워커에는 `eventfd`를 통해 쓰기 이벤트가 통지됩니다.

### 3. 패킷 프로토콜 및 스트림 처리
- `[4B TotalSize][2B PacketType][Body]` 구조의 길이 기반 바이너리 프로토콜을 사용합니다.
- TCP는 스트림 기반 프로토콜이므로 한 번의 `recv` 호출에 여러 패킷이 결합되어 수신되거나, 하나의 패킷이 여러 번에 걸쳐 분할 수신될 수 있습니다. 이를 처리하기 위해 `Session::RecvBuffer()`에 데이터를 누적하고, 완성된 패킷만 추출하여 디스패치하며 나머지는 다음 `recv` 호출까지 보존합니다.
- 부분 전송(`send` 호출 시 요청한 크기만큼 전송되지 않는 경우, short write)도 `EPOLLOUT` 이벤트를 기반으로 이어서 처리됩니다.

### 4. 동시성 안전 세션 관리
- `SessionManager`는 `std::shared_mutex`를 사용합니다. 채팅 브로드캐스트와 같이 "전체 순회 + 읽기" 연산이 빈번한 경우 `shared_lock`으로 동시 진입을 허용해 처리량을 확보하고, 세션 추가/삭제와 같이 컨테이너 구조가 변경되는 연산에만 `unique_lock`을 적용합니다.
- 브로드캐스트 시 락을 보유한 상태로 `send()`까지 수행하지 않도록, 세션 목록의 스냅샷만 획득한 뒤 즉시 락을 해제하고 순회하며 전송합니다(락 보유 시간 최소화).

### 5. 보안 및 안정성
- 비밀번호는 계정별 랜덤 salt와 SHA-256 스트레칭(10,000라운드)을 적용하여 저장하며, 평문 비교 로직은 존재하지 않습니다.
- 모든 쿼리 파라미터는 Prepared Statement로 바인딩하여 SQL Injection을 방지합니다.
- 하트비트 미수신 세션은 60초 타임아웃 기준으로 감지되어 강제 종료 통지가 전송됩니다.
- 비정상적인 패킷 크기 또는 과도한 recv 버퍼 누적이 감지되면 연결을 강제 종료합니다(악성/오작동 클라이언트 방어).
- `SIGPIPE` 시그널을 무시 처리하여 클라이언트의 비정상 종료가 서버 프로세스 종료로 이어지지 않도록 합니다.

## 디렉토리 구조
```
include/
  PacketDef.h          # 패킷 헤더 및 직렬화(Writer/Reader)
  ThreadSafeQueue.h     # 범용 스레드 세이프 큐 (DB 작업 큐 등에 사용)
  DBConnectionPool.h    # MySQL 커넥션 풀 (RAII 가드 포함)
  DBWorker.h             # DB 비동기 작업 큐 및 워커 스레드 풀
  Session.h              # 클라이언트 세션 (수신 버퍼, 송신 큐)
  SessionManager.h       # 전체 세션 컨테이너 (shared_mutex)
  NetServer.h            # epoll 기반 네트워크 코어 (Accept/IoWorker)
  PasswordHash.h         # SHA-256 + salt 해싱
  GameServer.h            # 비즈니스 로직 계층 (회원가입/로그인/채팅)
src/
  NetServer.cpp
  GameServer.cpp
  main.cpp
  TestClient.cpp          # 동작 검증용 테스트 클라이언트
sql/
  schema.sql               # users, chat_logs 테이블
tools/
  load_test.py              # 비동기 부하 테스트 클라이언트 (수백~수천 동시 접속 시뮬레이션)
  run_full_load_test.sh     # MySQL 기동+서버 실행+단계별 부하테스트를 한 번에 처리하는 러너
report/
  load_test.py           # 부하테스트 스크립트
CMakeLists.txt
```

## 빌드 방법

### 의존성 설치 (Ubuntu/Debian)
```bash
sudo apt-get install -y build-essential cmake libmysqlclient-dev libssl-dev
```

### 빌드
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```
빌드가 완료되면 `game_server`(서버 본체)와 `test_client`(검증용 클라이언트) 두 실행 파일이 생성됩니다.

### DB 준비
```bash
mysql -u root < sql/schema.sql
mysql -u root -e "CREATE USER 'gameapp'@'127.0.0.1' IDENTIFIED BY 'yourpassword'; \
                   GRANT ALL PRIVILEGES ON game_server.* TO 'gameapp'@'127.0.0.1'; FLUSH PRIVILEGES;"
```

### 실행
```bash
DB_HOST=127.0.0.1 DB_USER=gameapp DB_PASSWORD=yourpassword DB_NAME=game_server \
  ./build/game_server 9000
```
환경변수를 설정하지 않으면 기본값(`root` 계정, 빈 비밀번호, `game_server` 데이터베이스)으로 접속을 시도합니다.

### 동작 확인
```bash
./build/test_client 127.0.0.1 9000 <username>
```
접속, 회원가입, 로그인, 채팅 전송까지 자동으로 수행되며 서버 응답이 콘솔에 출력됩니다.
여러 터미널에서 각기 다른 username으로 동시에 실행하면 브로드캐스트 동작을 확인할 수 있습니다.

## 검증 결과 (로컬 통합 테스트)
- 클라이언트 3개를 동시 접속시켜 회원가입, 로그인, 채팅 브로드캐스트가 정상 동작함을 확인했습니다.
- 한 클라이언트가 전송한 채팅 메시지가 동시 접속 중인 다른 클라이언트에게 실시간으로 브로드캐스트됨을 확인했습니다.
- `chat_logs` 테이블에 한글 메시지가 UTF-8로 정확히 적재됨을 HEX 덤프로 확인했습니다.
- 하트비트 타임아웃(60초) 로직이 정상적으로 세션을 감지하고 `S2C_FORCE_DISCONNECT` 패킷을 전송함을 확인했습니다.
- `-Wall -Wextra -pthread` 옵션 기준 경고 없이 빌드됨을 확인했습니다.

## 부하 테스트: 대용량 동시 접속 처리 검증

`tools/load_test.py`(자체 제작 비동기 부하 생성기)로 실제 서버를 빌드·기동한 뒤
100 ~ 1,000명 동시 접속 시나리오를 직접 실측했습니다. 자세한 방법론과 전체 수치는
[`LOAD_TEST_REPORT.md`](./LOAD_TEST_REPORT.md)에 정리되어 있으며, 핵심 결과는 다음과 같습니다.

- **접속(epoll) 및 채팅 브로드캐스트 계층**: 1,000 동시 접속까지 성공률 100%, 지연시간
  p50 1ms 미만을 유지 — 설계 의도대로 병목이 되지 않음을 확인했습니다.
- **회원가입/로그인 처리량이 동시 접속 수와 무관하게 27.5 req/sec로 고정**되는 현상을
  발견하고, 원인이 `PasswordHash.h`의 SHA-256 스트레칭(10,000라운드, 1회 약 15.7ms
  소요)에 따른 CPU 포화임을 워커 스레드 증량 테스트와 라운드 축소 대조군 테스트로
  교차 검증했습니다.
- 스트레칭 라운드를 1,000으로 낮춘 대조군에서는 1,000명 동시 접속 기준 회원가입
  성공률 92.3% → 100%, 지연 p50 7.65초 → 0.54초(약 14배 개선)로 확인되어, 병목 지점과
  개선 효과를 실측 수치로 증명했습니다.

### 재현 방법
```bash
# 서버가 이미 떠 있는 상태에서 부하 테스트만 실행
python3 tools/load_test.py --host 127.0.0.1 --port 9000 \
    --clients 1000 --ramp-seconds 5 --chat-per-client 3 --out result.json

# MySQL 기동 확인 -> 서버 실행 -> 100/300/600/1000명 단계별 부하 -> 결과 저장까지 한 번에
LOAD_TEST_STAGES="100 300 600 1000" \
  ./tools/run_full_load_test.sh 9000 127.0.0.1 gameapp yourpassword game_server
```

## 개선이 필요한 사항 (실무 대비 트레이드오프)
포트폴리오 목적상 명확성을 우선하여 단순화한 부분이며, 실제 프로덕션 환경 적용 시의 개선 방향을 함께 명시합니다.

| 항목 | 현재 구현 | 실무 개선 방향 |
|---|---|---|
| epoll 트리거 방식 | Level-triggered | Edge-triggered(EPOLLET) 방식과 논블로킹 루프를 적용하여 시스템 콜 호출 횟수를 절감 |
| 브로드캐스트 버퍼 | 세션마다 벡터를 복사하여 전송 | `shared_ptr<vector<char>>`로 버퍼를 공유하여 복사 비용을 제거 |
| DB 재연결 | 미구현 (연결 끊김 발생 시 쿼리 실패) | 쿼리 실패를 감지한 뒤 명시적으로 재연결을 수행하는 로직 추가 |
| 하트비트 강제 종료 | 종료 통지만 전송하며, 실제 소켓 종료는 클라이언트의 자율적 연결 해제에 의존 | IoWorker에 강제 종료 큐를 별도로 두어 서버가 주도적으로 소켓을 종료하도록 개선 |
| 비밀번호 해싱 | SHA-256 + salt + 스트레칭 10,000라운드 (실측: 1회 약 15.7ms, 1,000 동시 접속 시 회원가입 처리량이 27.5 req/sec로 고정되는 CPU 병목 확인됨 — [부하 테스트 리포트](./LOAD_TEST_REPORT.md) 참고) | bcrypt 또는 Argon2 등 검증된 전용 해싱 라이브러리로 대체, 또는 스트레칭 라운드를 보안 요구 수준에 맞게 하향 조정 |

## 적용된 기술
- **네트워크**: epoll 기반 비동기 논블로킹 I/O, 다중 epoll 인스턴스를 통한 워커 수평 확장, eventfd를 이용한 스레드 간 통지, 부분 송수신(short read/write) 처리, TCP_NODELAY 적용 근거에 대한 이해.
- **비동기 설계**: Head-of-Line Blocking을 회피하기 위해 블로킹 방식의 DB 호출을 별도 워커 풀로 분리한 설계 의도와 트레이드오프에 대한 설명 능력.
- **동시성 제어**: `shared_mutex`를 활용한 읽기 위주 연산 최적화, 락 보유 시간 최소화(스냅샷 후 락 해제) 패턴, RAII 기반 리소스 관리(커넥션 풀, 세션 종료).
- **데이터베이스**: Prepared Statement 적용, 커넥션 풀링, 조회 패턴을 고려한 인덱스 설계(`idx_created_at` 등).
- **보안**: salt 및 해시 스트레칭 적용, SQL Injection 방지, 악성 패킷에 대한 방어(크기 검증, 버퍼 오버플로우 방지).
- **성능 분석 및 검증**: 자체 제작 비동기 부하 생성기로 실제 서버를 기동해 대용량 동시 접속을 실측, 병목 지점(CPU 바운드 vs I/O 바운드)을 워커 스레드 증량 테스트와 대조군 비교로 교차 검증하는 성능 프로파일링 방법론.
