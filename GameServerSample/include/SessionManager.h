#pragma once
#include <unordered_map>
#include <shared_mutex>
#include <memory>
#include <vector>
#include "Session.h"

// ============================================================
// 대용량 동시접속 유저를 다루기 위한 세션 컨테이너
// - 여러 워커 스레드(각각 다른 소켓 fd 집합을 epoll로 처리)가
//   동시에 세션을 조회/추가/삭제하므로 shared_mutex로 보호
// - 브로드캐스트(채팅)처럼 "전체 순회 + 읽기"가 잦은 연산은
//   shared_lock(읽기 락)으로 동시 진입 허용 -> 처리량 확보
// - 추가/삭제처럼 컨테이너 구조가 바뀌는 연산만 unique_lock
// ============================================================
class SessionManager {
public:
    void Add(const std::shared_ptr<Session>& session) {
        std::unique_lock lock(mutex_);
        sessions_[session->GetSessionId()] = session;
    }

    void Remove(uint64_t sessionId) {
        std::unique_lock lock(mutex_);
        sessions_.erase(sessionId);
    }

    std::shared_ptr<Session> Get(uint64_t sessionId) {
        std::shared_lock lock(mutex_);
        auto it = sessions_.find(sessionId);
        if (it == sessions_.end()) return nullptr;
        return it->second;
    }

    size_t Count() {
        std::shared_lock lock(mutex_);
        return sessions_.size();
    }

    // 채팅 브로드캐스트 등에 사용: 현재 인증된 모든 세션의 스냅샷을 복사해서 반환.
    // 락을 잡은 채로 send()까지 수행하면 락 보유 시간이 길어지므로,
    // 스냅샷만 뜨고 락을 즉시 해제한 뒤 호출 측에서 순회하며 전송한다.
    std::vector<std::shared_ptr<Session>> SnapshotAuthenticated() {
        std::shared_lock lock(mutex_);
        std::vector<std::shared_ptr<Session>> result;
        result.reserve(sessions_.size());
        for (auto& [id, session] : sessions_) {
            if (session->GetState() == SessionState::AUTHENTICATED) {
                result.push_back(session);
            }
        }
        return result;
    }

    // 하트비트 타임아웃 검사용 스냅샷
    std::vector<std::shared_ptr<Session>> SnapshotAll() {
        std::shared_lock lock(mutex_);
        std::vector<std::shared_ptr<Session>> result;
        result.reserve(sessions_.size());
        for (auto& [id, session] : sessions_) {
            result.push_back(session);
        }
        return result;
    }

private:
    std::shared_mutex mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<Session>> sessions_;
};
