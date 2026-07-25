#!/bin/bash
#
# test-run.sh — 统一测试 runner
#
# 用法: sudo bash tests/test-run.sh [--quick]
#
#   --quick  跳过 netem 测试（packet-delay/loss/duplication），加速运行
#
# 测试分组:
#   G1: IPv4 回归     (ping, ARP)
#   G2: TCP/IPv4 + UDP/IPv4 (curl, connection-refused, server echo, UDP echo)
#   G3: TCP netem     (delay, loss, duplication)  -- 除非 --quick
#   G4: IPv6 网络层   (ping6, NDP, SLAAC, DAD)
#   G5: TCP/IPv6      (client connect, server accept, echo)
#

set -eu

QUICK=0
for arg in "$@"; do
    case "$arg" in
        --quick) QUICK=1 ;;
        *) echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# ── 加载模块 ────────────────────────────────────────────
LIB_DIR="$(cd "$(dirname "$0")/lib" && pwd)"
source "$LIB_DIR/common.sh"
source "$LIB_DIR/g1-ipv4.sh"
source "$LIB_DIR/g2-transport.sh"
source "$LIB_DIR/g3-netem.sh"
source "$LIB_DIR/g4-ipv6.sh"
source "$LIB_DIR/g5-tcp6.sh"

trap cleanup EXIT

# ── 启动 ────────────────────────────────────────────────
echo -e "${BOLD}╔══════════════════════════════════════╗${NC}"
echo -e "${BOLD}║   level-ip 统一测试套件              ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════╝${NC}"
[ "$QUICK" = "1" ] && echo -e "  ${YELLOW}(--quick: 跳过 netem 测试)${NC}"

do_build
start_stack

# ── 执行测试 ────────────────────────────────────────────
run_g1_ipv4
run_g2_transport
run_g3_netem
run_g4_ipv6
run_stability_check
run_g5_tcp6

# ── 汇总 (G5 有 4 个预期失败) ───────────────────────────
print_summary 4
