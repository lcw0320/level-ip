#!/bin/bash
# g2-transport.sh — G2: 传输层测试 (TCP/IPv4 + UDP/IPv4)

run_g2_transport() {
    group "G2: TCP/IPv4"

    # ── 启动 HTTP server ────────────────────────────────
    python3 -m http.server 8001 --directory "$SCRIPT_DIR" >/dev/null 2>&1 &
    HTTP_PIDS="$HTTP_PIDS $!"
    sleep 2

    # Test: curl sync
    local resp
    resp=$(timeout 15 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" 10.0.0.5 8001 2>/dev/null \
        | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
    if [ -n "$resp" ] && diff -q "$FIXTURE" <(printf '%s\n' "$resp") >/dev/null 2>&1; then
        pass "curl sync HTTP GET"
    else
        fail "curl sync HTTP GET"
    fi

    # Test: curl poll
    resp=$(timeout 15 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl-poll/curl-poll" 10.0.0.5 8001 2>/dev/null \
        | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
    if [ -n "$resp" ] && diff -q "$FIXTURE" <(printf '%s\n' "$resp") >/dev/null 2>&1; then
        pass "curl poll HTTP GET"
    else
        fail "curl poll HTTP GET"
    fi

    # Test: connection refused
    if timeout 15 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" 10.0.0.5 9999 2>&1 | grep -q "Connection refused"; then
        pass "TCP connection refused"
    else
        fail "TCP connection refused"
    fi

    # Test: TCP server echo (stack 做服务端 — bind/listen/accept)
    _test_tcp_server_echo

    # 清理 HTTP server
    for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
    HTTP_PIDS=""

    # ── UDP 测试 ────────────────────────────────────────
    group "G2: UDP/IPv4"
    _test_udp_echo
}

_test_tcp_server_echo() {
    local log response

    log=$(mktemp /tmp/tcp-server.XXXXXX.log)

    "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/tcp/tcp_server" 10.0.0.4 8085 \
        > "$log" 2>&1 &
    SERVER_PID=$!
    sleep 3

    if kill -0 "$SERVER_PID" 2>/dev/null; then
        response=$(echo "hello-tcp-echo-test" | timeout 5 nc -q 1 10.0.0.4 8085 2>/dev/null) || true
        sleep 1

        if echo "$response" | grep -q "hello-tcp-echo-test"; then
            pass "TCP server echo"
        else
            fail "TCP server echo"
        fi
    else
        fail "TCP server echo (server 未启动)"
    fi

    kill "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
    rm -f "$log" 2>/dev/null
}

_test_udp_echo() {
    local log

    log=$(mktemp /tmp/udp-server.XXXXXX.log)

    "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/udp/udp_server" 10.0.0.4 9999 \
        > "$log" 2>&1 &
    SERVER_PID=$!
    sleep 3

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        fail "UDP server echo (server 未启动)"
        rm -f "$log" 2>/dev/null
        SERVER_PID=""
        return
    fi

    # 用 python 做可靠的 UDP echo 测试
    local result
    result=$(python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.settimeout(3)
s.sendto(b'hello-udp-echo-test', ('10.0.0.4', 9999))
try:
    data, addr = s.recvfrom(1024)
    print(data.decode())
except Exception as e:
    print(f'ERROR:{e}')
s.close()
" 2>/dev/null) || true

    if echo "$result" | grep -q "hello-udp-echo-test"; then
        pass "UDP server echo"
    else
        fail "UDP server echo"
    fi

    kill "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
    rm -f "$log" 2>/dev/null
}
