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
// ============================================================

struct DBTask {
    // conn: worker가 풀에서 빌려온 커넥션. Task 내부에서 쿼리 실행.
    std::function<void(MYSQL* conn)> execute;
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
            DBConnectionPool::ScopedConnection conn(dbPool_);
            try {
                taskOpt->execute(conn.Get());
            } catch (const std::exception& e) {
                fprintf(stderr, "[DBWorker] 작업 처리 중 예외: %s\n", e.what());
            }
        }
    }

    DBConnectionPool& dbPool_;
    ThreadSafeQueue<DBTask> taskQueue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> stopped_{false};
};
