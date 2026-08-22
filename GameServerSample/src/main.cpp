#include "GameServer.h"
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <thread>

static GameServer* g_server = nullptr;

void SignalHandler(int) {
    printf("\n[Main] 종료 신호 수신, 서버를 정리합니다...\n");
    if (g_server) {
        g_server->Stop();
    }
    std::exit(0);
}

int main(int argc, char** argv) {
    uint16_t port = 9000;
    int ioWorkerCount = std::thread::hardware_concurrency();
    if (ioWorkerCount == 0) ioWorkerCount = 4;
    int dbWorkerCount = 4;

    DBConfig dbConfig;
    dbConfig.host = std::getenv("DB_HOST") ? std::getenv("DB_HOST") : "127.0.0.1";
    dbConfig.user = std::getenv("DB_USER") ? std::getenv("DB_USER") : "root";
    dbConfig.password = std::getenv("DB_PASSWORD") ? std::getenv("DB_PASSWORD") : "";
    dbConfig.dbName = std::getenv("DB_NAME") ? std::getenv("DB_NAME") : "game_server";
    dbConfig.poolSize = 8;

    if (argc > 1) port = static_cast<uint16_t>(std::atoi(argv[1]));

    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    signal(SIGPIPE, SIG_IGN); // 클라이언트가 갑자기 끊어도 send()가 프로세스를 죽이지 않도록

    try {
        GameServer server(port, ioWorkerCount, dbConfig, dbWorkerCount);
        g_server = &server;

        printf("[Main] 서버 시작: port=%d, ioWorkers=%d, dbWorkers=%d\n", port, ioWorkerCount, dbWorkerCount);

        if (!server.Start()) {
            fprintf(stderr, "[Main] 서버 시작 실패\n");
            return 1;
        }

        // 메인 스레드는 대기 (accept/io/db 워커 스레드들이 실제 작업 수행)
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(60));
        }
    } catch (const std::exception& e) {
        fprintf(stderr, "[Main] 예외 발생: %s\n", e.what());
        return 1;
    }

    return 0;
}
