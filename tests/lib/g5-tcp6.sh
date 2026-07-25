#!/bin/bash
# g5-tcp6.sh — G5: TCP/IPv6 TDD 红灯测试 (预期失败，放最后)

run_g5_tcp6() {
    group "G5: TCP/IPv6 (TDD 红灯 — 预期失败)"

    local ipv6_host_ll="fe80::1"
    local ipv6_stack_ll=""

    if [ -n "$STACK_LL" ]; then
        ipv6_stack_ll="$STACK_LL"
    fi

    _test_tcp6_client_connect "$ipv6_host_ll"
    _test_tcp6_client_getpeername "$ipv6_host_ll"
    _test_tcp6_server_accept "$ipv6_stack_ll"
    _test_tcp6_echo_integrity "$ipv6_stack_ll"
}

_test_tcp6_client_connect() {
    local host_ll="$1"
    local output

    python3 -m http.server 8080 --bind "$host_ll" >/dev/null 2>&1 &
    HTTP_PIDS="$HTTP_PIDS $!"
    sleep 2

    output=$(timeout 10 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl-v6/curl-v6" \
             "$host_ll" 8080 2>&1) || true

    for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
    HTTP_PIDS=""

    if echo "$output" | grep -q "HTTP/"; then
        pass "TCP/IPv6 client connect"
    else
        fail "TCP/IPv6 client connect"
    fi
}

_test_tcp6_client_getpeername() {
    local host_ll="$1"
    local output

    nc -6 -l "$host_ll" 8081 >/dev/null 2>&1 &
    NC_PIDS="$NC_PIDS $!"
    sleep 1

    output=$(timeout 10 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl-v6/curl-v6" \
             "$host_ll" 8081 2>&1) || true

    for p in $NC_PIDS; do kill "$p" 2>/dev/null || true; done
    NC_PIDS=""

    if echo "$output" | grep -q "AF_INET6"; then
        pass "TCP/IPv6 client getpeername"
    else
        fail "TCP/IPv6 client getpeername"
    fi
}

_test_tcp6_server_accept() {
    local stack_ll="$1"
    local log nc_output server_output

    if [ -z "$stack_ll" ]; then
        skip "TCP/IPv6 server accept (无栈地址)"
        return
    fi

    log=$(mktemp /tmp/tcp-v6-server.XXXXXX.log)

    "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/tcp-v6-server/server" 9090 \
        > "$log" 2>&1 &
    SERVER_PID=$!

    if wait_server_ready "$log"; then
        nc_output=$(echo "hello-ipv6-test" | timeout 5 nc -6 "$stack_ll" 9090 2>&1) || true
        sleep 2
        server_output=$(cat "$log" 2>/dev/null)

        if echo "$nc_output" | grep -q "hello-ipv6-test" \
           && echo "$server_output" | grep -q "AF_INET6"; then
            pass "TCP/IPv6 server accept"
        else
            fail "TCP/IPv6 server accept"
        fi
    else
        fail "TCP/IPv6 server accept (server 未启动)"
    fi

    kill "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
    rm -f "$log" 2>/dev/null
}

_test_tcp6_echo_integrity() {
    local stack_ll="$1"
    local log send_file recv_file

    if [ -z "$stack_ll" ]; then
        skip "TCP/IPv6 echo 数据完整性 (无栈地址)"
        return
    fi

    log=$(mktemp /tmp/tcp-v6-server2.XXXXXX.log)
    send_file=$(mktemp /tmp/tcp-v6-send.XXXXXX)
    recv_file=$(mktemp /tmp/tcp-v6-recv.XXXXXX)

    dd if=/dev/urandom bs=1024 count=1 of="$send_file" 2>/dev/null

    "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/tcp-v6-server/server" 9091 \
        > "$log" 2>&1 &
    SERVER_PID=$!

    if wait_server_ready "$log"; then
        timeout 10 nc -6 "$stack_ll" 9091 < "$send_file" > "$recv_file" 2>/dev/null || true
        sleep 2

        local send_md5 recv_md5
        send_md5=$(md5sum "$send_file" | awk '{print $1}')
        recv_md5=$(md5sum "$recv_file" | awk '{print $1}')

        if [ "$send_md5" = "$recv_md5" ] && [ -n "$send_md5" ]; then
            pass "TCP/IPv6 echo 数据完整性"
        else
            fail "TCP/IPv6 echo 数据完整性"
        fi
    else
        fail "TCP/IPv6 echo 数据完整性 (server 未启动)"
    fi

    kill "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
    rm -f "$log" "$send_file" "$recv_file" 2>/dev/null
}
