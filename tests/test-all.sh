#!/bin/bash
#
# test-all.sh — 完整测试套件（IPv4 + IPv6）
#
# 用法: sudo bash tests/test-all.sh
#

set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
LOG="/tmp/lvl-ip-test-all.log"
TAP_DEV="tap0"
PREFIX="fd00:1234"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0
SKIPPED=0

pass() { echo -e "  ${GREEN}✅ PASS${NC}: $1"; PASSED=$((PASSED+1)); }
fail() { echo -e "  ${RED}❌ FAIL${NC}: $1"; FAILED=$((FAILED+1)); }
skip() { echo -e "  ${YELLOW}⏭ SKIP${NC}: $1"; SKIPPED=$((SKIPPED+1)); }
info() { echo -e "  ${YELLOW}ℹ${NC} $1"; }

cleanup() {
    echo ""
    echo "=== 清理 ==="
    pkill lvl-ip 2>/dev/null || true
    ip -6 addr del "${PREFIX}::1/64" dev "$TAP_DEV" 2>/dev/null || true
    ip -6 addr del "fe80::1/64" dev "$TAP_DEV" 2>/dev/null || true
    pkill -f "nc -6" 2>/dev/null || true
    echo "  已清理"
}

trap cleanup EXIT

echo "======================================"
echo "  level-ip 完整测试套件"
echo "======================================"
echo ""

# ============================================================
cd "$REPO_DIR"

# --- 编译 ---
echo "=== 编译 ==="
make clean 2>/dev/null || true
MAKE_OUT=$(make debug 2>&1)
if echo "$MAKE_OUT" | grep -q "lvl-ip needs"; then
    pass "编译成功"
else
    fail "编译失败"; echo "$MAKE_OUT" | tail -5; exit 1
fi

# --- 启动协议栈 ---
echo ""
echo "=== 启动协议栈 ==="
setcap cap_setpcap,cap_net_admin=ep lvl-ip
./lvl-ip > "$LOG" 2>&1 &
LVL_PID=$!
sleep 4

if kill -0 $LVL_PID 2>/dev/null; then
    pass "协议栈运行中 (PID=$LVL_PID)"
else
    fail "协议栈启动失败"; cat "$LOG"; exit 1
fi

# --- 配置 IPv6 环境 ---
echo ""
echo "=== 配置 IPv6 环境 ==="
ip -6 addr add "fe80::1/64" dev "$TAP_DEV" 2>/dev/null || true
ip -6 addr add "${PREFIX}::1/64" dev "$TAP_DEV" 2>/dev/null || true
pass "tap0 IPv6 地址已配置"

# 发送 RA
python3 "$SCRIPT_DIR/send_ra.py" 2>/dev/null
sleep 2
if grep -q "recv RA" "$LOG"; then
    pass "协议栈收到 RA"
else
    fail "协议栈未收到 RA"
fi

# 提取地址
STACK_LL=$(grep "link-local address" "$LOG" | sed 's/.*address //' | sed 's/ .*//' | head -1)
STACK_GLOBAL=$(grep "generated global" "$LOG" | sed 's/.*address //' | sed 's/ .*//' | head -1)
info "链路本地: $STACK_LL"
info "全局地址: $STACK_GLOBAL"

# ============================================================
#                    IPv4 测试
# ============================================================
echo ""
echo "======================================
  IPv4 回归测试
======================================"

# --- Test: IPv4 ping ---
echo ""
echo "--- IPv4 Ping ---"
if ping -c 2 -W 2 -I "$TAP_DEV" 10.0.0.4 2>&1 | grep -q "0% packet loss"; then
    pass "IPv4 ping 通"
else
    fail "IPv4 ping 失败"
fi

# --- Test: ARP ---
echo ""
echo "--- ARP ---"
ping -c 1 -W 2 -I "$TAP_DEV" 10.0.0.4 >/dev/null 2>&1 || true
sleep 1
if arp -n -i "$TAP_DEV" 2>/dev/null | grep -q "10.0.0.4"; then
    pass "ARP 缓存正常"
else
    # 尝试 ip neigh
    if ip neigh show dev "$TAP_DEV" | grep -q "10.0.0.4"; then
        pass "ARP 缓存正常 (via ip neigh)"
    else
        fail "ARP 缓存异常"
    fi
fi

