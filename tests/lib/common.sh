#!/bin/bash
# common.sh — 共享变量、颜色、计数器、辅助函数

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
LOG="/tmp/lvl-ip-test-run.log"
TAP_DEV="tap0"
FIXTURE="$REPO_DIR/test-fixtures/curl-fixture.txt"

# ── 颜色 ────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

# ── 计数器 ──────────────────────────────────────────────
PASSED=0; FAILED=0; SKIPPED=0
FAIL_LIST=""

pass()    { echo -e "  ${GREEN}✅ PASS${NC}: $1"; PASSED=$((PASSED+1)); }
fail()    { echo -e "  ${RED}❌ FAIL${NC}: $1"; FAILED=$((FAILED+1)); FAIL_LIST="$FAIL_LIST\n    $1"; }
skip()    { echo -e "  ${YELLOW}⏭  SKIP${NC}: $1"; SKIPPED=$((SKIPPED+1)); }
group()   { echo -e "\n${CYAN}${BOLD}━━━ $1 ━━━${NC}"; }
info()    { echo -e "  ${YELLOW}ℹ${NC} $1"; }

# ── 进程跟踪 ────────────────────────────────────────────
LVL_PID=""
HTTP_PIDS=""
NC_PIDS=""
SERVER_PID=""
SERVER_LOG=""

# ── 清理 ────────────────────────────────────────────────
cleanup() {
    echo -e "\n${BOLD}=== 清理 ===${NC}"
    [ -n "$LVL_PID" ]    && kill "$LVL_PID" 2>/dev/null || true
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

# ── 编译 ────────────────────────────────────────────────
do_build() {
    cd "$REPO_DIR"
    echo -e "\n${BOLD}=== 编译 ===${NC}"
    make clean >/dev/null 2>&1 || true
    if make debug >/dev/null 2>&1 && make apps >/dev/null 2>&1; then
        pass "编译成功"
    else
        fail "编译失败"; exit 1
    fi
}

# ── 启动协议栈 ──────────────────────────────────────────
start_stack() {
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

    # 配置 IPv6 环境
    ip -6 addr add "fe80::1/64" dev "$TAP_DEV" 2>/dev/null || true
    ip -6 addr add "fd00:1234::1/64" dev "$TAP_DEV" 2>/dev/null || true
    python3 "$SCRIPT_DIR/send_ra.py" 2>/dev/null
    sleep 2

    STACK_LL=$(grep "link-local address" "$LOG" | sed 's/.*address //' | sed 's/ .*//' | head -1)
    STACK_GLOBAL=$(grep "generated global" "$LOG" | sed 's/.*address //' | sed 's/ .*//' | head -1)
}

# ── 等待 server 就绪 ────────────────────────────────────
wait_server_ready() {
    local log_file="$1"
    local waited=0
    while [ $waited -lt 10 ]; do
        if grep -q "LISTENING" "$log_file" 2>/dev/null; then
            return 0
        fi
        sleep 1; waited=$((waited + 1))
    done
    return 1
}

# ── 汇总 ────────────────────────────────────────────────
# expected_failures: 预期失败的测试数（如 G5 红灯测试）
print_summary() {
    local expected="${1:-0}"
    local real_failures=$((FAILED - expected))
    [ "$real_failures" -lt 0 ] && real_failures=0
    local total=$((PASSED + FAILED + SKIPPED))
    echo ""
    echo -e "${BOLD}╔══════════════════════════════════════════════╗${NC}"
    echo -e "${BOLD}║              测试结果汇总                    ║${NC}"
    echo -e "${BOLD}╠══════════════════════════════════════════════╣${NC}"
    echo -e "${BOLD}║${NC}  通过: ${GREEN}${BOLD}${PASSED}${NC}  失败: ${RED}${BOLD}${FAILED}${NC}  跳过: ${YELLOW}${BOLD}${SKIPPED}${NC}  共: ${total}  ${BOLD}║${NC}"
    if [ "$expected" -gt 0 ]; then
        echo -e "${BOLD}║${NC}  预期失败: ${YELLOW}${BOLD}${expected}${NC}  实际失败: ${RED}${BOLD}${real_failures}${NC}                    ${BOLD}║${NC}"
    fi
    echo -e "${BOLD}╚══════════════════════════════════════════════╝${NC}"

    if [ "$FAILED" -gt 0 ]; then
        echo -e "\n  ${RED}失败项:${NC}"
        echo -e "$FAIL_LIST"
    fi

    if [ "$real_failures" -eq 0 ]; then
        echo -e "\n  ${GREEN}${BOLD}所有测试通过！${NC}"
    fi

    echo ""
    echo "  完整日志: $LOG"
    echo ""

    exit "$real_failures"
}
