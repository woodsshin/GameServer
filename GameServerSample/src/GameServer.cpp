#include "GameServer.h"
#include "PasswordHash.h"
#include <cstdio>
#include <chrono>

GameServer::GameServer(uint16_t port, int ioWorkerCount, const DBConfig& dbConfig, int dbWorkerCount)
    : dbPool_(dbConfig),
      dbWorkerPool_(dbPool_, dbWorkerCount),
      netServer_(port, ioWorkerCount, sessionManager_,
                 [this](std::shared_ptr<Session> s, proto::PacketType t, const char* b, size_t sz) {
                     OnPacket(std::move(s), t, b, sz);
                 }) {}

bool GameServer::Start() {
    if (!netServer_.Start()) {
        return false;
    }
    running_ = true;
    heartbeatThread_ = std::thread(&GameServer::HeartbeatMonitorLoop, this);
    return true;
}

void GameServer::Stop() {
    running_ = false;
    if (heartbeatThread_.joinable()) heartbeatThread_.join();
    netServer_.Stop();
    dbWorkerPool_.Stop();
}

// ================================================================
// 패킷 라우팅
// ================================================================
void GameServer::OnPacket(std::shared_ptr<Session> session, proto::PacketType type, const char* body, size_t bodySize) {
    switch (type) {
        case proto::PacketType::C2S_REGISTER:
            HandleRegister(session, body, bodySize);
            break;
        case proto::PacketType::C2S_LOGIN:
            HandleLogin(session, body, bodySize);
            break;
        case proto::PacketType::C2S_CHAT_MESSAGE:
            HandleChatMessage(session, body, bodySize);
            break;
        case proto::PacketType::C2S_HEARTBEAT:
            HandleHeartbeat(session);
            break;
        default:
            fprintf(stderr, "[GameServer] 알 수 없는 패킷 타입: %d\n", static_cast<int>(type));
            break;
    }
}

// ================================================================
// 회원가입
//   흐름: IoWorker 스레드에서 패킷 파싱 -> DB 작업을 DBWorkerPool에 위임
//         -> DB worker 스레드에서 쿼리 실행 -> 완료 후 NetServer::SendPacket으로 응답
//   (SendPacket은 어느 스레드에서 호출해도 안전하게 설계되어 있음)
// ================================================================
void GameServer::HandleRegister(std::shared_ptr<Session> session, const char* body, size_t bodySize) {
    proto::PacketReader reader(body, bodySize);
    std::string username, password, nickname;
    try {
        username = reader.ReadString();
        password = reader.ReadString();
        nickname = reader.ReadString();
    } catch (const std::exception&) {
        SendRegisterResult(session, -1, "잘못된 패킷 형식");
        return;
    }

    if (username.empty() || password.size() < 4 || nickname.empty()) {
        SendRegisterResult(session, -2, "입력값 검증 실패 (username/password/nickname)");
        return;
    }

    DBTask task;
    task.execute = [this, session, username, password, nickname](MYSQL* conn) {
        // 1) 중복 아이디 체크
        std::string checkQuery = "SELECT id FROM users WHERE username = ?";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(stmt, checkQuery.c_str(), checkQuery.size());

        MYSQL_BIND bindParam[1]{};
        bindParam[0].buffer_type = MYSQL_TYPE_STRING;
        bindParam[0].buffer = (void*)username.c_str();
        bindParam[0].buffer_length = username.size();
        mysql_stmt_bind_param(stmt, bindParam);
        mysql_stmt_execute(stmt);
        mysql_stmt_store_result(stmt);

        bool exists = mysql_stmt_num_rows(stmt) > 0;
        mysql_stmt_close(stmt);

        if (exists) {
            SendRegisterResult(session, -3, "이미 존재하는 아이디입니다");
            return;
        }

        // 2) salt + 해시 생성 후 INSERT
        std::string salt = security::GenerateSalt();
        std::string hash = security::HashPassword(password, salt);

        std::string insertQuery =
            "INSERT INTO users (username, password_hash, salt, nickname, created_at) "
            "VALUES (?, ?, ?, ?, NOW())";
        MYSQL_STMT* insertStmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(insertStmt, insertQuery.c_str(), insertQuery.size());

        MYSQL_BIND insertBind[4]{};
        insertBind[0].buffer_type = MYSQL_TYPE_STRING;
        insertBind[0].buffer = (void*)username.c_str();
        insertBind[0].buffer_length = username.size();

        insertBind[1].buffer_type = MYSQL_TYPE_STRING;
        insertBind[1].buffer = (void*)hash.c_str();
        insertBind[1].buffer_length = hash.size();

        insertBind[2].buffer_type = MYSQL_TYPE_STRING;
        insertBind[2].buffer = (void*)salt.c_str();
        insertBind[2].buffer_length = salt.size();

        insertBind[3].buffer_type = MYSQL_TYPE_STRING;
        insertBind[3].buffer = (void*)nickname.c_str();
        insertBind[3].buffer_length = nickname.size();

        mysql_stmt_bind_param(insertStmt, insertBind);

        if (mysql_stmt_execute(insertStmt) != 0) {
            fprintf(stderr, "[Register] INSERT 실패: %s\n", mysql_stmt_error(insertStmt));
            mysql_stmt_close(insertStmt);
            SendRegisterResult(session, -4, "서버 내부 오류 (DB)");
            return;
        }
        mysql_stmt_close(insertStmt);

        SendRegisterResult(session, 0, "회원가입 성공");
    };

    dbWorkerPool_.Enqueue(std::move(task));
}

