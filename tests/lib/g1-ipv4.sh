#!/bin/bash
# g1-ipv4.sh — G1: IPv4 回归测试 (ping, ARP)

run_g1_ipv4() {
    group "G1: IPv4 回归"

    if ping -c 2 -W 2 -I "$TAP_DEV" 10.0.0.4 2>&1 | grep -q "0% packet loss"; then
        pass "IPv4 ping"
    else
        fail "IPv4 ping"
    fi

    ping -c 1 -W 2 -I "$TAP_DEV" 10.0.0.4 >/dev/null 2>&1 || true
    sleep 1
    if ip neigh show dev "$TAP_DEV" 2>/dev/null | grep -q "10.0.0.4"; then
        pass "ARP 缓存"
    else
        fail "ARP 缓存"
    fi
}
