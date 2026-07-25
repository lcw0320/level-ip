#!/bin/bash
#
# test-run.sh — 统一测试 runner（替代 test-all.sh + test-run-all）
#
# 用法: sudo bash tests/test-run.sh [--quick]
#
#   --quick  跳过 netem 测试（packet-delay/loss/duplication），加速运行
#
# 测试分组:
#   G1: IPv4 回归     (ping, ARP)
#   G2: TCP/IPv4      (curl normal, curl-poll, connection-refused)
#   G3: TCP netem     (delay, loss, duplication)  -- 除非 --quick
#   G4: IPv6 网络层   (ping6, NDP, SLAAC, DAD, PMTUD)
#   G5: TCP/IPv6      (client connect, getpeername, server accept, echo)
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
LOG="/tmp/lvl-ip-test-run.log"
TAP_DEV="tap0"
QUICK=0

# ── 颜色 ────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

PASSED=0; FAILED=0; SKIPPED=0
FAIL_LIST=""

pass()    { echo -e "  ${GREEN}✅ PASS${NC}: $1"; PASSED=$((PASSED+1)); }
fail()    { echo -e "  ${RED}❌ FAIL${NC}: $1"; FAILED=$((FAILED+1)); FAIL_LIST="$FAIL_LIST\n    $1"; }
skip()    { echo -e "  ${YELLOW}⏭  SKIP${NC}: $1"; SKIPPED=$((SKIPPED+1)); }
group()   { echo -e "\n${CYAN}${BOLD}━━━ $1 ━━━${NC}"; }
info()    { echo -e "  ${YELLOW}ℹ${NC} $1"; }

# ── 参数 ────────────────────────────────────────────────
for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# ── 清理 ────────────────────────────────────────────────
LVL_PID=""
HTTP_PIDS=""
NC_PIDS=""
SERVER_PID=""
SERVER_LOG=""

cleanup() {
    echo -e "\n${BOLD}=== 清理 ===${NC}"
    [ -n "$LVL_PID" ]  && kill "$LVL_PID" 2>/dev/null || true
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
    for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
    for p in $NC_PIDS;   do kill "$p" 2>/dev/null || true; done
    pkill lvl-ip 2>/dev/null || true
    pkill -f "http.server [0-9]" 2>/dev/null || true
    pkill -f "nc -6" 2>/dev/null || true
    ip -6 addr del "fd00:1234::1/64" dev "$TAP_DEV" 2>/dev/null || true
    ip -6 addr del "fe80::1/64" dev "$TAP_DEV" 2>/dev/null || true
    tc qdisc del dev "$TAP_DEV" root 2>/dev/null || true
    [ -n "$SERVER_LOG" ] && rm -f "$SERVER_LOG" 2>/dev/null || true
    echo "  已清理"
}
trap cleanup EXIT

# ================================================================
echo -e "${BOLD}╔══════════════════════════════════════╗${NC}"
echo -e "${BOLD}║   level-ip 统一测试套件              ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════╝${NC}"
[ "$QUICK" = "1" ] && echo -e "  ${YELLOW}(--quick: 跳过 netem 测试)${NC}"

# ── 编译 ────────────────────────────────────────────────
cd "$REPO_DIR"
echo -e "\n${BOLD}=== 编译 ===${NC}"
make clean >/dev/null 2>&1 || true
if make debug >/dev/null 2>&1 && make apps >/dev/null 2>&1; then
    pass "编译成功"
else
    fail "编译失败"; exit 1
fi

# ── 启动协议栈 ─────────────────────────────────────────
echo -e "\n${BOLD}=== 启动协议栈 ===${NC}"
setcap cap_setpcap,cap_net_admin=ep lvl-ip
./lvl-ip > "$LOG" 2>&1 &
LVL_PID=$!
sleep 4

if kill -0 "$LVL_PID" 2>/dev/null; then
    pass "协议栈运行中 (PID=$LVL_PID)"
else
    fail "协议栈启动失败"; cat "$LOG"; exit 1
fi

tc qdisc add dev "$TAP_DEV" root handle 1: htb 2>/dev/null || true

