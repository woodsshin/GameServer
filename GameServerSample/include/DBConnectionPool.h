#pragma once
#include <mysql/mysql.h>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <stdexcept>

// ============================================================
// MySQL 커넥션 풀
// - N개의 MYSQL* 연결을 미리 생성해두고 재사용 (매 쿼리마다 connect/disconnect 비용 제거)
// - RAII 가드(ScopedConnection)로 반납 누락 방지
// - DB 워커 스레드들이 이 풀에서 커넥션을 빌려서 사용
// ============================================================

struct DBConfig {
    std::string host = "127.0.0.1";
    std::string user = "root";
    std::string password;
    std::string dbName;
    unsigned int port = 3306;
    int poolSize = 8;
};

class DBConnectionPool {
public:
    explicit DBConnectionPool(const DBConfig& config) : config_(config) {
        for (int i = 0; i < config_.poolSize; ++i) {
            MYSQL* conn = CreateConnection();
            pool_.push_back(conn);
        }
    }

    ~DBConnectionPool() {
        for (auto* conn : pool_) {
            mysql_close(conn);
        }
    }

    DBConnectionPool(const DBConnectionPool&) = delete;
    DBConnectionPool& operator=(const DBConnectionPool&) = delete;

    // 커넥션 획득 (풀이 비어있으면 대기)
    MYSQL* Acquire() {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !pool_.empty(); });
        MYSQL* conn = pool_.back();
        pool_.pop_back();
        return conn;
    }

    // 커넥션 반납
    void Release(MYSQL* conn) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_.push_back(conn);
        }
        cv_.notify_one();
    }

    // RAII 래퍼: 스코프를 벗어나면 자동으로 Release
    class ScopedConnection {
    public:
        explicit ScopedConnection(DBConnectionPool& pool)
            : pool_(pool), conn_(pool.Acquire()) {}

        ~ScopedConnection() { pool_.Release(conn_); }

        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;

        MYSQL* Get() const { return conn_; }
        MYSQL* operator->() const { return conn_; }

    private:
        DBConnectionPool& pool_;
        MYSQL* conn_;
    };

private:
    MYSQL* CreateConnection() {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) {
            throw std::runtime_error("mysql_init 실패");
        }

        // 참고: MYSQL_OPT_RECONNECT는 최신 클라이언트 라이브러리에서 deprecated 되었고
        // 자동 재연결 시 세션 상태(임시 테이블, 트랜잭션 등)가 조용히 초기화되는 위험이 있어
        // 의도적으로 사용하지 않는다. 연결 끊김은 DBWorkerPool 쪽에서 쿼리 실패를 감지해
        // 명시적으로 재연결하는 방식이 더 안전하다 (여기서는 포트폴리오 범위상 생략).

        if (!mysql_real_connect(conn, config_.host.c_str(), config_.user.c_str(),
                                 config_.password.c_str(), config_.dbName.c_str(),
                                 config_.port, nullptr, 0)) {
            std::string err = mysql_error(conn);
            mysql_close(conn);
            throw std::runtime_error("MySQL 연결 실패: " + err);
        }

        // utf8mb4로 통일 (한글/이모지 등 대비)
        mysql_set_character_set(conn, "utf8mb4");
        return conn;
    }

    DBConfig config_;
    std::vector<MYSQL*> pool_;
    std::mutex mutex_;
    std::condition_variable cv_;
};
