#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <deque>

// ============================================================
// 클라이언트 1명당 1개 생성되는 세션 객체
// - non-blocking 소켓의 recv 버퍼 누적 / 부분 패킷 조립 담당
// - send는 큐잉 후 EPOLLOUT 이벤트에서 flush (writev 스타일)
// - sendMutex_로 여러 스레드(브로드캐스트 스레드 vs 자기 자신 처리 스레드)가
//   동시에 같은 세션에 쓰기 시도해도 안전하게 함
// ============================================================

enum class SessionState {
    CONNECTED,      // TCP 연결됨, 아직 인증 전
    AUTHENTICATED,  // 로그인 완료
    CLOSING,        // 종료 처리 중
};

class Session {
public:
    explicit Session(int fd, uint64_t sessionId)
        : fd_(fd), sessionId_(sessionId), state_(SessionState::CONNECTED) {
        lastRecvTime_ = std::chrono::steady_clock::now();
    }

    int GetFd() const { return fd_; }
    uint64_t GetSessionId() const { return sessionId_; }

    SessionState GetState() const { return state_.load(); }
    void SetState(SessionState s) { state_.store(s); }

    // ---- 유저 정보 (로그인 이후 채움) ----
    void SetUserInfo(int64_t userId, const std::string& nickname) {
        std::lock_guard<std::mutex> lock(infoMutex_);
        userId_ = userId;
        nickname_ = nickname;
    }
    int64_t GetUserId() const {
        std::lock_guard<std::mutex> lock(infoMutex_);
        return userId_;
    }
    std::string GetNickname() const {
        std::lock_guard<std::mutex> lock(infoMutex_);
        return nickname_;
    }

    // ---- 수신 버퍼 (recv thread에서만 접근하므로 락 불필요) ----
    std::vector<char>& RecvBuffer() { return recvBuffer_; }

    // ---- 송신 큐 (다중 스레드에서 접근 가능하므로 mutex 보호) ----
    // 반환값: 이 호출로 인해 "빈 큐 -> 비어있지 않은 큐"가 되었는지 여부
    // (epoll에 EPOLLOUT 등록이 필요한 시점 판단용)
    bool EnqueueSend(std::vector<char> data) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        bool wasEmpty = sendQueue_.empty();
        sendQueue_.push_back(std::move(data));
        return wasEmpty;
    }

    // 현재 flush 대상 프레임 하나를 꺼냄 (부분 전송 지원 위해 offset 함께 관리)
    bool TryPeekSendFront(std::vector<char>** outData, size_t** outOffset) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (sendQueue_.empty()) return false;
        *outData = &sendQueue_.front();
        *outOffset = &sendOffset_;
        return true;
    }

    void PopSendFront() {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (!sendQueue_.empty()) {
            sendQueue_.pop_front();
            sendOffset_ = 0;
        }
    }

    bool HasPendingSend() {
        std::lock_guard<std::mutex> lock(sendMutex_);
        return !sendQueue_.empty();
    }

    void TouchRecvTime() {
        std::lock_guard<std::mutex> lock(timeMutex_);
        lastRecvTime_ = std::chrono::steady_clock::now();
    }

    // 하트비트 타임아웃 체크용
    int64_t SecondsSinceLastRecv() const {
        std::lock_guard<std::mutex> lock(timeMutex_);
        auto now = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::seconds>(now - lastRecvTime_).count();
    }

private:
    int fd_;
    uint64_t sessionId_;
    std::atomic<SessionState> state_;

    mutable std::mutex infoMutex_;
    int64_t userId_ = 0;
    std::string nickname_;

    std::vector<char> recvBuffer_;

    std::mutex sendMutex_;
    std::deque<std::vector<char>> sendQueue_;
    size_t sendOffset_ = 0;

    mutable std::mutex timeMutex_;
    std::chrono::steady_clock::time_point lastRecvTime_;
};
