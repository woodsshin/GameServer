-- ============================================================
-- 게임 서버 DB 스키마
-- ============================================================
CREATE DATABASE IF NOT EXISTS game_server
    CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;

USE game_server;

CREATE TABLE IF NOT EXISTS users (
    id              BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    username        VARCHAR(32)  NOT NULL,
    password_hash   CHAR(64)     NOT NULL,   -- SHA-256 hex
    salt            CHAR(32)     NOT NULL,   -- 16 byte hex
    nickname        VARCHAR(32)  NOT NULL,
    created_at      DATETIME     NOT NULL,
    last_login_at   DATETIME     NULL,

    UNIQUE KEY uk_username (username),
    KEY idx_nickname (nickname)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS chat_logs (
    id          BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
    user_id     BIGINT UNSIGNED NOT NULL,
    message     VARCHAR(500) NOT NULL,
    created_at  DATETIME NOT NULL,

    KEY idx_user_id (user_id),
    KEY idx_created_at (created_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
