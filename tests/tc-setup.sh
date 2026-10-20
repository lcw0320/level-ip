#!/bin/bash
# 拥塞控制测试 tc 配置脚本（对应 Documentation/testing-congestion-control.md）
# 用法: sudo bash tests/tc-setup.sh <command>
#
#   base     基础网络环境（TAP 节点 / 转发 / iptables / htb 根 qdisc），所有场景共用
#   sc2      场景二：8011 双向全丢包（触发 RTO），恢复用 sc2-ok
#   sc2-ok   场景二：恢复 8011 双向不丢包，让传输完成
#   sc3      场景三/四：8012 上行丢 25%（触发快速重传/快速恢复）
#   sc6      场景六：8081 双向全丢包（触发 RTO 拥塞控制重置）
#   clean    删除 tap0 和 ifb0 上的测试 qdisc
#   status   查看当前 qdisc 和 filter

set -e
require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "请用 sudo 运行: sudo bash $0 <command>" >&2
        exit 1
    fi
}

IFACE=tap0

# iptables 规则幂等添加：已存在则跳过
add_rule_once() {
    iptables -C "$@" 2>/dev/null || iptables -I "$@"
}

cmd_base() {
    # TAP 设备节点
    if [ ! -e /dev/net/tap ]; then
        mknod /dev/net/tap c 10 200
    fi
    chmod 0666 /dev/net/tap

    # IP 转发 + iptables（出口网卡 enp4s0 按实际环境修改）
    sysctl -w net.ipv4.ip_forward=1
    add_rule_once INPUT --source 10.0.0.0/24 -j ACCEPT
    add_rule_once -t nat POSTROUTING --out-interface enp4s0 -j MASQUERADE
    add_rule_once FORWARD --in-interface enp4s0 --out-interface $IFACE -j ACCEPT
    add_rule_once FORWARD --in-interface $IFACE --out-interface enp4s0 -j ACCEPT

    # htb 根 qdisc，下场景的 netem 挂在其子 class 上
    tc qdisc replace dev $IFACE root handle 1: htb default 10
    tc class replace dev $IFACE parent 1: classid 1:10 htb rate 100mbit
    echo "base 环境就绪"
}

# 上行丢包（lvl-ip -> server）：10.0.0.5/32 配在 tap0 上，上行包进内核后直接本地交付，
# 不经过任何 egress qdisc，因此用 ifb 把 tap0 ingress 重定向到 ifb0，在 ifb0 上丢。
setup_upload_loss() {
    local port=$1 loss=$2
    modprobe ifb 2>/dev/null || true
    ip link set ifb0 up
    tc qdisc add dev $IFACE handle ffff: ingress 2>/dev/null || true
    tc filter replace dev $IFACE parent ffff: protocol ip prio 1 u32 \
        match ip protocol 6 0xff match ip src 10.0.0.4 match ip dport $port 0xffff \
        action mirred egress redirect dev ifb0
    tc qdisc replace dev ifb0 root handle 2: netem loss $loss limit 10
}

# 场景二：8011 双向 100% 丢包，逼 lvl-ip 走 RTO
#   tap0 egress（sport 8011）= server 下行数据；tap0 ingress→ifb0 = lvl-ip 上行数据
cmd_sc2() {
    tc class replace dev $IFACE parent 1: classid 1:11 htb rate 100mbit
    tc filter replace dev $IFACE parent 1: protocol ip prio 3 u32 flowid 1:11 match ip sport 8011 0xffff
    tc qdisc replace dev $IFACE parent 1:11 handle 110: netem loss 100% limit 10
    setup_upload_loss 8011 100%
    echo "sc2: 8011 双向 loss 100%（恢复请运行 $0 sc2-ok）"
}

cmd_sc2_ok() {
    tc qdisc change dev $IFACE parent 1:11 netem loss 0% limit 10
    tc qdisc change dev ifb0 root handle 2: netem loss 0% limit 10 2>/dev/null || true
    echo "sc2-ok: 8011 双向 loss 0%"
}

# 场景三/四：8012 上行丢 25%，lvl-ip 数据段被丢后 server 回 dupACK，触发快速重传/快速恢复
cmd_sc3() {
    setup_upload_loss 8012 25%
    echo "sc3: 8012 上行 loss 25%"
}

# 场景六：8081 双向 100% 丢包，观察 RTO 触发的 cwnd/ssthresh 重置
cmd_sc6() {
    tc class replace dev $IFACE parent 1: classid 1:13 htb rate 100mbit
    tc filter replace dev $IFACE parent 1: protocol ip prio 4 u32 flowid 1:13 match ip sport 8081 0xffff
    tc qdisc replace dev $IFACE parent 1:13 handle 130: netem loss 100% limit 10
    setup_upload_loss 8081 100%
    echo "sc6: 8081 双向 loss 100%"
}

cmd_clean() {
    tc qdisc del dev $IFACE root 2>/dev/null || true
    tc qdisc del dev $IFACE ingress 2>/dev/null || true
    tc qdisc del dev ifb0 root 2>/dev/null || true
    echo "已删除 tap0（root+ingress）和 ifb0 上的测试 qdisc"
}

cmd_status() {
    tc qdisc show dev $IFACE
    echo "--- ifb0 ---"
    tc qdisc show dev ifb0
    echo "---"
    tc filter show dev $IFACE
    tc filter show dev $IFACE parent ffff:
}

require_root

case "$1" in
    base)   cmd_base ;;
    sc2)    cmd_sc2 ;;
    sc2-ok) cmd_sc2_ok ;;
    sc3)    cmd_sc3 ;;
    sc6)    cmd_sc6 ;;
    clean)  cmd_clean ;;
    status) cmd_status ;;
    *)      grep '^#   ' "$0" | sed 's/^#   //' ;;
esac
