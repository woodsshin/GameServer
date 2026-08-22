#pragma once
#include <thread>
#include <vector>
#include <atomic>
#include <functional>
#include <memory>
#include <sys/epoll.h>
#include "Session.h"
#include "SessionManager.h"
#include "PacketDef.h"

// ============================================================
// epoll 기반 비동기 TCP 서버
//
// 스레드 구성:
//   1) Accept 스레드 (1개)
//        - listen 소켓만 담당. accept()로 새 연결이 들어오면
//          워커 스레드 풀 중 하나에 라운드로빈으로 소켓을 분배.
//        - accept 자체를 별도 스레드로 분리한 이유:
//          워커 스레드들이 대량 트래픽 처리로 바쁠 때도
//          신규 접속 수락이 지연되지 않도록 하기 위함.
//
//   2) IO 워커 스레드 (N개, CPU 코어 수 기반)
//        - 각자 독립된 epoll 인스턴스를 가짐 (epoll fd 1개씩)
//        - 자신에게 할당된 소켓들의 EPOLLIN/EPOLLOUT 이벤트 처리
//        - 이렇게 fd를 워커별로 분산시키면 단일 epoll에 락 경합이
//          생기는 구조를 피하고, 코어 수만큼 수평 확장 가능
//
//   3) DB 워커 스레드 (DBWorkerPool, 별도 클래스)
//        - 회원가입/로그인 등 블로킹 쿼리를 IO 스레드 밖에서 처리
//
// 패킷이 완성되면 PacketHandler 콜백으로 전달 (비즈니스 로직은
// Server 클래스나 상위 계층에서 처리하도록 관심사 분리)
// ============================================================

using PacketHandler = std::function<void(std::shared_ptr<Session>, proto::PacketType, const char* body, size_t bodySize)>;

class IoWorker {
public:
    IoWorker(int workerIndex, SessionManager& sessionManager, PacketHandler handler);
    ~IoWorker();

    void Start();
    void Stop();

    // accept 스레드가 새 fd를 이 워커에 등록 요청
    void RegisterNewSession(int fd);

    // 다른 스레드(브로드캐스트 등)에서 특정 세션에 쓰기 이벤트를 걸고 싶을 때
    void RequestWrite(int fd);

    int GetEpollFd() const { return epollFd_; }

private:
    void Run();
    void HandleReadable(std::shared_ptr<Session>& session);
    void HandleWritable(std::shared_ptr<Session>& session);
    void CloseSession(std::shared_ptr<Session>& session);
    void TryDispatchPackets(std::shared_ptr<Session>& session);

    int workerIndex_;
    int epollFd_;
    int wakeupEventFd_; // RegisterNewSession/RequestWrite로 epoll_wait를 깨우기 위한 eventfd

    std::thread thread_;
    std::atomic<bool> running_{false};

    SessionManager& sessionManager_;
    PacketHandler packetHandler_;

    // eventfd로 깨어난 뒤 처리할 대기열 (신규 fd 등록 등)
    std::mutex pendingMutex_;
    std::vector<int> pendingNewFds_;
    std::vector<int> pendingWriteFds_;

    static std::atomic<uint64_t> nextSessionId_;
};

class NetServer {
public:
    NetServer(uint16_t port, int workerCount, SessionManager& sessionManager, PacketHandler handler);
    ~NetServer();

    bool Start();
    void Stop();

    // 특정 세션에 패킷 전송 (어느 스레드에서 호출해도 안전)
    void SendPacket(std::shared_ptr<Session> session, std::vector<char> packetData);

    // 인증된 전체 세션에 브로드캐스트
    void BroadcastPacket(SessionManager& sessionManager, std::vector<char> packetData, uint64_t excludeSessionId = 0);

private:
    void AcceptLoop();
    int SetNonBlocking(int fd);

    uint16_t port_;
    int listenFd_ = -1;
    std::thread acceptThread_;
    std::atomic<bool> running_{false};

    std::vector<std::unique_ptr<IoWorker>> workers_;
    std::atomic<size_t> nextWorkerRoundRobin_{0};

    SessionManager& sessionManager_;
    PacketHandler packetHandler_;

    // fd -> workerIndex 매핑 (송신 시 어느 워커의 epoll에 EPOLLOUT 걸어야 하는지 알기 위함)
    std::mutex fdWorkerMapMutex_;
    std::unordered_map<int, int> fdToWorkerIndex_;
};
