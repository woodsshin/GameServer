#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
game_server 부하 테스트 스크립트
- 수백~수천 개의 동시 TCP 연결을 asyncio로 생성
- 각 가상 클라이언트: 접속 -> 회원가입 -> 로그인 -> 채팅 N회 전송 -> 하트비트 -> 종료
- 서버가 보내는 실제 응답(패킷)을 파싱해서 성공/실패를 판정 (단순 접속 성공이 아니라
  "회원가입 성공", "로그인 성공", "채팅 브로드캐스트 수신"까지 검증)
- 결과: 연결 성공률, 각 단계별 RTT(p50/p95/p99/max), 초당 처리 채팅 메시지 수,
  전체 소요 시간, 에러 유형별 집계를 JSON + 표로 출력

사용법:
  python3 load_test.py --host 127.0.0.1 --port 9000 --clients 1000 --ramp-seconds 10 --chat-per-client 3

주의:
  --clients 값을 늘릴수록 이 스크립트를 실행하는 머신의 ephemeral port / fd 제한에도 영향을 받음.
  리눅스에서 대규모 테스트 시 `ulimit -n 65535` 등으로 fd 제한을 늘려야 할 수 있음.
"""
import argparse
import asyncio
import json
import struct
import time
import statistics
import sys
from dataclasses import dataclass, field

# ---- 프로토콜 정의 (PacketDef.h 와 동일) ----
HEADER_FMT = "<IH"  # uint32 totalSize, uint16 type
HEADER_SIZE = struct.calcsize(HEADER_FMT)

C2S_REGISTER = 1001
S2C_REGISTER_RESULT = 1002
C2S_LOGIN = 1003
S2C_LOGIN_RESULT = 1004
C2S_CHAT_MESSAGE = 2001
S2C_CHAT_BROADCAST = 2002
C2S_HEARTBEAT = 3001
S2C_HEARTBEAT_ACK = 3002
S2C_FORCE_DISCONNECT = 3003


def write_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("<H", len(b)) + b


def build_packet(ptype: int, body: bytes) -> bytes:
    total_size = HEADER_SIZE + len(body)
    return struct.pack(HEADER_FMT, total_size, ptype) + body


class PacketReader:
    def __init__(self, buf: bytes):
        self.buf = buf
        self.off = 0

    def read_i32(self) -> int:
        v = struct.unpack_from("<i", self.buf, self.off)[0]
        self.off += 4
        return v

    def read_u64(self) -> int:
        v = struct.unpack_from("<Q", self.buf, self.off)[0]
        self.off += 8
        return v

    def read_string(self) -> str:
        ln = struct.unpack_from("<H", self.buf, self.off)[0]
        self.off += 2
        s = self.buf[self.off:self.off + ln].decode("utf-8", errors="replace")
        self.off += ln
        return s


@dataclass
class ClientResult:
    client_id: int
    connect_ok: bool = False
    connect_ms: float = None
    register_ok: bool = False
    register_ms: float = None
    login_ok: bool = False
    login_ms: float = None
    chat_sent: int = 0
    chat_acked: int = 0
    chat_rtts_ms: list = field(default_factory=list)
    heartbeat_ok: bool = False
    error: str = None


async def read_exact(reader: asyncio.StreamReader, n: int) -> bytes:
    return await reader.readexactly(n)


async def read_one_packet(reader: asyncio.StreamReader, timeout: float):
    header = await asyncio.wait_for(read_exact(reader, HEADER_SIZE), timeout=timeout)
    total_size, ptype = struct.unpack(HEADER_FMT, header)
    body_size = total_size - HEADER_SIZE
    body = await asyncio.wait_for(read_exact(reader, body_size), timeout=timeout) if body_size > 0 else b""
    return ptype, body


async def read_until_type(reader: asyncio.StreamReader, expected_types, timeout: float,
                           deadline_from: float, max_skip: int = 8):
    """expected_types 중 하나가 나올 때까지 패킷을 계속 읽는다.
    지연으로 인해 이전 요청의 응답이 늦게 도착하는 경우는 정상적인 파이프라이닝 상황이므로
    (서버가 요청 순서대로 응답한다는 전제 하에) 계속 읽어서 원하는 타입을 찾는다.
    remaining timeout은 최초 호출 시점 기준 절대 데드라인으로 계산한다."""
    skipped = 0
    while True:
        remaining = timeout - (time.perf_counter() - deadline_from)
        if remaining <= 0:
            raise asyncio.TimeoutError()
        ptype, body = await read_one_packet(reader, remaining)
        if ptype in expected_types:
            return ptype, body
        skipped += 1
        if skipped >= max_skip:
            return ptype, body  # 더 이상 스킵하지 않고 있는 그대로 반환 (진짜 프로토콜 이상 케이스)


async def run_client(client_id: int, host: str, port: int, chat_per_client: int,
                      username_prefix: str, timeout: float, chat_interval: float) -> ClientResult:
    res = ClientResult(client_id=client_id)
    username = f"{username_prefix}{client_id}"
    password = "loadtest1234"
    nickname = f"lt{client_id}"

    t0 = time.perf_counter()
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=timeout
        )
    except Exception as e:
        res.error = f"connect_failed: {e!r}"
        return res

    res.connect_ok = True
    res.connect_ms = (time.perf_counter() - t0) * 1000

    try:
        # ---- 1) 회원가입 ----
        body = write_string(username) + write_string(password) + write_string(nickname)
        pkt = build_packet(C2S_REGISTER, body)
        t1 = time.perf_counter()
        writer.write(pkt)
        await writer.drain()

        ptype, rbody = await read_until_type(reader, {S2C_REGISTER_RESULT}, timeout, t1)
        res.register_ms = (time.perf_counter() - t1) * 1000
        if ptype == S2C_REGISTER_RESULT:
            r = PacketReader(rbody)
            code = r.read_i32()
            _msg = r.read_string()
            res.register_ok = (code == 0)
        else:
            res.error = f"unexpected_packet_after_register:{ptype}"

        # ---- 2) 로그인 ----
        body = write_string(username) + write_string(password)
        pkt = build_packet(C2S_LOGIN, body)
        t2 = time.perf_counter()
        writer.write(pkt)
        await writer.drain()

        ptype, rbody = await read_until_type(reader, {S2C_LOGIN_RESULT}, timeout, t2)
        res.login_ms = (time.perf_counter() - t2) * 1000
        if ptype == S2C_LOGIN_RESULT:
            r = PacketReader(rbody)
            code = r.read_i32()
            _msg = r.read_string()
            res.login_ok = (code == 0)
        else:
            res.error = f"unexpected_packet_after_login:{ptype}"

        # ---- 3) 채팅 N회 (에코 형태의 브로드캐스트 자기 자신 포함 수신 검증) ----
        for i in range(chat_per_client):
            msg = f"load-test message {i} from {username}"
            pkt = build_packet(C2S_CHAT_MESSAGE, write_string(msg))
            t3 = time.perf_counter()
            writer.write(pkt)
            await writer.drain()
            res.chat_sent += 1
            try:
                ptype, rbody = await read_until_type(reader, {S2C_CHAT_BROADCAST}, timeout, t3)
                if ptype == S2C_CHAT_BROADCAST:
                    res.chat_acked += 1
                    res.chat_rtts_ms.append((time.perf_counter() - t3) * 1000)
            except asyncio.TimeoutError:
                pass
            if chat_interval > 0:
                await asyncio.sleep(chat_interval)

        # ---- 4) 하트비트 1회 ----
        pkt = build_packet(C2S_HEARTBEAT, b"")
        writer.write(pkt)
        await writer.drain()
        try:
            ptype, _ = await read_one_packet(reader, timeout)
            res.heartbeat_ok = (ptype == S2C_HEARTBEAT_ACK)
        except asyncio.TimeoutError:
            pass

    except Exception as e:
        if not res.error:
            res.error = f"exception: {e!r}"
    finally:
        try:
            writer.close()
        except Exception:
            pass

    return res


def percentile(data, pct):
    if not data:
        return None
    s = sorted(data)
    k = (len(s) - 1) * (pct / 100)
    f = int(k)
    c = min(f + 1, len(s) - 1)
    if f == c:
        return s[f]
    return s[f] + (s[c] - s[f]) * (k - f)


async def main_async(args):
    sem = asyncio.Semaphore(args.max_inflight)
    results = []

    async def bound_run(cid, delay):
        if delay > 0:
            await asyncio.sleep(delay)
        async with sem:
            r = await run_client(
                cid, args.host, args.port, args.chat_per_client,
                args.username_prefix, args.timeout, args.chat_interval
            )
            results.append(r)
            return r

    ramp = args.ramp_seconds
    tasks = []
    wall_start = time.perf_counter()
    for cid in range(args.clients):
        delay = (ramp * cid / args.clients) if ramp > 0 else 0
        tasks.append(asyncio.create_task(bound_run(cid, delay)))

    total = len(tasks)
    done_count = 0
    for coro in asyncio.as_completed(tasks):
        await coro
        done_count += 1
        if args.progress and (done_count % max(1, total // 20) == 0 or done_count == total):
            elapsed = time.perf_counter() - wall_start
            print(f"  진행: {done_count}/{total} ({elapsed:.1f}s 경과)", file=sys.stderr)

    wall_elapsed = time.perf_counter() - wall_start
    return results, wall_elapsed


def summarize(results, wall_elapsed, args):
    n = len(results)
    connect_ok = [r for r in results if r.connect_ok]
    register_ok = [r for r in results if r.register_ok]
    login_ok = [r for r in results if r.login_ok]
    heartbeat_ok = [r for r in results if r.heartbeat_ok]

    total_chat_sent = sum(r.chat_sent for r in results)
    total_chat_acked = sum(r.chat_acked for r in results)
    all_chat_rtts = [x for r in results for x in r.chat_rtts_ms]

    connect_ms = [r.connect_ms for r in results if r.connect_ms is not None]
    register_ms = [r.register_ms for r in register_ok if r.register_ms is not None]
    login_ms = [r.login_ms for r in login_ok if r.login_ms is not None]

    errors = {}
    for r in results:
        if r.error:
            key = r.error.split(":")[0].split("(")[0].strip()
            errors[key] = errors.get(key, 0) + 1

    def stat_block(name, data):
        if not data:
            return {"name": name, "count": 0}
        return {
            "name": name,
            "count": len(data),
            "min_ms": round(min(data), 2),
            "p50_ms": round(percentile(data, 50), 2),
            "p95_ms": round(percentile(data, 95), 2),
            "p99_ms": round(percentile(data, 99), 2),
            "max_ms": round(max(data), 2),
            "avg_ms": round(statistics.mean(data), 2),
        }

    summary = {
        "config": {
            "host": args.host,
            "port": args.port,
            "clients": args.clients,
            "ramp_seconds": args.ramp_seconds,
            "max_inflight": args.max_inflight,
            "chat_per_client": args.chat_per_client,
        },
        "wall_elapsed_sec": round(wall_elapsed, 2),
        "clients_total": n,
        "connect_success": len(connect_ok),
        "connect_success_rate": round(len(connect_ok) / n * 100, 2) if n else 0,
        "register_success": len(register_ok),
        "register_success_rate": round(len(register_ok) / n * 100, 2) if n else 0,
        "login_success": len(login_ok),
        "login_success_rate": round(len(login_ok) / n * 100, 2) if n else 0,
        "heartbeat_success": len(heartbeat_ok),
        "chat_messages_sent": total_chat_sent,
        "chat_messages_acked": total_chat_acked,
        "chat_ack_rate": round(total_chat_acked / total_chat_sent * 100, 2) if total_chat_sent else 0,
        "chat_throughput_msg_per_sec": round(total_chat_acked / wall_elapsed, 2) if wall_elapsed > 0 else 0,
        "connections_per_sec": round(len(connect_ok) / wall_elapsed, 2) if wall_elapsed > 0 else 0,
        "latency": {
            "connect": stat_block("connect", connect_ms),
            "register": stat_block("register", register_ms),
            "login": stat_block("login", login_ms),
            "chat_roundtrip": stat_block("chat_roundtrip", all_chat_rtts),
        },
        "errors": errors,
    }
    return summary


def print_report(summary):
    c = summary["config"]
    print("=" * 70)
    print(f"부하 테스트 결과  (host={c['host']}:{c['port']}, clients={c['clients']}, "
          f"ramp={c['ramp_seconds']}s, chat/client={c['chat_per_client']})")
    print("=" * 70)
    print(f"총 소요 시간          : {summary['wall_elapsed_sec']} s")
    print(f"접속 성공             : {summary['connect_success']}/{summary['clients_total']} "
          f"({summary['connect_success_rate']}%)")
    print(f"회원가입 성공         : {summary['register_success']}/{summary['clients_total']} "
          f"({summary['register_success_rate']}%)")
    print(f"로그인 성공           : {summary['login_success']}/{summary['clients_total']} "
          f"({summary['login_success_rate']}%)")
    print(f"하트비트 응답 성공    : {summary['heartbeat_success']}/{summary['clients_total']}")
    print(f"채팅 전송/응답 확인   : {summary['chat_messages_sent']} / {summary['chat_messages_acked']} "
          f"({summary['chat_ack_rate']}%)")
    print(f"신규 연결 처리량      : {summary['connections_per_sec']} conn/sec")
    print(f"채팅 처리량           : {summary['chat_throughput_msg_per_sec']} msg/sec")
    print("-" * 70)
    for key in ("connect", "register", "login", "chat_roundtrip"):
        s = summary["latency"][key]
        if s["count"] == 0:
            print(f"{key:15s}: 샘플 없음")
            continue
        print(f"{key:15s}: n={s['count']:6d}  p50={s['p50_ms']:8.2f}ms  p95={s['p95_ms']:8.2f}ms  "
              f"p99={s['p99_ms']:8.2f}ms  max={s['max_ms']:8.2f}ms  avg={s['avg_ms']:8.2f}ms")
    print("-" * 70)
    if summary["errors"]:
        print("에러 유형 집계:")
        for k, v in sorted(summary["errors"].items(), key=lambda x: -x[1]):
            print(f"  - {k}: {v}건")
    else:
        print("에러 없음")
    print("=" * 70)


def parse_args():
    p = argparse.ArgumentParser(description="game_server 부하 테스트")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=9000)
    p.add_argument("--clients", type=int, default=500, help="동시 시뮬레이션할 가상 클라이언트 수")
    p.add_argument("--ramp-seconds", type=float, default=5.0,
                    help="전체 클라이언트를 몇 초에 걸쳐 순차 접속시킬지 (0이면 즉시 전부 시도)")
    p.add_argument("--max-inflight", type=int, default=2000,
                    help="동시에 진행 중일 수 있는 최대 클라이언트 태스크 수 (세마포어)")
    p.add_argument("--chat-per-client", type=int, default=3)
    p.add_argument("--chat-interval", type=float, default=0.05,
                    help="한 클라이언트가 채팅을 연달아 보낼 때 각 전송 사이 대기(초)")
    p.add_argument("--timeout", type=float, default=10.0, help="개별 요청 응답 대기 타임아웃(초)")
    p.add_argument("--username-prefix", default="lt_user_")
    p.add_argument("--out", default=None, help="JSON 결과 저장 경로")
    p.add_argument("--progress", action="store_true", help="진행 상황을 stderr로 출력")
    return p.parse_args()


def main():
    args = parse_args()
    print(f"[load_test] {args.clients}개 클라이언트로 {args.host}:{args.port} 부하 테스트 시작 "
          f"(ramp={args.ramp_seconds}s)", file=sys.stderr)
    results, wall_elapsed = asyncio.run(main_async(args))
    summary = summarize(results, wall_elapsed, args)
    print_report(summary)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(summary, f, ensure_ascii=False, indent=2)
        print(f"[load_test] JSON 결과 저장: {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
