// ============================================================
// 서버 검증용 간이 테스트 클라이언트
// 사용법: ./test_client <host> <port> <username>
// 흐름: 접속 -> 회원가입 -> 로그인 -> 채팅 메시지 전송 -> 수신 대기
// ============================================================
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "../include/PacketDef.h"

static int ConnectTo(const char* host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        exit(1);
    }
    return fd;
}

static void SendAll(int fd, const std::vector<char>& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) { perror("send"); exit(1); }
        sent += n;
    }
}

// 수신 스레드: 서버 응답을 계속 읽어서 파싱, 콘솔에 출력
static void RecvLoop(int fd) {
    std::vector<char> buf;
    char tmp[4096];
    while (true) {
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            printf("[Client] 연결 종료됨\n");
            return;
        }
        buf.insert(buf.end(), tmp, tmp + n);

        size_t consumed = 0;
        while (buf.size() - consumed >= proto::HEADER_SIZE) {
            auto* header = reinterpret_cast<proto::PacketHeader*>(buf.data() + consumed);
            if (buf.size() - consumed < header->totalSize) break;

            const char* body = buf.data() + consumed + proto::HEADER_SIZE;
            size_t bodySize = header->totalSize - proto::HEADER_SIZE;
            proto::PacketReader reader(body, bodySize);

            auto type = static_cast<proto::PacketType>(header->type);
            switch (type) {
                case proto::PacketType::S2C_REGISTER_RESULT: {
                    int32_t code = reader.ReadInt32();
                    std::string msg = reader.ReadString();
                    printf("[Client] 회원가입 결과: code=%d msg=%s\n", code, msg.c_str());
                    break;
                }
                case proto::PacketType::S2C_LOGIN_RESULT: {
                    int32_t code = reader.ReadInt32();
                    std::string msg = reader.ReadString();
                    uint64_t userId = reader.ReadUInt64();
                    std::string nickname = reader.ReadString();
                    printf("[Client] 로그인 결과: code=%d msg=%s userId=%llu nickname=%s\n",
                           code, msg.c_str(), (unsigned long long)userId, nickname.c_str());
                    break;
                }
                case proto::PacketType::S2C_CHAT_BROADCAST: {
                    reader.ReadUInt64(); // fromUserId
                    std::string nickname = reader.ReadString();
                    std::string message = reader.ReadString();
                    printf("[Client] 채팅 수신 [%s]: %s\n", nickname.c_str(), message.c_str());
                    break;
                }
                case proto::PacketType::S2C_HEARTBEAT_ACK:
                    printf("[Client] 하트비트 응답 수신\n");
                    break;
                case proto::PacketType::S2C_FORCE_DISCONNECT:
                    printf("[Client] 서버로부터 강제 종료 통지 수신 (하트비트 타임아웃)\n");
                    break;
                default:
                    printf("[Client] 알 수 없는 패킷: %d\n", static_cast<int>(type));
            }
            consumed += header->totalSize;
        }
        buf.erase(buf.begin(), buf.begin() + consumed);
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("사용법: %s <host> <port> <username>\n", argv[0]);
        return 1;
    }
    const char* host = argv[1];
    int port = std::atoi(argv[2]);
    std::string username = argv[3];

    int fd = ConnectTo(host, port);
    std::thread recvThread(RecvLoop, fd);

    // 1) 회원가입 시도
    {
        proto::PacketWriter w(proto::PacketType::C2S_REGISTER);
        w.WriteString(username);
        w.WriteString("test1234");
        w.WriteString(username + "_nick");
        SendAll(fd, w.Finalize());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 2) 로그인
    {
        proto::PacketWriter w(proto::PacketType::C2S_LOGIN);
        w.WriteString(username);
        w.WriteString("test1234");
        SendAll(fd, w.Finalize());
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 3) 채팅 전송
    {
        proto::PacketWriter w(proto::PacketType::C2S_CHAT_MESSAGE);
        w.WriteString("안녕하세요, " + username + " 입니다!");
        SendAll(fd, w.Finalize());
    }

    std::this_thread::sleep_for(std::chrono::seconds(3));
    close(fd);
    recvThread.join();
    return 0;
}
