#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>

// ============================================================
// 클라이언트 1명당 1개 생성되는 세션 객체
// - non-blocking 소켓의 recv 버퍼 누적 / 부분 패킷 조립 담당
// - send는 큐잉 후 EPOLLOUT 이벤트에서 flush (writev 스타일)
// - sendMutex_로 여러 스레드(브로드캐스트 스레드 vs 자기 자신 처리 스레드)가
//   동시에 같은 세션에 쓰기 시도해도 안전하게 함
// - 송신 큐는 shared_ptr<const vector<char>>로 버퍼를 보관한다. 브로드캐스트처럼
//   동일한 패킷을 N명에게 보낼 때, 세션마다 vector를 복사하지 않고 참조 카운트만
//   늘려서 공유하기 위함 (자세한 이유는 NetServer::BroadcastPacket 주석 참고)
// ============================================================

using PacketBuffer = std::shared_ptr<const std::vector<char>>;

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
    // 버퍼는 shared_ptr<const vector<char>>로 보관: 브로드캐스트 시 동일 패킷을
    // 여러 세션이 참조 카운트만 늘려 공유하고, 실제 바이트 복사는 소켓에
    // write()/send()로 내보낼 때 커널이 하는 복사 한 번뿐이다.
    // 반환값: 이 호출로 인해 "빈 큐 -> 비어있지 않은 큐"가 되었는지 여부
    // (epoll에 EPOLLOUT 등록이 필요한 시점 판단용)
    bool EnqueueSend(PacketBuffer data) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        bool wasEmpty = sendQueue_.empty();
        sendQueue_.push_back(std::move(data));
        return wasEmpty;
    }

    // 현재 flush 대상 프레임 하나를 꺼냄 (부분 전송 지원 위해 offset 함께 관리)
    bool TryPeekSendFront(PacketBuffer* outData, size_t** outOffset) {
        std::lock_guard<std::mutex> lock(sendMutex_);
        if (sendQueue_.empty()) return false;
        *outData = sendQueue_.front();
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

    // ---- 서버 주도 강제 종료 플래그 ----
    // 하트비트 타임아웃 등으로 서버가 이 세션을 끊기로 결정했음을 표시.
    // 실제 close()는 이 세션이 속한 IoWorker 스레드에서만 수행되어야 하므로,
    // 여기서는 "끊어야 한다"는 의사만 스레드 세이프하게 기록해두고,
    // IoWorker가 자신의 강제종료 큐를 처리할 때 이 플래그를 보고 최종 정리한다.
    void MarkForForceClose() { forceClose_.store(true); }
    bool IsMarkedForForceClose() const { return forceClose_.load(); }

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
    std::deque<PacketBuffer> sendQueue_;
    size_t sendOffset_ = 0;

    mutable std::mutex timeMutex_;
    std::chrono::steady_clock::time_point lastRecvTime_;

    std::atomic<bool> forceClose_{false};
};
