#include "NetServer.h"
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cstdio>

std::atomic<uint64_t> IoWorker::nextSessionId_{1};

// ================================================================
// IoWorker
// ================================================================

IoWorker::IoWorker(int workerIndex, SessionManager& sessionManager, PacketHandler handler)
    : workerIndex_(workerIndex), sessionManager_(sessionManager), packetHandler_(std::move(handler)) {
    epollFd_ = epoll_create1(0);
    if (epollFd_ < 0) {
        throw std::runtime_error("epoll_create1 실패");
    }

    // eventfd: 다른 스레드가 이 worker의 epoll_wait를 깨울 때 사용하는 통지 채널
    wakeupEventFd_ = eventfd(0, EFD_NONBLOCK);
    if (wakeupEventFd_ < 0) {
        throw std::runtime_error("eventfd 생성 실패");
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = wakeupEventFd_;
    epoll_ctl(epollFd_, EPOLL_CTL_ADD, wakeupEventFd_, &ev);
}

IoWorker::~IoWorker() {
    Stop();
    close(wakeupEventFd_);
    close(epollFd_);
}

void IoWorker::Start() {
    running_ = true;
    thread_ = std::thread(&IoWorker::Run, this);
}

void IoWorker::Stop() {
    if (!running_.exchange(false)) return;
    // eventfd에 값을 써서 epoll_wait를 즉시 깨움 (블로킹 탈출용)
    uint64_t one = 1;
    ssize_t n = write(wakeupEventFd_, &one, sizeof(one));
    (void)n;
    if (thread_.joinable()) thread_.join();
}

void IoWorker::RegisterNewSession(int fd) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingNewFds_.push_back(fd);
    }
    uint64_t one = 1;
    ssize_t n = write(wakeupEventFd_, &one, sizeof(one));
    (void)n;
}

void IoWorker::RequestWrite(int fd) {
    {
        std::lock_guard<std::mutex> lock(pendingMutex_);
        pendingWriteFds_.push_back(fd);
    }
    uint64_t one = 1;
    ssize_t n = write(wakeupEventFd_, &one, sizeof(one));
    (void)n;
}

void IoWorker::Run() {
    constexpr int MAX_EVENTS = 256;
    std::vector<epoll_event> events(MAX_EVENTS);

    // fd -> Session 매핑은 worker 스레드 하나만 접근하므로 lock 불필요 (fd는 worker 간 공유되지 않음)
    std::unordered_map<int, std::shared_ptr<Session>> localSessions;

    while (running_) {
        int n = epoll_wait(epollFd_, events.data(), MAX_EVENTS, 1000 /*ms, 타임아웃으로 주기적 깨어나서 running_ 체크*/);
        if (n < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[IoWorker %d] epoll_wait 오류: %s\n", workerIndex_, strerror(errno));
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            // 1) 다른 스레드로부터의 깨우기 신호 처리
            if (fd == wakeupEventFd_) {
                uint64_t val;
                ssize_t r = read(wakeupEventFd_, &val, sizeof(val));
                (void)r;

                std::vector<int> newFds;
                std::vector<int> writeFds;
                {
                    std::lock_guard<std::mutex> lock(pendingMutex_);
                    newFds.swap(pendingNewFds_);
                    writeFds.swap(pendingWriteFds_);
                }

                // 신규 소켓을 이 worker의 epoll에 등록
                for (int newFd : newFds) {
                    uint64_t sessionId = nextSessionId_.fetch_add(1);
                    auto session = std::make_shared<Session>(newFd, sessionId);
                    sessionManager_.Add(session);
                    localSessions[newFd] = session;

                    epoll_event sev{};
                    sev.events = EPOLLIN | EPOLLRDHUP;
                    sev.data.fd = newFd;
                    if (epoll_ctl(epollFd_, EPOLL_CTL_ADD, newFd, &sev) < 0) {
                        fprintf(stderr, "[IoWorker %d] epoll_ctl(ADD) 실패 fd=%d: %s\n",
                                workerIndex_, newFd, strerror(errno));
                        close(newFd);
                        sessionManager_.Remove(sessionId);
                        localSessions.erase(newFd);
                    }
                }

                // 송신 대기 소켓에 EPOLLOUT 추가 등록 (edge-trigger 아닌 level-trigger 사용,
                // 포트폴리오 목적상 단순성 우선. 실무에선 EPOLLET + 논블로킹 루프도 흔함)
                for (int wfd : writeFds) {
                    auto it = localSessions.find(wfd);
                    if (it == localSessions.end()) continue;
                    epoll_event sev{};
                    sev.events = EPOLLIN | EPOLLOUT | EPOLLRDHUP;
                    sev.data.fd = wfd;
                    epoll_ctl(epollFd_, EPOLL_CTL_MOD, wfd, &sev);
                }
                continue;
            }

            // 2) 일반 클라이언트 소켓 이벤트
            auto it = localSessions.find(fd);
            if (it == localSessions.end()) continue;
            auto session = it->second;

            if (events[i].events & (EPOLLHUP | EPOLLERR | EPOLLRDHUP)) {
                CloseSession(session);
                localSessions.erase(fd);
                continue;
            }

            if (events[i].events & EPOLLIN) {
                HandleReadable(session);
                if (session->GetState() == SessionState::CLOSING) {
                    localSessions.erase(fd);
                    continue;
                }
            }

            if (events[i].events & EPOLLOUT) {
                HandleWritable(session);
            }
        }
    }

    // worker 종료 시 남은 세션 정리
    for (auto& [fd, session] : localSessions) {
        sessionManager_.Remove(session->GetSessionId());
        close(fd);
    }
}

