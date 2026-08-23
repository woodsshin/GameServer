# C++ 비동기 게임 서버 (Network / Async / MySQL 포트폴리오)

epoll 기반 비동기 I/O, DB 커넥션 풀, 스레드 분리 아키텍처로 구현한 소규모 게임 서버입니다.
계정 생성, 로그인, 채팅 브로드캐스트, 대규모 동시 접속 처리를 지원하는 서버 아키텍처를
설계 및 구현하는 것을 목표로 합니다.

## 핵심 설계 포인트

### 1. Accept 스레드 / IO worker 스레드 분리
```
[Accept Thread]  --accept()--> 라운드로빈 분배 --> [IoWorker 0] [IoWorker 1] ... [IoWorker N]
                                                       (epoll)      (epoll)         (epoll)
```
- Accept 전용 스레드가 `listen` 소켓만 전담하여 감시합니다. worker 스레드가 트래픽 처리로 부하가 높은 상태에서도 신규 연결 수락(accept)이 지연되지 않습니다.
- 각 IoWorker는 **독립된 epoll 인스턴스**를 보유합니다. 단일 epoll 인스턴스에 전체 파일 디스크립터를 모아 처리하는 구조와 비교해 락 경합이 발생하지 않으며, CPU 코어 수에 비례한 수평 확장(`std::thread::hardware_concurrency()` 기준 worker 수 결정)이 가능합니다.
- 신규 소켓 등록, 쓰기 요청, 강제 종료 요청 등 스레드 경계를 넘나드는 통지는 `eventfd`와 `epoll_wait`를 조합해 처리하여, worker 스레드가 불필요한 폴링 없이 이벤트 기반으로만 활성화됩니다.
- 소켓은 **Edge-triggered(EPOLLET)** 모드로 등록되며, `HandleReadable`/`HandleWritable`이 이벤트가 발생할 때마다 `EAGAIN`이 반환될 때까지 read/write를 반복하는 논블로킹 루프로 처리합니다. Level-triggered 방식은 처리되지 않은 데이터가 남아있는 한 `epoll_wait`가 동일한 fd를 계속 반환하지만, Edge-triggered는 상태가 변화하는 시점에 한 번만 통지하므로 동일 fd에 대한 `epoll_wait` 반환 및 관련 시스템 콜 호출 횟수를 줄일 수 있습니다. 신규 쓰기 요청을 Edge-triggered로 등록한 직후에는 소켓 버퍼에 여유가 있어도 별도의 `EPOLLOUT` 이벤트가 발생하지 않을 수 있어, 등록과 동시에 한 번 직접 flush를 시도하여 첫 전송 지연을 없앴습니다.

### 2. DB 비동기 처리 (Head-of-Line Blocking 회피 + 재연결)
IoWorker 스레드가 로그인/회원가입 처리 중 블로킹 방식의 MySQL 쿼리를 직접 호출할 경우, 해당 스레드에 연결된 **다른 모든 소켓**의 이벤트 처리가 쿼리 응답 대기 시간만큼 지연됩니다(Head-of-Line Blocking). 이를 방지하기 위해 다음과 같은 파이프라인을 구성했습니다.

```
IoWorker 스레드: 패킷 파싱 → DBTask(람다) 생성 → DBWorkerPool 큐에 Enqueue → 즉시 다음 이벤트 처리
DB worker 스레드:   큐에서 Task Pop → 커넥션 풀에서 커넥션 대여 → 쿼리 실행 → NetServer::SendPacket으로 응답
                    (연결 유실 감지 시 재연결 후 Task 재시도)
```
- `DBConnectionPool`: `MYSQL*` 커넥션 N개를 사전에 생성해두는 풀과, RAII 가드(`ScopedConnection`)를 통한 반납 누락 방지 로직으로 구성됩니다.
- `DBWorkerPool`: `ThreadSafeQueue<DBTask>` 기반의 작업 큐와 worker 스레드 풀로 구성되며, Prepared Statement를 사용해 SQL Injection을 차단합니다.
- `NetServer::SendPacket`은 호출 스레드에 관계없이 안전하게 동작합니다. 세션의 송신 큐는 mutex로 보호되며, 대상 세션이 속한 worker에는 `eventfd`를 통해 쓰기 이벤트가 통지됩니다.
- **DB 재연결**: `MYSQL_OPT_RECONNECT`(클라이언트 라이브러리의 자동 재연결 옵션)는 최신 버전에서 deprecated되었고, 재연결 시 세션 상태가 조용히 초기화되는 위험이 있어 사용하지 않았습니다. 대신 `DBTask::execute`가 `ScopedConnection&`을 전달받아, 쿼리 실행 후 `mysql_errno()` 값이 `CR_SERVER_GONE_ERROR`/`CR_SERVER_LOST` 등 연결 유실 코드에 해당하는지(`IsConnectionLostError`) 직접 판별합니다. 연결 유실로 판단되면 `MarkBroken()`으로 표시하고, `ScopedConnection` 소멸 시점에 `DBConnectionPool::Reconnect()`가 새 커넥션을 맺어 풀에 반납합니다. `DBWorkerPool::WorkerLoop`는 이 표시를 확인하여 동일한 Task를 최대 1회 재시도합니다(문법 오류 등 재연결로 해결되지 않는 실패는 재시도하지 않고 로그로 남깁니다).