# ============================================================
#                    IPv6 测试
# ============================================================
echo ""
echo "======================================
  IPv6 测试
======================================"

# --- Test: ping6 链路本地 ---
echo ""
echo "--- ping6 链路本地 ---"
if [ -n "$STACK_LL" ]; then
    if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_LL" 2>&1 | grep -q "0% packet loss"; then
        pass "ping6 链路本地通"
    else
        fail "ping6 链路本地失败"
    fi
else
    skip "无链路本地地址"
fi

# --- Test: ping6 全局地址 ---
echo ""
echo "--- ping6 全局地址 ---"
if [ -n "$STACK_GLOBAL" ]; then
    if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_GLOBAL" 2>&1 | grep -q "0% packet loss"; then
        pass "ping6 全局地址通"
    else
        fail "ping6 全局地址失败"
    fi
else
    skip "无全局地址"
fi

# --- Test: NDP 邻居缓存 ---
echo ""
echo "--- NDP 邻居缓存 ---"
sleep 1
NDP_STATE=$(ip -6 neigh show dev "$TAP_DEV" | grep -v "fe80::1\b" | grep "fe80" | head -1 | awk '{print $NF}')
if [ -n "$NDP_STATE" ]; then
    case "$NDP_STATE" in
        REACHABLE|STALE|DELAY|PROBE)
            pass "NDP 状态: $NDP_STATE"
            ;;
        FAILED)
            fail "NDP 状态: FAILED"
            ;;
        *)
            skip "NDP 状态: $NDP_STATE (未知)"
            ;;
    esac
else
    fail "NDP 邻居缓存为空"
fi

# --- Test: SLAAC DAD ---
echo ""
echo "--- SLAAC DAD ---"
if grep -q "DAD passed" "$LOG"; then
    DAD_COUNT=$(grep -c "DAD passed" "$LOG")
    pass "DAD 通过 (${DAD_COUNT} 个地址)"
else
    fail "DAD 未通过"
fi

# --- Test: 默认路由 ---
echo ""
echo "--- 默认路由 ---"
if grep -q "added default route" "$LOG"; then
    pass "默认路由已添加"
else
    fail "默认路由未添加"
fi

# --- Test: ICMPv6 校验和 ---
echo ""
echo "--- ICMPv6 校验和 ---"
# 排除 DAD 回环误报 (0xf7ff = DAD NS with src=:: looped back through TAP)
CSUM_ERRORS=$(grep "checksum mismatch" "$LOG" | grep -v "0xf7ff" | wc -l)
if [ "$CSUM_ERRORS" -gt 0 ]; then
    fail "校验和错误 ${CSUM_ERRORS} 次"
else
    pass "无校验和错误"
fi

# --- Test: RS 发送 ---
echo ""
echo "--- RS 发送 ---"
if grep -q "send RS" "$LOG"; then
    pass "RS 已发送"
else
    skip "RS 未发送"
fi

# ============================================================
#                    崩溃检测
# ============================================================
echo ""
echo "======================================
  稳定性检查
======================================"

# --- Test: 协议栈仍存活 ---
echo ""
echo "--- 协议栈稳定性 ---"
if kill -0 $LVL_PID 2>/dev/null; then
    pass "协议栈仍然运行"
else
    fail "协议栈已崩溃"
fi

# --- Test: 无 ThreadSanitizer 错误 ---
echo ""
echo "--- ThreadSanitizer ---"
if grep -q "ThreadSanitizer" "$LOG"; then
    fail "检测到线程错误"
else
    pass "无线程错误"
fi

# --- Test: 无 double-free ---
echo ""
echo "--- 内存安全 ---"
if grep -q "double free\|corruption\|munmap_chunk\|buffer overflow" "$LOG"; then
    fail "检测到内存错误"
else
    pass "无内存错误"
fi

# ============================================================
#                    汇总
# ============================================================
echo ""
echo "======================================"
echo -e "  通过: ${GREEN}${PASSED}${NC}  失败: ${RED}${FAILED}${NC}  跳过: ${YELLOW}${SKIPPED}${NC}"
if [ $FAILED -eq 0 ]; then
    echo -e "  ${GREEN}所有测试通过！${NC}"
else
    echo -e "  ${RED}有 ${FAILED} 个测试失败${NC}"
fi
echo "======================================"
echo ""
echo "完整日志: $LOG"

exit $FAILED
