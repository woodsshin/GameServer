#!/usr/bin/env bash
# ============================================================
# 전체 부하 테스트 러너
# - MySQL(MariaDB) 기동 확인
# - game_server 백그라운드 실행
# - 여러 동시접속 단계(예: 100 / 500 / 1000 / 2000)를 순차적으로 부하 테스트
# - 각 단계마다 서버 프로세스의 CPU/메모리 사용량을 함께 기록
# - 결과를 results/ 디렉토리에 JSON + 텍스트 로그로 저장
#
# 사용법:
#   ./run_full_load_test.sh [PORT] [DB_HOST] [DB_USER] [DB_PASSWORD] [DB_NAME]
# ============================================================
set -uo pipefail

PORT="${1:-9000}"
DB_HOST="${2:-127.0.0.1}"
DB_USER="${3:-gameapp}"
DB_PASSWORD="${4:-testpass123}"
DB_NAME="${5:-game_server}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
RESULTS_DIR="$PROJECT_DIR/results/${RESULT_SUBDIR:-default}"
SERVER_BIN="$BUILD_DIR/game_server"

mkdir -p "$RESULTS_DIR"

echo "=== [0/5] MySQL(MariaDB) 기동 및 스키마 준비 ==="
pkill -9 -f mariadbd 2>/dev/null
pkill -9 -f mysqld_safe 2>/dev/null
sleep 1
mkdir -p /run/mysqld && chown mysql:mysql /run/mysqld
mkdir -p /var/log/mysql
rm -f /run/mysqld/mysqld.pid /run/mysqld/mysqld.sock /var/lib/mysql/aria_log_control
mysqld_safe --user=mysql --log-error=/var/log/mysql/error.log > /var/log/mysql/startup.log 2>&1 &
for i in $(seq 1 20); do
    mysqladmin ping >/dev/null 2>&1 && break
    sleep 1
done
mysqladmin ping || { echo "MySQL 기동 실패"; exit 1; }

mysql -u root < "$PROJECT_DIR/sql/schema.sql"
mysql -u root -e "CREATE USER IF NOT EXISTS '$DB_USER'@'$DB_HOST' IDENTIFIED BY '$DB_PASSWORD'; GRANT ALL PRIVILEGES ON $DB_NAME.* TO '$DB_USER'@'$DB_HOST'; FLUSH PRIVILEGES;"
mysql -u root -e "TRUNCATE TABLE $DB_NAME.users; TRUNCATE TABLE $DB_NAME.chat_logs;"

echo "=== [1/5] MySQL 연결 확인 ==="
mysql -u "$DB_USER" -h "$DB_HOST" -p"$DB_PASSWORD" -e "SELECT 1;" "$DB_NAME" > /dev/null
if [ $? -ne 0 ]; then
    echo "DB 연결 실패. 스크립트를 중단합니다." >&2
    exit 1
fi
echo "DB 연결 OK ($DB_USER@$DB_HOST/$DB_NAME)"

echo "=== [2/5] 서버 기동 (IO_WORKER_COUNT=${IO_WORKER_COUNT:-auto}, DB_WORKER_COUNT=${DB_WORKER_COUNT:-4}) ==="
export DB_HOST DB_USER DB_PASSWORD DB_NAME
export IO_WORKER_COUNT DB_WORKER_COUNT
"$SERVER_BIN" "$PORT" > "$RESULTS_DIR/server_stdout.log" 2>&1 &
SERVER_PID=$!
echo "game_server PID=$SERVER_PID, port=$PORT"

sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "서버가 기동 직후 종료되었습니다. 로그 확인:" >&2
    cat "$RESULTS_DIR/server_stdout.log" >&2
    exit 1
fi

cleanup() {
    echo "=== 서버 프로세스 종료 (PID=$SERVER_PID) ==="
    kill -TERM "$SERVER_PID" 2>/dev/null
    sleep 1
    kill -KILL "$SERVER_PID" 2>/dev/null
}
trap cleanup EXIT

echo "=== [3/5] 리소스 모니터 시작 ==="
MONITOR_LOG="$RESULTS_DIR/resource_usage.csv"
echo "timestamp,cpu_percent,rss_kb,open_fds" > "$MONITOR_LOG"
(
    while kill -0 "$SERVER_PID" 2>/dev/null; do
        TS=$(date +%s.%N)
        if [ -r "/proc/$SERVER_PID/stat" ]; then
            RSS_KB=$(awk '{print $2 * 4}' "/proc/$SERVER_PID/statm" 2>/dev/null || echo 0)
            FDS=$(ls "/proc/$SERVER_PID/fd" 2>/dev/null | wc -l)
            CPU=$(ps -p "$SERVER_PID" -o %cpu= 2>/dev/null | tr -d ' ')
            echo "$TS,$CPU,$RSS_KB,$FDS" >> "$MONITOR_LOG"
        fi
        sleep 1
    done
) &
MONITOR_PID=$!

echo "=== [4/5] 단계별 부하 테스트 실행 ==="
STAGES=(${LOAD_TEST_STAGES:-100 500 1000 2000})
for N in "${STAGES[@]}"; do
    echo ""
    echo ">>> 동시 클라이언트 $N 명 테스트 시작 <<<"
    OUT_JSON="$RESULTS_DIR/load_test_${N}.json"
    python3 "$SCRIPT_DIR/load_test.py" \
        --host 127.0.0.1 --port "$PORT" \
        --clients "$N" \
        --ramp-seconds "$(python3 -c "print(max(2.0, $N/200))")" \
        --max-inflight "$(( N < 500 ? 500 : N ))" \
        --chat-per-client 3 \
        --chat-interval 0.02 \
        --timeout 15 \
        --username-prefix "lt_s${N}_" \
        --progress \
        --out "$OUT_JSON" \
        | tee "$RESULTS_DIR/load_test_${N}.txt"

    echo "--- 서버가 살아있는지 확인 ---"
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "!!! 서버가 다운되었습니다 (클라이언트 $N 단계 이후). 로그:" >&2
        tail -50 "$RESULTS_DIR/server_stdout.log" >&2
        break
    fi
    sleep 3
done

echo "=== [5/5] 리소스 모니터 종료 ==="
kill "$MONITOR_PID" 2>/dev/null

echo ""
echo "모든 결과는 $RESULTS_DIR 에 저장되었습니다."
ls -la "$RESULTS_DIR"
