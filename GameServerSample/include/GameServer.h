#pragma once
#include <memory>
#include "NetServer.h"
#include "SessionManager.h"
#include "DBConnectionPool.h"
#include "DBWorker.h"
#include "PacketDef.h"

// ============================================================
// GameServer: 프로토콜 계층(NetServer)과 DB 계층(DBWorkerPool)을
// 엮어 실제 비즈니스 로직(회원가입/로그인/채팅)을 구현하는 상위 계층.
//
// 관심사 분리:
//   NetServer      -> "바이트를 어떻게 주고받는가" (epoll, 소켓)
//   DBWorkerPool   -> "블로킹 쿼리를 어떻게 비동기로 실행하는가"
//   GameServer     -> "패킷이 오면 무엇을 할 것인가" (도메인 로직)
// ============================================================
class GameServer {
public:
    GameServer(uint16_t port, int ioWorkerCount, const DBConfig& dbConfig, int dbWorkerCount);

    bool Start();
    void Stop();

private:
    // NetServer가 패킷 조립을 완료하면 호출하는 콜백. IoWorker 스레드 컨텍스트에서 실행됨.
    void OnPacket(std::shared_ptr<Session> session, proto::PacketType type, const char* body, size_t bodySize);

    void HandleRegister(std::shared_ptr<Session> session, const char* body, size_t bodySize);
    void HandleLogin(std::shared_ptr<Session> session, const char* body, size_t bodySize);
    void HandleChatMessage(std::shared_ptr<Session> session, const char* body, size_t bodySize);
    void HandleHeartbeat(std::shared_ptr<Session> session);

    void SendRegisterResult(std::shared_ptr<Session> session, int32_t resultCode, const std::string& message);
    void SendLoginResult(std::shared_ptr<Session> session, int32_t resultCode, const std::string& message,
                          int64_t userId = 0, const std::string& nickname = "");

    // 유휴 커넥션(하트비트 끊긴 세션) 정리 스레드
    void HeartbeatMonitorLoop();

    DBConnectionPool dbPool_;
    DBWorkerPool dbWorkerPool_;
    SessionManager sessionManager_;
    NetServer netServer_;

    std::thread heartbeatThread_;
    std::atomic<bool> running_{false};

    static constexpr int64_t HEARTBEAT_TIMEOUT_SEC = 60;
};
