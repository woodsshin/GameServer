#pragma once
#include <condition_variable>
#include <mutex>
#include <queue>
#include <optional>
#include <atomic>

// ============================================================
// 범용 스레드 세이프 큐
// - DB 비동기 작업 큐, 브로드캐스트 큐 등에서 공용으로 사용
// - Shutdown() 호출 시 대기 중인 모든 컨슈머 스레드를 깨워서 종료 유도
// ============================================================
template <typename T>
class ThreadSafeQueue {
public:
    void Push(T item) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push(std::move(item));
        }
        cv_.notify_one();
    }

    // 큐가 비어있으면 블록. Shutdown 되면 nullopt 반환
    std::optional<T> WaitPop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_; });

        if (queue_.empty()) {
            return std::nullopt; // shutdown 케이스
        }

        T item = std::move(queue_.front());
        queue_.pop();
        return item;
    }

    void Shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

    size_t Size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> queue_;
    std::atomic<bool> shutdown_{false};
};
