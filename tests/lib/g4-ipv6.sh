#!/bin/bash
# g4-ipv6.sh — G4: IPv6 网络层测试 + 稳定性检查

run_g4_ipv6() {
    group "G4: IPv6 网络层"

    _test_ping6_link_local
    _test_ping6_global
    _test_ndp
    _test_dad
    _test_default_route
    _test_icmpv6_checksum
    _test_rs
}

_test_ping6_link_local() {
    if [ -n "$STACK_LL" ]; then
        if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_LL" 2>&1 | grep -q "0% packet loss"; then
            pass "ping6 链路本地"
        else
            fail "ping6 链路本地"
        fi
    else
        skip "ping6 链路本地 (无地址)"
    fi
}

_test_ping6_global() {
    if [ -n "$STACK_GLOBAL" ]; then
        if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_GLOBAL" 2>&1 | grep -q "0% packet loss"; then
            pass "ping6 全局地址"
        else
            fail "ping6 全局地址"
        fi
    else
        skip "ping6 全局地址 (无地址)"
    fi
}

_test_ndp() {
    local ndp_state
    sleep 1
    ndp_state=$(ip -6 neigh show dev "$TAP_DEV" 2>/dev/null \
        | grep -v "fe80::1\b" | grep "fe80" | head -1 | awk '{print $NF}')
    case "$ndp_state" in
        REACHABLE|STALE|DELAY|PROBE) pass "NDP 状态: $ndp_state" ;;
        "")      fail "NDP 邻居缓存为空" ;;
        *)       skip "NDP 状态: $ndp_state" ;;
    esac
}

_test_dad() {
    if grep -q "DAD passed" "$LOG"; then
        local count
        count=$(grep -c "DAD passed" "$LOG")
        pass "DAD 通过 ($count 个地址)"
    else
        fail "DAD 未通过"
    fi
}

_test_default_route() {
    if grep -q "added default route" "$LOG"; then
        pass "默认路由"
    else
        fail "默认路由"
    fi
}

_test_icmpv6_checksum() {
    local csum_errors
    csum_errors=$(grep "checksum mismatch" "$LOG" 2>/dev/null | grep -v "0xf7ff" | wc -l)
    if [ "$csum_errors" -gt 0 ]; then
        fail "ICMPv6 校验和错误 ${csum_errors} 次"
    else
        pass "ICMPv6 校验和"
    fi
}

_test_rs() {
    if grep -q "send RS" "$LOG"; then
        pass "RS 发送"
    else
        skip "RS 发送"
    fi
}

# ── 稳定性检查 (在 G5 之前，G5 红灯测试可能崩溃 daemon) ─
run_stability_check() {
    group "稳定性检查"

    if kill -0 "$LVL_PID" 2>/dev/null; then
        pass "协议栈仍然运行"
    else
        fail "协议栈已崩溃"
    fi

    if grep -q "ThreadSanitizer" "$LOG" 2>/dev/null; then
        fail "ThreadSanitizer 错误"
    else
        pass "无线程错误"
    fi

    if grep -qE "double free|corruption|munmap_chunk|buffer overflow" "$LOG" 2>/dev/null; then
        fail "内存错误"
    else
        pass "无内存错误"
    fi
}