void IoWorker::HandleReadable(std::shared_ptr<Session>& session) {
    char tmp[4096];
    int fd = session->GetFd();

    while (true) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) {
            auto& buf = session->RecvBuffer();
            buf.insert(buf.end(), tmp, tmp + n);
            session->TouchRecvTime();

            // recv 버퍼가 넘치는 것 방지 (비정상 클라이언트 방어)
            if (buf.size() > proto::MAX_PACKET_SIZE * 4) {
                fprintf(stderr, "[IoWorker] fd=%d 수신 버퍼 초과, 연결 종료\n", fd);
                CloseSession(session);
                return;
            }
        } else if (n == 0) {
            // 정상 종료 (FIN)
            CloseSession(session);
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // 이번 이벤트에서 읽을 수 있는 만큼 다 읽음
            }
            if (errno == EINTR) continue;
            CloseSession(session);
            return;
        }
    }

    TryDispatchPackets(session);
}

void IoWorker::TryDispatchPackets(std::shared_ptr<Session>& session) {
    // 누적된 recv 버퍼에서 완성된 패킷(들)을 잘라내어 핸들러로 전달.
    // TCP는 스트림 프로토콜이라 한 번의 recv에 여러 패킷이 뭉쳐 오거나,
    // 하나의 패킷이 여러 번의 recv에 걸쳐 나뉘어 올 수 있으므로 길이 기반으로 분리한다.
    auto& buf = session->RecvBuffer();

    size_t consumed = 0;
    while (buf.size() - consumed >= proto::HEADER_SIZE) {
        const char* base = buf.data() + consumed;
        auto* header = reinterpret_cast<const proto::PacketHeader*>(base);

        if (header->totalSize < proto::HEADER_SIZE || header->totalSize > proto::MAX_PACKET_SIZE) {
            fprintf(stderr, "[IoWorker] 비정상 패킷 크기(%u), 연결 종료\n", header->totalSize);
            CloseSession(session);
            return;
        }

        if (buf.size() - consumed < header->totalSize) {
            break; // 아직 body가 다 도착하지 않음 -> 다음 recv를 기다림
        }

        auto type = static_cast<proto::PacketType>(header->type);
        const char* body = base + proto::HEADER_SIZE;
        size_t bodySize = header->totalSize - proto::HEADER_SIZE;

        packetHandler_(session, type, body, bodySize);

        consumed += header->totalSize;
    }

    if (consumed > 0) {
        buf.erase(buf.begin(), buf.begin() + consumed);
    }
}

void IoWorker::HandleWritable(std::shared_ptr<Session>& session) {
    int fd = session->GetFd();

    while (true) {
        std::vector<char>* data = nullptr;
        size_t* offset = nullptr;
        if (!session->TryPeekSendFront(&data, &offset)) {
            break; // 더 보낼 게 없음
        }

        size_t remaining = data->size() - *offset;
        ssize_t n = send(fd, data->data() + *offset, remaining, MSG_NOSIGNAL);
        if (n > 0) {
            *offset += static_cast<size_t>(n);
            if (*offset >= data->size()) {
                session->PopSendFront();
            }
            if (static_cast<size_t>(n) < remaining) {
                break; // 소켓 버퍼가 꽉 참, 다음 EPOLLOUT을 기다림
            }
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            if (errno == EINTR) continue;
            CloseSession(session);
            return;
        }
    }

    // 더 보낼 게 없으면 EPOLLOUT 감시를 해제해 불필요한 wake-up을 줄임
    if (!session->HasPendingSend()) {
        epoll_event sev{};
        sev.events = EPOLLIN | EPOLLRDHUP;
        sev.data.fd = fd;
        epoll_ctl(epollFd_, EPOLL_CTL_MOD, fd, &sev);
    }
}

