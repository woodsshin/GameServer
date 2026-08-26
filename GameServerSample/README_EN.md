# C++ Asynchronous Game Server (Network / Async / MySQL Portfolio)

A small-scale game server implemented with epoll-based asynchronous I/O, a DB connection pool, and a thread-separated architecture.
The goal is to design and implement a server architecture that supports account creation, login, chat broadcasting, and handling a large number of concurrent connections.

## Core Design Points

### 1. Separating the Accept Thread from IO Worker Threads
```
[Accept Thread]  --accept()--> round-robin distribution --> [IoWorker 0] [IoWorker 1] ... [IoWorker N]
                                                       (epoll)      (epoll)         (epoll)
```
- A dedicated Accept thread watches only the `listen` socket. This means accepting new connections is never delayed even when worker threads are under heavy load from traffic processing.
- Each IoWorker owns its **own independent epoll instance**. Compared to a structure where all file descriptors are handled through a single shared epoll instance, this avoids lock contention and allows horizontal scaling proportional to the number of CPU cores (the worker count is determined based on `std::thread::hardware_concurrency()`).
- Notifications that cross thread boundaries — new socket registration, write requests, forced-close requests — are handled through a combination of `eventfd` and `epoll_wait`, so worker threads only wake up on actual events rather than polling unnecessarily.
- Sockets are registered in **edge-triggered (EPOLLET)** mode, and `HandleReadable`/`HandleWritable` process them with a non-blocking loop that repeats read/write until `EAGAIN` is returned. With level-triggered mode, `epoll_wait` keeps returning the same fd as long as unprocessed data remains, whereas edge-triggered mode only notifies once when the state changes, reducing the number of `epoll_wait` returns and related syscalls for the same fd. Right after registering a new write request as edge-triggered, a separate `EPOLLOUT` event may not fire even if there's room in the socket buffer, so a direct flush is attempted immediately upon registration to eliminate the initial send delay.

```cpp
// NetServer.cpp - Registering a new socket as EPOLLET (edge-triggered)
for (int newFd : newFds) {
    uint64_t sessionId = nextSessionId_.fetch_add(1);
    auto session = std::make_shared<Session>(newFd, sessionId);
    sessionManager_.Add(session);
    localSessions[newFd] = session;

    epoll_event sev{};
    sev.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    sev.data.fd = newFd;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, newFd, &sev);
}

// Additionally register EPOLLOUT for sockets waiting to send (keeping ET mode)
for (int wfd : writeFds) {
    auto it = localSessions.find(wfd);
    if (it == localSessions.end()) continue;
    epoll_event sev{};
    sev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLET;
    sev.data.fd = wfd;
    epoll_ctl(epollFd_, EPOLL_CTL_MOD, wfd, &sev);
    // In ET mode, a separate EPOLLOUT event may not fire even if the
    // socket buffer already had room at registration time, so a direct
    // flush is attempted right after registration to eliminate the
    // initial send delay.
    HandleWritable(it->second);
}
```

```cpp
// NetServer.cpp - HandleReadable: a non-blocking loop that repeats until EAGAIN
void IoWorker::HandleReadable(std::shared_ptr<Session>& session) {
    char tmp[4096];
    int fd = session->GetFd();

    while (true) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            auto& buf = session->RecvBuffer();
            buf.insert(buf.end(), tmp, tmp + n);
            session->TouchRecvTime();

            if (buf.size() > proto::MAX_PACKET_SIZE * 4) {
                CloseSession(session); // guard against a misbehaving client
                return;
            }
        } else if (n == 0) {
            CloseSession(session); // normal close (FIN)
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // read everything available for this event
            }
            if (errno == EINTR) continue;
            CloseSession(session);
            return;
        }
    }

    TryDispatchPackets(session);
}
```

### 2. Asynchronous DB Processing (Avoiding Head-of-Line Blocking + Reconnection)
If an IoWorker thread directly calls a blocking MySQL query while handling login/signup, event processing for **every other socket** on that thread gets delayed for as long as the query takes to respond (head-of-line blocking). To prevent this, the following pipeline was built.