### 3. 패킷 프로토콜 및 스트림 처리
- `[4B TotalSize][2B PacketType][Body]` 구조의 길이 기반 바이너리 프로토콜을 사용합니다.
- TCP는 스트림 기반 프로토콜이므로 한 번의 `recv` 호출에 여러 패킷이 결합되어 수신되거나, 하나의 패킷이 여러 번에 걸쳐 분할 수신될 수 있습니다. 이를 처리하기 위해 `Session::RecvBuffer()`에 데이터를 누적하고, 완성된 패킷만 추출하여 디스패치하며 나머지는 다음 `recv` 호출까지 보존합니다.
- 부분 전송(`send` 호출 시 요청한 크기만큼 전송되지 않는 경우, short write)도 `EPOLLOUT` 이벤트를 기반으로 이어서 처리됩니다.

### 4. 동시성 안전 세션 관리 및 버퍼 공유
- `SessionManager`는 `std::shared_mutex`를 사용합니다. 채팅 브로드캐스트와 같이 "전체 순회 + 읽기" 연산이 빈번한 경우 `shared_lock`으로 동시 진입을 허용해 처리량을 확보하고, 세션 추가/삭제와 같이 컨테이너 구조가 변경되는 연산에만 `unique_lock`을 적용합니다.
- 브로드캐스트 시 락을 보유한 상태로 `send()`까지 수행하지 않도록, 세션 목록의 스냅샷만 획득한 뒤 즉시 락을 해제하고 순회하며 전송합니다(락 보유 시간 최소화).
- **송신 버퍼는 `shared_ptr<const vector<char>>`(`PacketBuffer`)로 공유합니다.** 브로드캐스트로 동일한 패킷을 N명에게 전송할 때 세션마다 `vector<char>`를 복사하지 않고, `NetServer::BroadcastPacket`이 버퍼를 하나만 생성하여 각 세션의 송신 큐에는 참조 카운트만 증가시켜 전달합니다. 실제 바이트 복사는 각 세션이 소켓에 `send()`를 호출할 때 커널로 넘어가는 한 번뿐이며, 이는 어떤 전송 방식을 쓰더라도 피할 수 없는 복사입니다.

### 5. 보안 및 안정성
- 비밀번호는 계정별 랜덤 salt와 SHA-256 스트레칭(10,000라운드)을 적용하여 저장하며, 평문 비교 로직은 존재하지 않습니다.
- 모든 쿼리 파라미터는 Prepared Statement로 바인딩하여 SQL Injection을 방지합니다.
- 하트비트 미수신 세션은 60초 타임아웃 기준으로 감지되어 강제 종료 통지가 전송됩니다.
- **서버 주도 강제 종료**: 통지만 전송하고 클라이언트의 자발적인 연결 해제를 기다리는 대신, `IoWorker`에 강제 종료 큐(`pendingForceCloseFds_`)를 별도로 두었습니다. `HeartbeatMonitorLoop`가 타임아웃을 감지하면 `NetServer::ForceDisconnect`를 호출하고, 이 요청은 해당 세션을 소유한 `IoWorker`의 큐에 적재된 뒤 `eventfd`로 해당 워커를 깨워 실제 `close()`까지 서버가 직접 수행합니다. 통지를 무시하는 악성/오작동 클라이언트도 좀비 세션으로 남지 않습니다.
- 비정상적인 패킷 크기 또는 과도한 recv 버퍼 누적이 감지되면 연결을 강제 종료합니다(악성/오작동 클라이언트 방어).
- `SIGPIPE` 시그널을 무시 처리하여 클라이언트의 비정상 종료가 서버 프로세스 종료로 이어지지 않도록 합니다.

