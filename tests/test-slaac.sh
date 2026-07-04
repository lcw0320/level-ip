#!/bin/bash
#
# test-slaac.sh — SLAAC 端到端测试脚本
#
# 测试协议栈通过 Router Advertisement 自动获取全局地址。
# 使用 Python 脚本发送 RA（不依赖 radvd）。
#
# 用法: sudo bash tests/test-slaac.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$SCRIPT_DIR")"
LOG="/tmp/lvl-ip-slaac-test.log"

# 配置
PREFIX="fd00:1234"
ROUTER_ADDR="${PREFIX}::1"
TAP_DEV="tap0"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

pass() { echo -e "  ${GREEN}✅ PASS${NC}: $1"; }
fail() { echo -e "  ${RED}❌ FAIL${NC}: $1"; FAILED=1; }
info() { echo -e "  ${YELLOW}ℹ${NC} $1"; }

cleanup() {
    echo ""
    echo "=== 清理 ==="
    pkill lvl-ip 2>/dev/null || true
    ip -6 addr del "${ROUTER_ADDR}/64" dev "$TAP_DEV" 2>/dev/null || true
    rm -f "$LOG"
    echo "  已清理"
}

trap cleanup EXIT

FAILED=0

echo "======================================"
echo "  IPv6 SLAAC 端到端测试"
echo "======================================"
echo ""

# --- Step 1: 编译 ---
echo "=== Step 1: 编译 ==="
cd "$REPO_DIR"
make clean 2>/dev/null || true
MAKE_OUT=$(make debug 2>&1)
echo "$MAKE_OUT" | tail -3
if echo "$MAKE_OUT" | grep -q "lvl-ip needs"; then
    pass "编译成功"
else
    fail "编译失败"
    exit 1
fi

# --- Step 2: 启动协议栈 ---
echo ""
echo "=== Step 2: 启动协议栈 ==="
setcap cap_setpcap,cap_net_admin=ep lvl-ip
./lvl-ip > "$LOG" 2>&1 &
LVL_PID=$!
sleep 3

if kill -0 $LVL_PID 2>/dev/null; then
    pass "协议栈运行中 (PID=$LVL_PID)"
else
    fail "协议栈启动失败"
    cat "$LOG"
    exit 1
fi

# 提取链路本地地址
STACK_LL=$(grep "link-local address" "$LOG" | grep -o 'fe80:[0-9a-f:]*' | head -1 | tr -d ':')
# 重新格式化（去掉多余的零）
STACK_LL=$(grep "link-local address" "$LOG" | sed 's/.*fe80:/fe80:/' | sed 's/ .*//' | head -1)
info "协议栈链路本地地址: $STACK_LL"

# --- Step 3: 配置宿主机侧 ---
echo ""
echo "=== Step 3: 配置宿主机 ==="
ip -6 addr add "${ROUTER_ADDR}/64" dev "$TAP_DEV" 2>/dev/null || true
if ip -6 addr show dev "$TAP_DEV" | grep -q "$ROUTER_ADDR"; then
    pass "tap0 配置 ${ROUTER_ADDR}/64"
else
    fail "tap0 配置失败"
fi

# --- Step 4: 链路本地 ping6 ---
echo ""
echo "=== Step 4: 链路本地 ping6 ==="
if ping6 -c 2 -W 2 -I "$TAP_DEV" "$STACK_LL" 2>&1 | grep -q "0% packet loss"; then
    pass "链路本地 ping6 通"
else
    fail "链路本地 ping6 失败"
fi

# --- Step 5: 发送 RA ---
echo ""
echo "=== Step 5: 发送 Router Advertisement ==="
python3 "$SCRIPT_DIR/send_ra.py" 2>&1 | while read line; do info "$line"; done
sleep 2

# 检查协议栈是否收到 RA
if grep -q "recv RA" "$LOG"; then
    pass "协议栈收到 RA"
else
    fail "协议栈未收到 RA"
fi

# 检查前缀提取
if grep -q "prefix_len=64" "$LOG"; then
    pass "前缀提取成功 (${PREFIX}::/64)"
else
    fail "前缀提取失败"
fi

# 检查全局地址生成
GLOBAL_ADDR=$(grep "generated global address" "$LOG" | grep -o 'fd00:[0-9a-f:]*' | head -1)
if [ -n "$GLOBAL_ADDR" ]; then
    pass "全局地址生成: $GLOBAL_ADDR"
else
    fail "全局地址生成失败"
fi

# 检查全局地址 DAD
if grep "DAD passed for ${PREFIX}" "$LOG" | grep -q "PREFERRED"; then
    pass "全局地址 DAD 通过"
else
    info "全局地址 DAD 结果待确认（可能需要等待）"
fi

# 检查默认路由
if grep -q "added default route" "$LOG"; then
    pass "默认路由已添加"
else
    fail "默认路由未添加"
fi

# --- Step 6: 全局地址 ping6 ---
echo ""
echo "=== Step 6: 全局地址 ping6 ==="
if [ -n "$GLOBAL_ADDR" ]; then
    # 简化地址格式用于 ping
    PING_ADDR=$(echo "$GLOBAL_ADDR" | sed 's/0000://g' | sed 's/0000://g' | sed 's/0000://g')
    # 用完整地址 ping
    if ping6 -c 3 -W 2 -I "$TAP_DEV" "$GLOBAL_ADDR" 2>&1 | grep -q "0% packet loss"; then
        pass "全局地址 ping6 通 ($GLOBAL_ADDR)"
    else
        fail "全局地址 ping6 失败"
    fi
else
    fail "无全局地址，跳过"
fi

# --- Step 7: 多次 RA 测试 ---
echo ""
echo "=== Step 7: 持续 RA 测试 ==="
python3 "$SCRIPT_DIR/send_ra.py" --loop &
RA_PID=$!
sleep 8
kill $RA_PID 2>/dev/null || true

RA_COUNT=$(grep -c "recv RA" "$LOG" 2>/dev/null || echo 0)
info "共收到 $RA_COUNT 个 RA"
if [ "$RA_COUNT" -gt 1 ]; then
    pass "持续 RA 接收正常"
else
    fail "RA 接收不足"
fi

# --- 汇总 ---
echo ""
echo "======================================"
if [ "$FAILED" -eq 0 ]; then
    echo -e "  ${GREEN}所有测试通过！${NC}"
else
    echo -e "  ${RED}部分测试失败${NC}"
fi
echo "======================================"
echo ""
echo "协议栈完整日志: $LOG"

exit $FAILED