```
IoWorker thread: parse packet → create DBTask (lambda) → enqueue into DBWorkerPool queue → immediately process the next event
DB worker thread:   pop task from queue → borrow a connection from the connection pool → run the query → respond via NetServer::SendPacket
                    (on detecting a lost connection, reconnect and retry the task)
```
- `DBConnectionPool`: consists of a pool that pre-creates N `MYSQL*` connections, plus RAII guard logic (`ScopedConnection`) that prevents a connection from being forgotten and never returned.
- `DBWorkerPool`: consists of a task queue based on `ThreadSafeQueue<DBTask>` and a pool of worker threads, and blocks SQL injection by using prepared statements.
- `NetServer::SendPacket` is safe to call from any thread. A session's send queue is protected by a mutex, and the worker owning the target session is notified of the write event via `eventfd`.
- **DB reconnection**: `MYSQL_OPT_RECONNECT` (the client library's automatic reconnect option) is deprecated in recent versions and carries the risk of silently resetting session state on reconnect, so it isn't used. Instead, `DBTask::execute` receives a `ScopedConnection&`, and after running the query it directly checks whether the `mysql_errno()` value corresponds to a connection-lost code such as `CR_SERVER_GONE_ERROR`/`CR_SERVER_LOST` (`IsConnectionLostError`). If the connection is judged lost, it's marked via `MarkBroken()`, and when the `ScopedConnection` is destroyed, `DBConnectionPool::Reconnect()` establishes a new connection and returns it to the pool. `DBWorkerPool::WorkerLoop` checks this flag and retries the same task at most once (failures that reconnection can't fix, such as syntax errors, are not retried and are simply logged).

```cpp
// DBConnectionPool.h - ScopedConnection: reconnects on destruction if MarkBroken was called
class ScopedConnection {
public:
    explicit ScopedConnection(DBConnectionPool& pool)
        : pool_(pool), conn_(pool.Acquire()) {}

    ~ScopedConnection() {
        if (broken_) {
            MYSQL* fresh = pool_.Reconnect(conn_);
            conn_ = fresh; // nullptr on failure; the next caller to Acquire will
                           // immediately detect the failure on query execution,
                           // call MarkBroken() again, and trigger another retry.
        }
        if (conn_) {
            pool_.Release(conn_);
        }
    }

    MYSQL* Get() const { return conn_; }

    // Called by the caller when a connection loss (CR_SERVER_GONE_ERROR,
    // CR_SERVER_LOST, etc.) is detected during query execution. The
    // connection is reconnected before being returned, at destruction time.
    void MarkBroken() { broken_ = true; }

private:
    DBConnectionPool& pool_;
    MYSQL* conn_;
    bool broken_ = false;
};
```

```cpp
// DBWorker.h - WorkerLoop: reconnects and retries at most once when a lost connection is detected
void WorkerLoop() {
    while (true) {
        auto taskOpt = taskQueue_.WaitPop();
        if (!taskOpt.has_value()) break; // shutdown

        for (int attempt = 0; attempt < 2; ++attempt) {
            DBConnectionPool::ScopedConnection conn(dbPool_);
            if (!conn.Get()) break; // even reconnection failed

            bool connectionLost = false;
            try {
                taskOpt->execute(conn);
                connectionLost = IsConnectionLostError(conn.Get());
            } catch (const std::exception& e) {
                fprintf(stderr, "[DBWorker] Exception while processing task: %s\n", e.what());
            }

            if (connectionLost) {
                conn.MarkBroken(); // reconnected and returned to the pool on destruction
                continue; // retry
            }
            break; // succeeded, or the error can't be fixed by retrying, so stop
        }
    }
}
```