void IoWorker::CloseSession(std::shared_ptr<Session>& session) {
    session->SetState(SessionState::CLOSING);
    int fd = session->GetFd();
    epoll_ctl(epollFd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    sessionManager_.Remove(session->GetSessionId());
}

// ================================================================
// NetServer
// ================================================================

NetServer::NetServer(uint16_t port, int workerCount, SessionManager& sessionManager, PacketHandler handler)
    : port_(port), sessionManager_(sessionManager), packetHandler_(std::move(handler)) {
    for (int i = 0; i < workerCount; ++i) {
        workers_.push_back(std::make_unique<IoWorker>(i, sessionManager_, packetHandler_));
    }
}

NetServer::~NetServer() {
    Stop();
}

int NetServer::SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

bool NetServer::Start() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        fprintf(stderr, "socket() 실패: %s\n", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // SO_REUSEPORT를 쓰면 accept 자체를 멀티스레드로 분산할 수도 있으나,
    // 여기서는 accept 스레드 1개 + worker 분배 구조를 명확히 보여주기 위해 미사용.

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);

    if (bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        fprintf(stderr, "bind() 실패: %s\n", strerror(errno));
        return false;
    }

    if (listen(listenFd_, SOMAXCONN) < 0) {
        fprintf(stderr, "listen() 실패: %s\n", strerror(errno));
        return false;
    }

    SetNonBlocking(listenFd_);

    for (auto& w : workers_) {
        w->Start();
    }

    running_ = true;
    acceptThread_ = std::thread(&NetServer::AcceptLoop, this);

    printf("[NetServer] 포트 %d 에서 리스닝 시작 (worker %zu개)\n", port_, workers_.size());
    return true;
}

void NetServer::Stop() {
    if (!running_.exchange(false)) return;

    if (listenFd_ >= 0) {
        shutdown(listenFd_, SHUT_RDWR);
        close(listenFd_);
        listenFd_ = -1;
    }
    if (acceptThread_.joinable()) acceptThread_.join();

    for (auto& w : workers_) {
        w->Stop();
    }
}

void NetServer::AcceptLoop() {
    // accept 스레드는 별도의 작은 epoll로 listenFd_만 감시.
    // (poll/select도 가능하지만 일관성을 위해 epoll 사용)
    int acceptEpollFd = epoll_create1(0);
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = listenFd_;
    epoll_ctl(acceptEpollFd, EPOLL_CTL_ADD, listenFd_, &ev);

    std::vector<epoll_event> events(16);

    while (running_) {
        int n = epoll_wait(acceptEpollFd, events.data(), events.size(), 1000);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; ++i) {
            if (events[i].data.fd != listenFd_) continue;

            while (true) {
                sockaddr_in clientAddr{};
                socklen_t addrLen = sizeof(clientAddr);
                int clientFd = accept(listenFd_, reinterpret_cast<sockaddr*>(&clientAddr), &addrLen);
                if (clientFd < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    if (errno == EINTR) continue;
                    break;
                }

                SetNonBlocking(clientFd);

                // TCP_NODELAY: 채팅처럼 지연에 민감한 소규모 패킷 다수 전송 시 Nagle 알고리즘 비활성화
                int nodelay = 1;
                setsockopt(clientFd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

                // 라운드로빈으로 worker 선택 -> 대용량 유저를 코어 수만큼 수평 분산
                size_t idx = nextWorkerRoundRobin_.fetch_add(1) % workers_.size();
                {
                    std::lock_guard<std::mutex> lock(fdWorkerMapMutex_);
                    fdToWorkerIndex_[clientFd] = static_cast<int>(idx);
                }
                workers_[idx]->RegisterNewSession(clientFd);

                char ipBuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &clientAddr.sin_addr, ipBuf, sizeof(ipBuf));
                printf("[Accept] 새 연결: %s:%d (fd=%d) -> Worker %zu\n",
                       ipBuf, ntohs(clientAddr.sin_port), clientFd, idx);
            }
        }
    }

    close(acceptEpollFd);
}

void NetServer::SendPacket(std::shared_ptr<Session> session, std::vector<char> packetData) {
    if (!session || session->GetState() == SessionState::CLOSING) return;

    bool becameNonEmpty = session->EnqueueSend(std::move(packetData));
    if (!becameNonEmpty) {
        return; // 이미 flush 진행 중이던 큐라면, 해당 worker가 알아서 마저 보냄
    }

    int fd = session->GetFd();
    int workerIdx = -1;
    {
        std::lock_guard<std::mutex> lock(fdWorkerMapMutex_);
        auto it = fdToWorkerIndex_.find(fd);
        if (it != fdToWorkerIndex_.end()) workerIdx = it->second;
    }
    if (workerIdx >= 0) {
        workers_[workerIdx]->RequestWrite(fd);
    }
}

void NetServer::BroadcastPacket(SessionManager& sessionManager, std::vector<char> packetData, uint64_t excludeSessionId) {
    // 인증된 세션 스냅샷을 뜬 뒤, 각 세션에 동일 버퍼를 복사 전송.
    // 실무에서는 레퍼런스 카운트 버퍼(shared_ptr<vector<char>>) 공유로 복사 비용을
    // 줄이는 최적화가 흔하지만, 여기서는 명확성을 위해 단순 복사로 작성.
    auto sessions = sessionManager.SnapshotAuthenticated();
    for (auto& s : sessions) {
        if (s->GetSessionId() == excludeSessionId) continue;
        SendPacket(s, packetData); // 벡터 복사(오버로드가 값 전달이라 매 호출마다 복사됨)
    }
}