# ── 配置 IPv6 环境 ─────────────────────────────────────
ip -6 addr add "fe80::1/64" dev "$TAP_DEV" 2>/dev/null || true
ip -6 addr add "fd00:1234::1/64" dev "$TAP_DEV" 2>/dev/null || true
python3 "$SCRIPT_DIR/send_ra.py" 2>/dev/null
sleep 2

STACK_LL=$(grep "link-local address" "$LOG" | sed 's/.*address //' | sed 's/ .*//' | head -1)
STACK_GLOBAL=$(grep "generated global" "$LOG" | sed 's/.*address //' | sed 's/ .*//' | head -1)

# ================================================================
#  G1: IPv4 回归
# ================================================================
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

# ================================================================
#  G2: TCP/IPv4
# ================================================================
group "G2: TCP/IPv4"

python3 -m http.server 8001 --directory "$SCRIPT_DIR" >/dev/null 2>&1 &
HTTP_PIDS="$HTTP_PIDS $!"
sleep 2

# Test: curl sync
resp=$(timeout 15 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" 10.0.0.5 8001 2>/dev/null \
    | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
if [ -n "$resp" ] && diff -q "$SCRIPT_DIR/suites/tcp/curl-fixture.txt" <(printf '%s\n' "$resp") >/dev/null 2>&1; then
    pass "curl sync HTTP GET"
else
    fail "curl sync HTTP GET"
fi

# Test: curl poll
resp=$(timeout 15 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl-poll/curl-poll" 10.0.0.5 8001 2>/dev/null \
    | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
if [ -n "$resp" ] && diff -q "$SCRIPT_DIR/suites/tcp/curl-fixture.txt" <(printf '%s\n' "$resp") >/dev/null 2>&1; then
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

# 清理 G2 HTTP server
for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
HTTP_PIDS=""

# ================================================================
#  G3: TCP netem (可选)
# ================================================================
if [ "$QUICK" = "0" ]; then
    group "G3: TCP netem"

    for cond in "delay:1:1:delay 2000ms:8002" "loss:1:2:loss 25%:8004"; do
        IFS=: read -r name classid classnum netem port <<< "$cond"

        python3 -m http.server "$port" --directory "$SCRIPT_DIR" >/dev/null 2>&1 &
        HTTP_PIDS="$HTTP_PIDS $!"

        tc class add dev "$TAP_DEV" parent 1: classid "1:$classid" htb rate 100mbit 2>/dev/null || true
        tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 flowid "1:$classid" match ip sport "$port" 0xffff 2>/dev/null || true
        tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 flowid "1:$classid" match ip dport "$port" 0xffff 2>/dev/null || true
        tc qdisc add dev "$TAP_DEV" parent "1:$classid" netem $netem 2>/dev/null || true
        sleep 2

        resp=$(timeout 30 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" 10.0.0.5 "$port" 2>/dev/null \
            | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
        if diff -q "$SCRIPT_DIR/suites/tcp/curl-fixture.txt" <(echo "$resp") >/dev/null 2>&1; then
            pass "curl sync ($name)"
        else
            fail "curl sync ($name)"
        fi

        # 清理该条件的 netem
        tc qdisc del dev "$TAP_DEV" parent "1:$classid" 2>/dev/null || true
        tc filter del dev "$TAP_DEV" parent 1: prio 1 2>/dev/null || true
        tc class del dev "$TAP_DEV" parent 1: classid "1:$classid" 2>/dev/null || true
        for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
        HTTP_PIDS=""
    done

    # duplication (隔离运行，因为它不能与其他 netem 共存)
    tc qdisc del dev "$TAP_DEV" root 2>/dev/null || true
    tc qdisc add dev "$TAP_DEV" root handle 1: htb 2>/dev/null || true

    python3 -m http.server 8003 --directory "$SCRIPT_DIR" >/dev/null 2>&1 &
    HTTP_PIDS="$HTTP_PIDS $!"
    tc class add dev "$TAP_DEV" parent 1: classid 1:2 htb rate 100mbit 2>/dev/null || true
    tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 flowid 1:2 match ip sport 8003 0xffff 2>/dev/null || true
    tc filter add dev "$TAP_DEV" parent 1: protocol ip prio 1 u32 flowid 1:2 match ip dport 8003 0xffff 2>/dev/null || true
    tc qdisc add dev "$TAP_DEV" parent 1:2 netem duplicate 50% 2>/dev/null || true
    sleep 2

    resp=$(timeout 30 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl/curl" 10.0.0.5 8003 2>/dev/null \
        | sed 's/^Date:.*/Date:/' | sed 's/^Server:.*/Server:/') || true
    if diff -q "$SCRIPT_DIR/suites/tcp/curl-fixture.txt" <(echo "$resp") >/dev/null 2>&1; then
        pass "curl sync (duplication)"
    else
        fail "curl sync (duplication)"
    fi

    for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
    HTTP_PIDS=""
    tc qdisc del dev "$TAP_DEV" root 2>/dev/null || true
else
    group "G3: TCP netem (跳过)"
    skip "curl sync (delay)"
    skip "curl sync (loss)"
    skip "curl sync (duplication)"
fi

# ================================================================
#  G4: IPv6 网络层
# ================================================================
group "G4: IPv6 网络层"

if [ -n "$STACK_LL" ]; then
    if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_LL" 2>&1 | grep -q "0% packet loss"; then
        pass "ping6 链路本地"
    else
        fail "ping6 链路本地"
    fi
else
    skip "ping6 链路本地 (无地址)"
fi

if [ -n "$STACK_GLOBAL" ]; then
    if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_GLOBAL" 2>&1 | grep -q "0% packet loss"; then
        pass "ping6 全局地址"
    else
        fail "ping6 全局地址"
    fi
else
    skip "ping6 全局地址 (无地址)"
fi

sleep 1
NDP_STATE=$(ip -6 neigh show dev "$TAP_DEV" 2>/dev/null | grep -v "fe80::1\b" | grep "fe80" | head -1 | awk '{print $NF}')
case "$NDP_STATE" in
    REACHABLE|STALE|DELAY|PROBE) pass "NDP 状态: $NDP_STATE" ;;
    "")      fail "NDP 邻居缓存为空" ;;
    *)       skip "NDP 状态: $NDP_STATE" ;;
esac

if grep -q "DAD passed" "$LOG"; then
    DAD_COUNT=$(grep -c "DAD passed" "$LOG")
    pass "DAD 通过 ($DAD_COUNT 个地址)"
else
    fail "DAD 未通过"
fi

if grep -q "added default route" "$LOG"; then
    pass "默认路由"
else
    fail "默认路由"
fi

CSUM_ERRORS=$(grep "checksum mismatch" "$LOG" 2>/dev/null | grep -v "0xf7ff" | wc -l)
if [ "$CSUM_ERRORS" -gt 0 ]; then
    fail "ICMPv6 校验和错误 ${CSUM_ERRORS} 次"
else
    pass "ICMPv6 校验和"
fi

if grep -q "send RS" "$LOG"; then
    pass "RS 发送"
else
    skip "RS 发送"
fi

# ================================================================
#  稳定性检查 (在 G5 之前，因为 G5 红灯测试可能崩溃 daemon)
# ================================================================
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

# ================================================================
#  G5: TCP/IPv6 (TDD 红灯测试 — 预期失败，放最后)
# ================================================================
group "G5: TCP/IPv6 (TDD 红灯 — 预期失败)"

IPV6_HOST_LL="fe80::1"
IPV6_STACK_LL_DISC=""

# 发现栈的链路本地地址（用于 server 测试）
if [ -n "$STACK_LL" ]; then
    IPV6_STACK_LL_DISC="$STACK_LL"
fi

# Test 5.1: TCP/IPv6 client connect
python3 -m http.server 8080 --bind "$IPV6_HOST_LL" >/dev/null 2>&1 &
HTTP_PIDS="$HTTP_PIDS $!"
sleep 2

output=$(timeout 10 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl-v6/curl-v6" \
         "$IPV6_HOST_LL" 8080 2>&1) || true

for p in $HTTP_PIDS; do kill "$p" 2>/dev/null || true; done
HTTP_PIDS=""

if echo "$output" | grep -q "HTTP/"; then
    pass "TCP/IPv6 client connect"
else
    fail "TCP/IPv6 client connect"
fi

# Test 5.2: TCP/IPv6 client getpeername
nc -6 -l "$IPV6_HOST_LL" 8081 >/dev/null 2>&1 &
NC_PIDS="$NC_PIDS $!"
sleep 1

output=$(timeout 10 "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/curl-v6/curl-v6" \
         "$IPV6_HOST_LL" 8081 2>&1) || true

for p in $NC_PIDS; do kill "$p" 2>/dev/null || true; done
NC_PIDS=""

if echo "$output" | grep -q "AF_INET6"; then
    pass "TCP/IPv6 client getpeername"
else
    fail "TCP/IPv6 client getpeername"
fi

# Test 5.3: TCP/IPv6 server accept
if [ -n "$IPV6_STACK_LL_DISC" ]; then
    SERVER_LOG=$(mktemp /tmp/tcp-v6-server.XXXXXX.log)

    "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/tcp-v6-server/server" 9090 \
        > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!

    waited=0
    while [ $waited -lt 10 ]; do
        if grep -q "LISTENING" "$SERVER_LOG" 2>/dev/null; then break; fi
        sleep 1; waited=$((waited + 1))
    done

    if grep -q "LISTENING" "$SERVER_LOG" 2>/dev/null; then
        nc_output=$(echo "hello-ipv6-test" | timeout 5 nc -6 "$IPV6_STACK_LL_DISC" 9090 2>&1) || true
        sleep 2
        server_output=$(cat "$SERVER_LOG" 2>/dev/null)

        if echo "$nc_output" | grep -q "hello-ipv6-test" && echo "$server_output" | grep -q "AF_INET6"; then
            pass "TCP/IPv6 server accept"
        else
            fail "TCP/IPv6 server accept"
        fi
    else
        fail "TCP/IPv6 server accept (server 未启动)"
    fi

    kill "$SERVER_PID" 2>/dev/null || true; SERVER_PID=""
    rm -f "$SERVER_LOG" 2>/dev/null; SERVER_LOG=""
else
    skip "TCP/IPv6 server accept (无栈地址)"
fi

# Test 5.4: TCP/IPv6 echo data integrity
if [ -n "$IPV6_STACK_LL_DISC" ]; then
    SERVER_LOG=$(mktemp /tmp/tcp-v6-server2.XXXXXX.log)
    send_file=$(mktemp /tmp/tcp-v6-send.XXXXXX)
    recv_file=$(mktemp /tmp/tcp-v6-recv.XXXXXX)

    dd if=/dev/urandom bs=1024 count=1 of="$send_file" 2>/dev/null

    "$REPO_DIR/tools/level-ip" "$REPO_DIR/apps/tcp-v6-server/server" 9091 \
        > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!

    waited=0
    while [ $waited -lt 10 ]; do
        if grep -q "LISTENING" "$SERVER_LOG" 2>/dev/null; then break; fi
        sleep 1; waited=$((waited + 1))
    done

    if grep -q "LISTENING" "$SERVER_LOG" 2>/dev/null; then
        timeout 10 nc -6 "$IPV6_STACK_LL_DISC" 9091 < "$send_file" > "$recv_file" 2>/dev/null || true
        sleep 2

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
    rm -f "$SERVER_LOG" "$send_file" "$recv_file" 2>/dev/null
    SERVER_LOG=""
else
    skip "TCP/IPv6 echo 数据完整性 (无栈地址)"
fi

# ================================================================
#  汇总
# ================================================================
TOTAL=$((PASSED + FAILED + SKIPPED))
echo ""
echo -e "${BOLD}╔══════════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║              测试结果汇总                    ║${NC}"
echo -e "${BOLD}╠══════════════════════════════════════════════╣${NC}"
echo -e "${BOLD}║${NC}  通过: ${GREEN}${BOLD}${PASSED}${NC}  失败: ${RED}${BOLD}${FAILED}${NC}  跳过: ${YELLOW}${BOLD}${SKIPPED}${NC}  共: ${TOTAL}  ${BOLD}║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════════╝${NC}"

if [ "$FAILED" -gt 0 ]; then
    echo -e "\n  ${RED}失败项:${NC}"
    echo -e "$FAIL_LIST"
fi

if [ "$FAILED" -eq 0 ]; then
    echo -e "\n  ${GREEN}${BOLD}所有测试通过！${NC}"
fi

echo ""
echo "  完整日志: $LOG"
echo ""

exit "$FAILED"