// ================================================================
// 로그인
// ================================================================
void GameServer::HandleLogin(std::shared_ptr<Session> session, const char* body, size_t bodySize) {
    proto::PacketReader reader(body, bodySize);
    std::string username, password;
    try {
        username = reader.ReadString();
        password = reader.ReadString();
    } catch (const std::exception&) {
        SendLoginResult(session, -1, "잘못된 패킷 형식");
        return;
    }

    DBTask task;
    task.execute = [this, session, username, password](MYSQL* conn) {
        std::string query =
            "SELECT id, password_hash, salt, nickname FROM users WHERE username = ? LIMIT 1";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(stmt, query.c_str(), query.size());

        MYSQL_BIND paramBind[1]{};
        paramBind[0].buffer_type = MYSQL_TYPE_STRING;
        paramBind[0].buffer = (void*)username.c_str();
        paramBind[0].buffer_length = username.size();
        mysql_stmt_bind_param(stmt, paramBind);
        mysql_stmt_execute(stmt);

        int64_t userId = 0;
        char hashBuf[128]{}, saltBuf[64]{}, nicknameBuf[128]{};
        unsigned long hashLen = 0, saltLen = 0, nicknameLen = 0;

        MYSQL_BIND resultBind[4]{};
        resultBind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        resultBind[0].buffer = &userId;

        resultBind[1].buffer_type = MYSQL_TYPE_STRING;
        resultBind[1].buffer = hashBuf;
        resultBind[1].buffer_length = sizeof(hashBuf);
        resultBind[1].length = &hashLen;

        resultBind[2].buffer_type = MYSQL_TYPE_STRING;
        resultBind[2].buffer = saltBuf;
        resultBind[2].buffer_length = sizeof(saltBuf);
        resultBind[2].length = &saltLen;

        resultBind[3].buffer_type = MYSQL_TYPE_STRING;
        resultBind[3].buffer = nicknameBuf;
        resultBind[3].buffer_length = sizeof(nicknameBuf);
        resultBind[3].length = &nicknameLen;

        mysql_stmt_bind_result(stmt, resultBind);
        mysql_stmt_store_result(stmt);

        bool found = (mysql_stmt_fetch(stmt) == 0);
        mysql_stmt_close(stmt);

        if (!found) {
            SendLoginResult(session, -2, "존재하지 않는 계정입니다");
            return;
        }

        std::string hash(hashBuf, hashLen);
        std::string salt(saltBuf, saltLen);
        std::string nickname(nicknameBuf, nicknameLen);

        if (!security::VerifyPassword(password, salt, hash)) {
            SendLoginResult(session, -3, "비밀번호가 일치하지 않습니다");
            return;
        }

        // 로그인 성공: 세션에 유저 정보 반영
        session->SetUserInfo(userId, nickname);
        session->SetState(SessionState::AUTHENTICATED);

        // 마지막 로그인 시간 갱신 (실패해도 로그인 자체는 성공 처리)
        std::string updateQuery = "UPDATE users SET last_login_at = NOW() WHERE id = ?";
        MYSQL_STMT* updateStmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(updateStmt, updateQuery.c_str(), updateQuery.size());
        MYSQL_BIND updateBind[1]{};
        updateBind[0].buffer_type = MYSQL_TYPE_LONGLONG;
        updateBind[0].buffer = &userId;
        mysql_stmt_bind_param(updateStmt, updateBind);
        mysql_stmt_execute(updateStmt);
        mysql_stmt_close(updateStmt);

        SendLoginResult(session, 0, "로그인 성공", userId, nickname);
    };

    dbWorkerPool_.Enqueue(std::move(task));
}

