#include "GameServer.h"
#include "PasswordHash.h"
#include <cstdio>
#include <chrono>

namespace {
// PacketWriter::Finalize()가 반환하는 vector<char>를 shared_ptr로 감싸는 헬퍼.
// 브로드캐스트 대상이 여러 명이어도 이 shared_ptr 하나를 세션들이 공유하므로
// 세션 수만큼 vector를 복사하지 않는다 (NetServer::BroadcastPacket 참고).
PacketBuffer MakeBuffer(std::vector<char> data) {
    return std::make_shared<const std::vector<char>>(std::move(data));
}
}  // namespace

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
//
//   DB 재연결: Task는 MYSQL*가 아니라 ScopedConnection&을 받는다. 쿼리 실행 후
//   연결이 끊어져서 실패한 것인지(IsConnectionLostError)를 확인해 MarkBroken()으로
//   표시하면, DBWorkerPool이 커넥션을 재연결한 뒤 이 Task를 자동으로 1회 재시도한다.
//   재시도 시 이 람다가 처음부터 다시 통째로 실행되므로, "중복 아이디 체크 -> INSERT"
//   흐름 전체가 재연결된 새 커넥션으로 다시 수행된다.
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
    task.execute = [this, session, username, password, nickname](DBConnectionPool::ScopedConnection& connGuard) {
        MYSQL* conn = connGuard.Get();

        // 1) 중복 아이디 체크
        std::string checkQuery = "SELECT id FROM users WHERE username = ?";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(stmt, checkQuery.c_str(), checkQuery.size());

        MYSQL_BIND bindParam[1]{};
        bindParam[0].buffer_type = MYSQL_TYPE_STRING;
        bindParam[0].buffer = (void*)username.c_str();
        bindParam[0].buffer_length = username.size();
        mysql_stmt_bind_param(stmt, bindParam);

        if (mysql_stmt_execute(stmt) != 0) {
            bool lost = IsConnectionLostError(conn);
            mysql_stmt_close(stmt);
            if (lost) { connGuard.MarkBroken(); return; } // 재시도
            SendRegisterResult(session, -4, "서버 내부 오류 (DB)");
            return;
        }
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
            bool lost = IsConnectionLostError(conn);
            fprintf(stderr, "[Register] INSERT 실패: %s\n", mysql_stmt_error(insertStmt));
            mysql_stmt_close(insertStmt);
            if (lost) { connGuard.MarkBroken(); return; } // 재시도
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
    task.execute = [this, session, username, password](DBConnectionPool::ScopedConnection& connGuard) {
        MYSQL* conn = connGuard.Get();

        std::string query =
            "SELECT id, password_hash, salt, nickname FROM users WHERE username = ? LIMIT 1";
        MYSQL_STMT* stmt = mysql_stmt_init(conn);
        mysql_stmt_prepare(stmt, query.c_str(), query.size());

        MYSQL_BIND paramBind[1]{};
        paramBind[0].buffer_type = MYSQL_TYPE_STRING;
        paramBind[0].buffer = (void*)username.c_str();
        paramBind[0].buffer_length = username.size();
        mysql_stmt_bind_param(stmt, paramBind);

        if (mysql_stmt_execute(stmt) != 0) {
            bool lost = IsConnectionLostError(conn);
            mysql_stmt_close(stmt);
            if (lost) { connGuard.MarkBroken(); return; } // 재시도
            SendLoginResult(session, -4, "서버 내부 오류 (DB)");
            return;
        }

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

        // 마지막 로그인 시간 갱신 (이 갱신 자체가 연결 유실로 실패해도 로그인은 이미
        // 성공 처리되었으므로 재시도하지 않고 그냥 흘려보낸다 - 부가 정보이기 때문)
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
    PacketBuffer packet = MakeBuffer(writer.Finalize());

    // 브로드캐스트는 네트워크 I/O이므로 DB 스레드가 아니라 이 IoWorker 스레드에서 바로 처리.
    // (SendPacket이 내부적으로 대상 세션이 속한 worker에 안전하게 큐잉하므로 스레드 경계를 넘어도 안전)
    // packet은 shared_ptr이므로 N명에게 브로드캐스트해도 바이트 복사는 발생하지 않는다.
    netServer_.BroadcastPacket(sessionManager_, packet);

    // 채팅 로그를 DB에 비동기로 적재 (분석/신고 대응용, 실패해도 서비스에 영향 없도록 fire-and-forget).
    // 연결 유실 시에도 재시도하되, 이마저 실패하면 로그 한 건 유실을 감수하고 넘어간다
    // (채팅 자체는 이미 브로드캐스트되었으므로 유저 체감에는 영향 없음).
    int64_t userId = session->GetUserId();
    DBTask logTask;
    logTask.execute = [userId, message](DBConnectionPool::ScopedConnection& connGuard) {
        MYSQL* conn = connGuard.Get();
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
        if (mysql_stmt_execute(stmt) != 0 && IsConnectionLostError(conn)) {
            mysql_stmt_close(stmt);
            connGuard.MarkBroken(); // 재시도
            return;
        }
        mysql_stmt_close(stmt);
    };
    dbWorkerPool_.Enqueue(std::move(logTask));
}

void GameServer::HandleHeartbeat(std::shared_ptr<Session> session) {
    session->TouchRecvTime();
    proto::PacketWriter writer(proto::PacketType::S2C_HEARTBEAT_ACK);
    netServer_.SendPacket(session, MakeBuffer(writer.Finalize()));
}

// ================================================================
// 응답 헬퍼
// ================================================================
void GameServer::SendRegisterResult(std::shared_ptr<Session> session, int32_t resultCode, const std::string& message) {
    proto::PacketWriter writer(proto::PacketType::S2C_REGISTER_RESULT);
    writer.WriteInt32(resultCode);
    writer.WriteString(message);
    netServer_.SendPacket(session, MakeBuffer(writer.Finalize()));
}

void GameServer::SendLoginResult(std::shared_ptr<Session> session, int32_t resultCode, const std::string& message,
                                  int64_t userId, const std::string& nickname) {
    proto::PacketWriter writer(proto::PacketType::S2C_LOGIN_RESULT);
    writer.WriteInt32(resultCode);
    writer.WriteString(message);
    writer.WriteUInt64(static_cast<uint64_t>(userId));
    writer.WriteString(nickname);
    netServer_.SendPacket(session, MakeBuffer(writer.Finalize()));
}

// ================================================================
// 하트비트 타임아웃 감시: 일정 시간 응답 없는 세션을 서버가 직접 강제 종료
//   기존에는 S2C_FORCE_DISCONNECT 통지만 보내고 클라이언트의 자발적 종료에
//   의존했으나, 이제는 통지 전송과 함께 NetServer::ForceDisconnect를 호출해
//   해당 세션이 속한 IoWorker의 강제종료 큐에 fd를 넣는다. 그 IoWorker가
//   다음 wakeup 시점에 실제로 close()하므로, 응답 없는(혹은 악의적으로 통지를
//   무시하는) 클라이언트도 서버 주도로 확실히 정리된다.
// ================================================================
void GameServer::HeartbeatMonitorLoop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        auto sessions = sessionManager_.SnapshotAll();
        for (auto& s : sessions) {
            if (s->SecondsSinceLastRecv() > HEARTBEAT_TIMEOUT_SEC) {
                proto::PacketWriter writer(proto::PacketType::S2C_FORCE_DISCONNECT);
                netServer_.SendPacket(s, MakeBuffer(writer.Finalize()));

                fprintf(stderr, "[Heartbeat] 세션 %llu 타임아웃 감지, 서버 주도로 강제 종료합니다\n",
                        (unsigned long long)s->GetSessionId());
                netServer_.ForceDisconnect(s);
            }
        }
    }
}
