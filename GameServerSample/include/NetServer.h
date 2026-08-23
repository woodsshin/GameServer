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
//          worker 스레드 풀 중 하나에 라운드로빈으로 소켓을 분배.
//        - accept 자체를 별도 스레드로 분리한 이유:
//          worker 스레드들이 대량 트래픽 처리로 바쁠 때도
//          신규 접속 수락이 지연되지 않도록 하기 위함.
//
//   2) IO worker 스레드 (N개, CPU 코어 수 기반)
//        - 각자 독립된 epoll 인스턴스를 가짐 (epoll fd 1개씩)
//        - Edge-triggered(EPOLLET) 모드로 등록되며, 각 이벤트마다
//          EAGAIN이 나올 때까지 read/write를 반복하는 논블로킹 루프로 처리
//          (Level-triggered 대비 동일 fd에 대한 epoll_wait 반환 횟수를 줄여
//          시스템 콜 총량을 절감한다. 자세한 이유는 Run()의 주석 참고)
//        - 자신에게 할당된 소켓들의 EPOLLIN/EPOLLOUT 이벤트 처리
//        - 이렇게 fd를 worker별로 분산시키면 단일 epoll에 락 경합이
//          생기는 구조를 피하고, 코어 수만큼 수평 확장 가능
//        - 강제 종료 큐(pendingForceCloseFds_)를 별도로 두어, 하트비트
//          타임아웃 등으로 서버가 "이 세션은 끊어야 한다"고 판단했을 때
//          클라이언트의 자발적 연결 해제를 기다리지 않고 해당 세션이
//          속한 IoWorker가 직접 소켓을 닫도록 한다.
//
//   3) DB worker 스레드 (DBWorkerPool, 별도 클래스)
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

    // accept 스레드가 새 fd를 이 worker에 등록 요청
    void RegisterNewSession(int fd);

    // 다른 스레드(브로드캐스트 등)에서 특정 세션에 쓰기 이벤트를 걸고 싶을 때
    void RequestWrite(int fd);

    // 서버 주도로 특정 세션을 강제 종료하고 싶을 때 (예: 하트비트 타임아웃 감시 스레드).
    // 실제 close()는 이 fd를 소유한 IoWorker 스레드에서만 일어나야 하므로,
    // 큐에 적재만 하고 eventfd로 깨워서 해당 스레드가 직접 처리하게 한다.
    void RequestForceClose(int fd);

    int GetEpollFd() const { return epollFd_; }

private:
    void Run();
    void HandleReadable(std::shared_ptr<Session>& session);
    void HandleWritable(std::shared_ptr<Session>& session);
    void CloseSession(std::shared_ptr<Session>& session);
    void TryDispatchPackets(std::shared_ptr<Session>& session);

    int workerIndex_;
    int epollFd_;
    int wakeupEventFd_; // RegisterNewSession/RequestWrite/RequestForceClose로 epoll_wait를 깨우기 위한 eventfd

    std::thread thread_;
    std::atomic<bool> running_{false};

    SessionManager& sessionManager_;
    PacketHandler packetHandler_;

    // eventfd로 깨어난 뒤 처리할 대기열 (신규 fd 등록, 강제 종료 등)
    std::mutex pendingMutex_;
    std::vector<int> pendingNewFds_;
    std::vector<int> pendingWriteFds_;
    std::vector<int> pendingForceCloseFds_;

    static std::atomic<uint64_t> nextSessionId_;
};

class NetServer {
public:
    NetServer(uint16_t port, int workerCount, SessionManager& sessionManager, PacketHandler handler);
    ~NetServer();

    bool Start();
    void Stop();

    // 특정 세션에 패킷 전송 (어느 스레드에서 호출해도 안전).
    // packetData는 shared_ptr로 받아 브로드캐스트 시 세션마다 복사하지 않고 공유한다.
    void SendPacket(std::shared_ptr<Session> session, PacketBuffer packetData);

    // 인증된 전체 세션에 브로드캐스트. 버퍼 하나를 모든 세션이 shared_ptr로
    // 공유하므로, 세션이 N명이어도 실제 vector<char> 복사는 발생하지 않는다.
    void BroadcastPacket(SessionManager& sessionManager, PacketBuffer packetData, uint64_t excludeSessionId = 0);

    // 특정 세션을 서버 주도로 강제 종료 (하트비트 타임아웃 등에서 사용).
    // 호출 스레드에 관계없이 안전하며, 실제 종료는 해당 세션을 소유한
    // IoWorker의 강제종료 큐를 통해 그 worker 스레드에서 수행된다.
    void ForceDisconnect(std::shared_ptr<Session> session);

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

    // fd -> workerIndex 매핑 (송신/강제종료 시 어느 worker의 epoll을 건드려야 하는지 알기 위함)
    std::mutex fdWorkerMapMutex_;
    std::unordered_map<int, int> fdToWorkerIndex_;
};