// ================================================================
// 채팅: 인증된 유저만 전송 가능, 전체 브로드캐스트
// ================================================================
void GameServer::HandleChatMessage(std::shared_ptr<Session> session, const char* body, size_t bodySize) {
    if (session->GetState() != SessionState::AUTHENTICATED) {
        return; // 미인증 유저의 채팅 시도는 조용히 무시 (혹은 별도 에러 응답 가능)
    }

    proto::PacketReader reader(body, bodySize);
    std::string message;
    try {
        message = reader.ReadString();
    } catch (const std::exception&) {
        return;
    }

    if (message.empty() || message.size() > 500) return;

    proto::PacketWriter writer(proto::PacketType::S2C_CHAT_BROADCAST);
    writer.WriteUInt64(static_cast<uint64_t>(session->GetUserId()));
    writer.WriteString(session->GetNickname());
    writer.WriteString(message);
    auto packet = writer.Finalize();

    // 브로드캐스트는 네트워크 I/O이므로 DB 스레드가 아니라 이 IoWorker 스레드에서 바로 처리.
    // (SendPacket이 내부적으로 대상 세션이 속한 worker에 안전하게 큐잉하므로 스레드 경계를 넘어도 안전)
    netServer_.BroadcastPacket(sessionManager_, packet);

    // 채팅 로그를 DB에 비동기로 적재 (분석/신고 대응용, 실패해도 서비스에 영향 없도록 fire-and-forget)
    int64_t userId = session->GetUserId();
    DBTask logTask;
    logTask.execute = [userId, message](MYSQL* conn) {
        std::string query = "INSERT INTO chat_logs (user_id, message, created_at) VALUES (?, ?, NOW())";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(stmt, query.c_str(), query.size());

        MYSQL_BIND bindParam[2]{};
        bindParam[0].buffer_type = MYSQL_TYPE_LONGLONG;
        bindParam[0].buffer = (void*)&userId;

        bindParam[1].buffer_type = MYSQL_TYPE_STRING;
        bindParam[1].buffer = (void*)message.c_str();
        bindParam[1].buffer_length = message.size();

        mysql_stmt_bind_param(stmt, bindParam);
        mysql_stmt_execute(stmt);
        mysql_stmt_close(stmt);
    };
    dbWorkerPool_.Enqueue(std::move(logTask));
}

void GameServer::HandleHeartbeat(std::shared_ptr<Session> session) {
    session->TouchRecvTime();
    proto::PacketWriter writer(proto::PacketType::S2C_HEARTBEAT_ACK);
    netServer_.SendPacket(session, writer.Finalize());
}

// ================================================================
// 응답 헬퍼
// ================================================================
void GameServer::SendRegisterResult(std::shared_ptr<Session> session, int32_t resultCode, const std::string& message) {
    proto::PacketWriter writer(proto::PacketType::S2C_REGISTER_RESULT);
    writer.WriteInt32(resultCode);
    writer.WriteString(message);
    netServer_.SendPacket(session, writer.Finalize());
}

void GameServer::SendLoginResult(std::shared_ptr<Session> session, int32_t resultCode, const std::string& message,
                                  int64_t userId, const std::string& nickname) {
    proto::PacketWriter writer(proto::PacketType::S2C_LOGIN_RESULT);
    writer.WriteInt32(resultCode);
    writer.WriteString(message);
    writer.WriteUInt64(static_cast<uint64_t>(userId));
    writer.WriteString(nickname);
    netServer_.SendPacket(session, writer.Finalize());
}

// ================================================================
// 하트비트 타임아웃 감시: 일정 시간 응답 없는 세션을 강제 종료
// ================================================================
void GameServer::HeartbeatMonitorLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        auto sessions = sessionManager_.SnapshotAll();
        for (auto& s : sessions) {
            if (s->SecondsSinceLastRecv() > HEARTBEAT_TIMEOUT_SEC) {
                proto::PacketWriter writer(proto::PacketType::S2C_FORCE_DISCONNECT);
                netServer_.SendPacket(s, writer.Finalize());
                // 실제 소켓 종료는 해당 세션이 속한 IoWorker가 다음 이벤트 루프에서
                // 처리하도록 별도 강제종료 큐를 둘 수도 있음. 여기서는 클라이언트가
                // 통지를 받고 스스로 연결을 끊는 것을 기대하는 형태로 단순화.
                fprintf(stderr, "[Heartbeat] 세션 %llu 타임아웃 감지\n",
                        (unsigned long long)s->GetSessionId());
            }
        }
    }
}
