#pragma once
#include <mysql/mysql.h>
#include <string>
#include <vector>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <stdexcept>
#include <cstdio>

// ============================================================
// MySQL 커넥션 풀
// - N개의 MYSQL* 연결을 미리 생성해두고 재사용 (매 쿼리마다 connect/disconnect 비용 제거)
// - RAII 가드(ScopedConnection)로 반납 누락 방지
// - DB worker 스레드들이 이 풀에서 커넥션을 빌려서 사용
// - 재연결 정책: MYSQL_OPT_RECONNECT(클라이언트 라이브러리 자동 재연결)는 최신
//   버전에서 deprecated되었고, 재연결 시 세션 상태(임시 테이블/트랜잭션 등)가
//   조용히 초기화되는 위험이 있어 사용하지 않는다. 대신 쿼리를 실행한 쪽(DBWorker)이
//   실패를 감지하면 Reconnect()를 명시적으로 호출하는 방식을 쓴다 — "언제 재연결할지"를
//   커넥션 풀이 추측하지 않고, 실패를 실제로 관찰한 호출자가 결정하도록 하기 위함이다.
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

    // 쿼리 실패 후 호출자가 명시적으로 재연결을 요청할 때 사용.
    // 기존 핸들을 닫고 새로 연결한 뒤 같은 포인터 값을 재사용할 수 없으므로
    // (mysql_close가 내부 버퍼를 해제) 새 MYSQL* 를 반환한다.
    // 실패 시 nullptr을 반환하며, 이 경우 원래 커넥션은 이미 close된 상태이므로
    // 호출자는 반드시 반환된 새 포인터(혹은 nullptr)로 conn 변수를 교체해야 한다.
    MYSQL* Reconnect(MYSQL* oldConn) {
        if (oldConn) {
            mysql_close(oldConn);
        }
        try {
            return CreateConnection();
        } catch (const std::exception& e) {
            fprintf(stderr, "[DBConnectionPool] 재연결 실패: %s\n", e.what());
            return nullptr;
        }
    }

    // RAII 래퍼: 스코프를 벗어나면 자동으로 Release.
    // MarkBroken()으로 표시된 커넥션은 반납 시 즉시 재연결을 시도해, 풀에는
    // 항상 살아있는(것으로 확인된) 커넥션만 돌아가도록 한다.
    class ScopedConnection {
    public:
        explicit ScopedConnection(DBConnectionPool& pool)
            : pool_(pool), conn_(pool.Acquire()) {}

        ~ScopedConnection() {
            if (broken_) {
                MYSQL* fresh = pool_.Reconnect(conn_);
                conn_ = fresh; // 실패하면 nullptr; Acquire()가 nullptr을 걸러내지 않으므로
                               // 다음 Acquire 호출자가 쿼리 실행 시 즉시 실패를 감지하고
                               // 다시 MarkBroken()을 호출하는 흐름으로 자연스럽게 재시도된다.
            }
            if (conn_) {
                pool_.Release(conn_);
            }
        }

        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;

        MYSQL* Get() const { return conn_; }
        MYSQL* operator->() const { return conn_; }

        // 쿼리 실행 중 연결 유실(CR_SERVER_GONE_ERROR, CR_SERVER_LOST 등)을
        // 감지했을 때 호출자가 표시. 이 커넥션은 풀에 그대로 돌아가지 않고
        // 소멸 시점에 재연결을 거친 뒤 반납된다.
        void MarkBroken() { broken_ = true; }

    private:
        DBConnectionPool& pool_;
        MYSQL* conn_;
        bool broken_ = false;
    };

private:
    MYSQL* CreateConnection() {
        MYSQL* conn = mysql_init(nullptr);
        if (!conn) {
            throw std::runtime_error("mysql_init 실패");
        }

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

// ============================================================
// 쿼리 실행 후 연결 유실 여부를 판별하는 헬퍼.
// mysql_errno()가 아래 세 코드 중 하나면 "쿼리 자체의 문제"가 아니라
// "연결이 끊어져서" 실패한 것이므로 재연결 대상으로 분류한다.
//   CR_SERVER_GONE_ERROR (2006): 서버가 연결을 닫음 (idle timeout 등)
//   CR_SERVER_LOST       (2013): 쿼리 도중 연결 유실 (네트워크 순단 등)
//   CR_SERVER_LOST_EXTENDED (2055): 위와 동일하되 상세 사유 포함 (최신 클라이언트)
// ============================================================
inline bool IsConnectionLostError(MYSQL* conn) {
    unsigned int err = mysql_errno(conn);
    return err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST
#ifdef CR_SERVER_LOST_EXTENDED
        || err == CR_SERVER_LOST_EXTENDED
#endif
        ;
}
