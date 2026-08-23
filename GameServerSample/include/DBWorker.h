#pragma once
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include "DBConnectionPool.h"
#include "ThreadSafeQueue.h"

// ============================================================
// 비동기 DB 처리 계층
//
// 왜 필요한가:
//   네트워크 I/O 스레드(epoll worker)가 로그인/회원가입 처리 중에
//   블로킹 MySQL 쿼리를 직접 호출하면, 해당 스레드가 쿼리 응답을
//   기다리는 동안 그 스레드에 물린 다른 모든 소켓의 이벤트 처리가
//   지연된다 (Head-of-line blocking).
//
//   그래서 "DB 요청"을 작업(Task) 객체로 캡슐화해 큐에 넣고,
//   별도의 DB worker 스레드 풀이 이를 꺼내 처리한 뒤, 결과를
//   콜백(std::function)으로 다시 네트워크 스레드/메인 로직에 전달한다.
//   -> 네트워크 스레드는 절대 블로킹 쿼리를 직접 실행하지 않는다.
//
// 재연결 정책:
//   Task 콜백은 MYSQL*가 아니라 ScopedConnection&을 받는다. 쿼리 실행 후
//   IsConnectionLostError(conn)로 "연결이 끊어져서" 실패했는지 판별할 수
//   있고, 그렇다면 conn.MarkBroken()으로 표시한다. WorkerLoop은 이 표시를
//   보고 커넥션을 새로 맺은 뒤 같은 Task를 한 번 더 실행한다(재시도는
//   최대 1회로 제한해 DB 자체가 죽어있는 경우 무한 루프에 빠지지 않게 한다).
// ============================================================

struct DBTask {
    // conn: worker가 풀에서 빌려온 커넥션 가드. Task 내부에서 conn.Get()으로
    // MYSQL*를 얻어 쿼리를 실행하고, 연결 유실을 감지하면 conn.MarkBroken()을 호출한다.
    std::function<void(DBConnectionPool::ScopedConnection& conn)> execute;
};

class DBWorkerPool {
public:
    DBWorkerPool(DBConnectionPool& pool, int workerCount)
        : dbPool_(pool) {
        for (int i = 0; i < workerCount; ++i) {
            workers_.emplace_back([this] { WorkerLoop(); });
        }
    }

    ~DBWorkerPool() {
        Stop();
    }

    void Enqueue(DBTask task) {
        taskQueue_.Push(std::move(task));
    }

    void Stop() {
        if (stopped_.exchange(true)) return;
        taskQueue_.Shutdown();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
    }

    size_t PendingCount() const { return taskQueue_.Size(); }

private:
    void WorkerLoop() {
        while (true) {
            auto taskOpt = taskQueue_.WaitPop();
            if (!taskOpt.has_value()) {
                break; // shutdown
            }

            // 연결 유실로 인한 실패는 최대 1회 재연결 후 재시도한다.
            // (일반 쿼리 오류 - 문법 오류, 제약 위반 등 - 는 재연결해도 결과가
            // 같으므로 재시도하지 않고 그대로 예외/로그로 흘려보낸다)
            for (int attempt = 0; attempt < 2; ++attempt) {
                DBConnectionPool::ScopedConnection conn(dbPool_);
                if (!conn.Get()) {
                    // 직전 시도에서 재연결까지 실패한 경우. 다음 루프 반복(다음 Task)에서
                    // 다시 Acquire하면 또 nullptr을 받을 수 있으나, Reconnect는 매번
                    // 재시도되므로 DB가 복구되면 이후 Task부터는 정상 커넥션을 받는다.
                    fprintf(stderr, "[DBWorker] 유효한 DB 커넥션을 얻지 못해 작업을 건너뜁니다\n");
                    break;
                }

                bool connectionLost = false;
                try {
                    taskOpt->execute(conn);
                    connectionLost = IsConnectionLostError(conn.Get());
                } catch (const std::exception& e) {
                    fprintf(stderr, "[DBWorker] 작업 처리 중 예외: %s\n", e.what());
                }

                if (connectionLost) {
                    conn.MarkBroken(); // 소멸 시 재연결 후 반납됨
                    fprintf(stderr, "[DBWorker] DB 연결 유실 감지, 재연결 후 재시도합니다 (attempt=%d)\n", attempt + 1);
                    continue; // 재시도
                }
                break; // 성공했거나, 재시도로 해결되지 않는 오류이므로 종료
            }
        }
    }

    DBConnectionPool& dbPool_;
    ThreadSafeQueue<DBTask> taskQueue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopped_{false};
};