### 3. Packet Protocol and Stream Handling
- Uses a length-prefixed binary protocol with the structure `[4B TotalSize][2B PacketType][Body]`.
- Since TCP is a stream-based protocol, a single `recv` call may return multiple packets combined together, or a single packet may arrive split across multiple calls. To handle this, data is accumulated in `Session::RecvBuffer()`, only complete packets are extracted and dispatched, and the remainder is preserved until the next `recv` call.
- Partial sends (a "short write," where a `send` call doesn't transmit the full requested size) are also handled by resuming based on the `EPOLLOUT` event.

```cpp
// NetServer.cpp - TryDispatchPackets: cuts out and dispatches only complete packets, by length
void IoWorker::TryDispatchPackets(std::shared_ptr<Session>& session) {
    auto& buf = session->RecvBuffer();

    size_t consumed = 0;
    while (buf.size() - consumed >= proto::HEADER_SIZE) {
        const char* base = buf.data() + consumed;
        auto* header = reinterpret_cast<const proto::PacketHeader*>(base);

        if (header->totalSize < proto::HEADER_SIZE || header->totalSize > proto::MAX_PACKET_SIZE) {
            CloseSession(session); // guard against an abnormal packet size
            return;
        }

        if (buf.size() - consumed < header->totalSize) {
            break; // the body hasn't fully arrived yet -> wait for the next recv
        }

        auto type = static_cast<proto::PacketType>(header->type);
        const char* body = base + proto::HEADER_SIZE;
        size_t bodySize = header->totalSize - proto::HEADER_SIZE;

        packetHandler_(session, type, body, bodySize);
        consumed += header->totalSize;
    }

    if (consumed > 0) {
        buf.erase(buf.begin(), buf.begin() + consumed); // preserve any remaining partial packet in the buffer
    }
}
```

### 4. Concurrency-Safe Session Management and Buffer Sharing
- `SessionManager` uses `std::shared_mutex`. For operations like chat broadcasting, where "iterate over everything + read" happens frequently, a `shared_lock` allows concurrent entry to maximize throughput, while a `unique_lock` is only applied to operations that modify the container's structure, like adding/removing a session.
- To avoid holding a lock all the way through `send()` during a broadcast, only a snapshot of the session list is taken, the lock is released immediately, and the send happens afterward while iterating (minimizing lock hold time).
- **The send buffer is shared as `shared_ptr<const vector<char>>` (`PacketBuffer`).** When broadcasting the same packet to N recipients, instead of copying a `vector<char>` for every session, `NetServer::BroadcastPacket` creates the buffer only once and delivers it to each session's send queue with just a reference-count increment. The actual byte copy happens only once, when each session calls `send()` on its socket and the data is handed to the kernel — a copy that's unavoidable no matter which transmission method is used.

```cpp
// Session.h - The send queue holds shared_ptr<const vector<char>> (PacketBuffer)
using PacketBuffer = std::shared_ptr<const std::vector<char>>;

bool EnqueueSend(PacketBuffer data) {
    std::lock_guard<std::mutex> lock(sendMutex_);
    bool wasEmpty = sendQueue_.empty();
    sendQueue_.push_back(std::move(data)); // only the shared_ptr is enqueued, no vector copy
    return wasEmpty;
}
```

```cpp
// NetServer.cpp - BroadcastPacket: N recipients share a single buffer via reference-count increments only
void NetServer::BroadcastPacket(SessionManager& sessionManager, PacketBuffer packetData, uint64_t excludeSessionId) {
    auto sessions = sessionManager.SnapshotAuthenticated();
    for (auto& s : sessions) {
        if (s->GetSessionId() == excludeSessionId) continue;
        SendPacket(s, packetData); // only a shared_ptr copy (refcount increment) happens; the buffer itself is shared
    }
}
```

### 5. Security and Stability
- Passwords are stored with a per-account random salt and SHA-256 stretching (10,000 rounds); no plaintext comparison logic exists.
- All query parameters are bound via prepared statements to prevent SQL injection.
- Sessions that miss heartbeats are detected on a 60-second timeout basis, at which point a forced-disconnect notification is sent.
- **Server-driven forced disconnection**: instead of just sending a notification and waiting for the client to voluntarily close the connection, a separate forced-close queue (`pendingForceCloseFds_`) is kept on each `IoWorker`. When `HeartbeatMonitorLoop` detects a timeout, it calls `NetServer::ForceDisconnect`; this request is placed on the queue of the `IoWorker` that owns the session, that worker is woken via `eventfd`, and the server itself performs the actual `close()`. Even a malicious or malfunctioning client that ignores the notification won't linger as a zombie session.
- The connection is forcibly closed if an abnormal packet size or excessive accumulation in the recv buffer is detected (defense against malicious/malfunctioning clients).
- The `SIGPIPE` signal is ignored so that an abnormal client disconnect doesn't bring down the server process.

```cpp
// NetServer.cpp - ForceDisconnect: places a close request on the queue of the IoWorker that owns the session
void NetServer::ForceDisconnect(std::shared_ptr<Session> session) {
    if (!session) return;
    session->MarkForForceClose(); // mark first so any further send requests are silently ignored

    int fd = session->GetFd();
    int workerIdx = -1;
    {
        std::lock_guard<std::mutex> lock(fdWorkerMapMutex_);
        auto it = fdToWorkerIndex_.find(fd);
        if (it != fdToWorkerIndex_.end()) workerIdx = it->second;
    }
    if (workerIdx >= 0) {
        // The actual close() is performed by the IoWorker thread that owns this fd, via the forced-close queue.
        workers_[workerIdx]->RequestForceClose(fd);
    }
}
```

```cpp
// NetServer.cpp - IoWorker: processes the forced-close queue after waking on eventfd
for (int cfd : forceCloseFds) {
    auto it = localSessions.find(cfd);
    if (it == localSessions.end()) continue;
    CloseSession(it->second); // close() is only ever performed inside the thread that owns this fd
    localSessions.erase(it);
}
```

## Directory Structure
```
include/
  PacketDef.h          # Packet header and serialization (Writer/Reader)
  ThreadSafeQueue.h     # General-purpose thread-safe queue (used for the DB task queue, etc.)
  DBConnectionPool.h    # MySQL connection pool (with RAII guard)
  DBWorker.h             # Asynchronous DB task queue and worker thread pool
  Session.h              # Client session (recv buffer, send queue)
  SessionManager.h       # Container for all sessions (shared_mutex)
  NetServer.h            # epoll-based network core (Accept/IoWorker)
  PasswordHash.h         # SHA-256 + salt hashing
  GameServer.h            # Business logic layer (signup/login/chat)
src/
  NetServer.cpp
  GameServer.cpp
  main.cpp
  TestClient.cpp          # Test client for verifying behavior
sql/
  schema.sql               # users, chat_logs tables
tools/
  load_test.py              # Custom async load-test client (simulates hundreds to thousands of concurrent connections)
  run_full_load_test.sh     # Runner that handles starting MySQL + running the server + staged load tests all at once
report/
  load_test.py           # Load test script
CMakeLists.txt
```

## Build Instructions

### Installing Dependencies (Ubuntu/Debian)
```bash
sudo apt-get install -y build-essential cmake libmysqlclient-dev libssl-dev
```

### Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```
Once the build finishes, two executables are produced: `game_server` (the server itself) and `test_client` (a client for verification).

### Preparing the DB
```bash
mysql -u root < sql/schema.sql
mysql -u root -e "CREATE USER 'gameapp'@'127.0.0.1' IDENTIFIED BY 'yourpassword'; \
                   GRANT ALL PRIVILEGES ON game_server.* TO 'gameapp'@'127.0.0.1'; FLUSH PRIVILEGES;"
```

### Running
```bash
DB_HOST=127.0.0.1 DB_USER=gameapp DB_PASSWORD=yourpassword DB_NAME=game_server \
  ./build/game_server 9000
```
If the environment variables aren't set, it attempts to connect using the defaults (the `root` account, an empty password, and the `game_server` database).

### Verifying Behavior
```bash
./build/test_client 127.0.0.1 9000 <username>
```
Connection, signup, login, and sending a chat message are all performed automatically, and the server's responses are printed to the console.
Running this in multiple terminals with different usernames at the same time lets you verify the broadcast behavior.

## Verification Results (Local Integration Testing)
- Confirmed that signup, login, and chat broadcasting work correctly with 3 clients connected simultaneously.
- Confirmed that a chat message sent by one client is broadcast in real time to other concurrently connected clients (this was confirmed to still work correctly after switching to EPOLLET and the `shared_ptr<const vector<char>>` shared-buffer approach).
- Confirmed via hex dump that Korean-language messages are stored correctly as UTF-8 in the `chat_logs` table.
- Confirmed that the heartbeat timeout (60 seconds) logic correctly detects a session, sends an `S2C_FORCE_DISCONNECT` notification, and that the server itself closes that session's socket via `NetServer::ForceDisconnect` (verified in the client log, from receiving the notification through to the connection closing).
- Confirmed the project builds with no warnings under `-Wall -Wextra -pthread`.

## Load Testing: Verifying Large-Scale Concurrent Connection Handling

Using `report/load_test.py` (a custom-built asynchronous load generator), the actual server was built and started, and
scenarios with 100 to 1,000 concurrent connections were measured directly. The detailed methodology and full figures
are documented in [`report/README_EN.md`](./report/README_EN.md); the key results are as follows.

- **Connection (epoll) and chat broadcast layer**: maintained a 100% success rate and a p50 latency under 1ms
  up to 1,000 concurrent connections — confirming it does not become a bottleneck, as intended by the design.
- Discovered that **signup/login throughput stays fixed at 27.5 req/sec regardless of the number of concurrent
  connections**, and cross-verified — through a worker-thread-count scaling test and a control test with a reduced
  round count — that the cause was CPU saturation from the SHA-256 stretching in `PasswordHash.h`
  (10,000 rounds, taking roughly 15.7ms per call).
- In a control test with the stretching rounds lowered to 1,000, the signup success rate at 1,000 concurrent
  connections improved from 92.3% → 100%, and p50 latency improved from 7.65s → 0.54s (roughly a 14x improvement),
  proving both the bottleneck's location and the effectiveness of the fix with measured figures.

### How to Reproduce
```bash
# Run only the load test, assuming the server is already up
python3 tools/load_test.py --host 127.0.0.1 --port 9000 \
    --clients 1000 --ramp-seconds 5 --chat-per-client 3 --out result.json

# Check that MySQL is up -> run the server -> staged load at 100/300/600/1000 clients -> save results, all in one go
LOAD_TEST_STAGES="100 300 600 1000" \
  ./tools/run_full_load_test.sh 9000 127.0.0.1 gameapp yourpassword game_server
```

## Skills Demonstrated
- **Networking**: epoll-based asynchronous non-blocking I/O, horizontal worker scaling via multiple epoll instances, cross-thread notification using eventfd, handling partial sends/receives (short read/write), understanding of the rationale for applying TCP_NODELAY.
- **Asynchronous design**: ability to explain the design intent and trade-offs of separating blocking DB calls into a dedicated worker pool to avoid head-of-line blocking.
- **Concurrency control**: optimizing read-heavy operations with `shared_mutex`, the pattern of minimizing lock hold time (release the lock after taking a snapshot), RAII-based resource management (connection pool, session termination).
- **Database**: applying prepared statements, connection pooling, index design that accounts for query patterns (e.g., `idx_created_at`).
- **Security**: applying salting and hash stretching, preventing SQL injection, defending against malicious packets (size validation, buffer overflow prevention).
- **Performance analysis and verification**: a performance profiling methodology that starts an actual server with a custom-built asynchronous load generator to measure large-scale concurrent connections directly, and cross-verifies the bottleneck (CPU-bound vs. I/O-bound) through worker-thread-count scaling tests and control-group comparisons.