## 디렉토리 구조
```
include/
  PacketDef.h          # 패킷 헤더 및 직렬화(Writer/Reader)
  ThreadSafeQueue.h     # 범용 스레드 세이프 큐 (DB 작업 큐 등에 사용)
  DBConnectionPool.h    # MySQL 커넥션 풀 (RAII 가드 포함)
  DBWorker.h             # DB 비동기 작업 큐 및 worker 스레드 풀
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
- 한 클라이언트가 전송한 채팅 메시지가 동시 접속 중인 다른 클라이언트에게 실시간으로 브로드캐스트됨을 확인했습니다(EPOLLET 및 `shared_ptr<const vector<char>>` 공유 버퍼 방식으로 전환한 이후에도 동일하게 정상 동작함을 확인했습니다).
- `chat_logs` 테이블에 한글 메시지가 UTF-8로 정확히 적재됨을 HEX 덤프로 확인했습니다.
- 하트비트 타임아웃(60초) 로직이 정상적으로 세션을 감지해 `S2C_FORCE_DISCONNECT` 통지를 전송하고, `NetServer::ForceDisconnect`를 통해 서버가 해당 세션의 소켓을 직접 종료함을 확인했습니다(클라이언트 로그에서 통지 수신 직후 연결 종료까지 확인했습니다).
- `-Wall -Wextra -pthread` 옵션 기준 경고 없이 빌드됨을 확인했습니다.

## 부하 테스트: 대용량 동시 접속 처리 검증

`tools/load_test.py`(자체 제작 비동기 부하 생성기)로 실제 서버를 빌드·기동한 뒤
100 ~ 1,000명 동시 접속 시나리오를 직접 실측했습니다. 자세한 방법론과 전체 수치는
[`report/README.md`](./GameServerSample/report/README.md)에 정리되어 있으며, 핵심 결과는 다음과 같습니다.

- **접속(epoll) 및 채팅 브로드캐스트 계층**: 1,000 동시 접속까지 성공률 100%, 지연시간
  p50 1ms 미만을 유지 — 설계 의도대로 병목이 되지 않음을 확인했습니다.
- **회원가입/로그인 처리량이 동시 접속 수와 무관하게 27.5 req/sec로 고정**되는 현상을
  발견하고, 원인이 `PasswordHash.h`의 SHA-256 스트레칭(10,000라운드, 1회 약 15.7ms
  소요)에 따른 CPU 포화임을 worker 스레드 증량 테스트와 라운드 축소 대조군 테스트로
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

## 적용된 기술
- **네트워크**: epoll 기반 비동기 논블로킹 I/O, 다중 epoll 인스턴스를 통한 worker 수평 확장, eventfd를 이용한 스레드 간 통지, 부분 송수신(short read/write) 처리, TCP_NODELAY 적용 근거에 대한 이해.
- **비동기 설계**: Head-of-Line Blocking을 회피하기 위해 블로킹 방식의 DB 호출을 별도 worker 풀로 분리한 설계 의도와 트레이드오프에 대한 설명 능력.
- **동시성 제어**: `shared_mutex`를 활용한 읽기 위주 연산 최적화, 락 보유 시간 최소화(스냅샷 후 락 해제) 패턴, RAII 기반 리소스 관리(커넥션 풀, 세션 종료).
- **데이터베이스**: Prepared Statement 적용, 커넥션 풀링, 조회 패턴을 고려한 인덱스 설계(`idx_created_at` 등).
- **보안**: salt 및 해시 스트레칭 적용, SQL Injection 방지, 악성 패킷에 대한 방어(크기 검증, 버퍼 오버플로우 방지).
- **성능 분석 및 검증**: 자체 제작 비동기 부하 생성기로 실제 서버를 기동해 대용량 동시 접속을 실측, 병목 지점(CPU 바운드 vs I/O 바운드)을 worker 스레드 증량 테스트와 대조군 비교로 교차 검증하는 성능 프로파일링 방법론.
