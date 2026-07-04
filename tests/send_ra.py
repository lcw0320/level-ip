#!/usr/bin/env python3
"""
send_ra.py — 在 tap0 上发送 ICMPv6 Router Advertisement
模拟路由器，让协议栈通过 SLAAC 获取全局地址。

用法:
  sudo python3 send_ra.py          # 发送一次 RA 后退出
  sudo python3 send_ra.py --loop   # 每 3 秒发一次，Ctrl+C 退出
"""

import socket
import struct
import sys
import time
import os

# === 配置 ===
IFACE = "tap0"
ROUTER_ADDR = "fd00:1234::1"
PREFIX = "fd00:1234::"
PREFIX_LEN = 64
VALID_LIFETIME = 86400     # 24 小时
PREFERRED_LIFETIME = 14400  # 4 小时

# 所有节点多播地址
ALL_NODES = "ff02::1"


def build_icmpv6_ra():
    """构造 ICMPv6 RA 报文（不含校验和，校验和由内核计算）"""
    # RA 固定部分 (8 bytes after ICMPv6 header)
    # cur_hop_limit(1) + flags(1) + router_lifetime(2) +
    # reachable_time(4) + retrans_timer(4)
    cur_hop_limit = 64
    flags = 0x00  # M=0 (no DHCPv6), O=0 (no other config)
    router_lifetime = 1800  # 30 分钟
    reachable_time = 0      # 未指定
    retrans_timer = 0       # 未指定

    ra_fixed = struct.pack("!BBHII",
                           cur_hop_limit, flags, router_lifetime,
                           reachable_time, retrans_timer)

    # Prefix Information Option (type=3, length=4 = 32 bytes)
    # type(1) + length(1) + prefix_len(1) + flags(1) +
    # valid_lifetime(4) + preferred_lifetime(4) + reserved(4) + prefix(16)
    prefix_opt_type = 3
    prefix_opt_len = 4  # in 8-byte units = 32 bytes
    la_flags = 0xC0     # L=1 (on-link), A=1 (autonomous)
    reserved = 0

    prefix_bytes = socket.inet_pton(socket.AF_INET6, PREFIX)

    prefix_opt = struct.pack("!BBBBIIi",
                             prefix_opt_type, prefix_opt_len,
                             PREFIX_LEN, la_flags,
                             VALID_LIFETIME, PREFERRED_LIFETIME,
                             reserved) + prefix_bytes

    # ICMPv6 header: type(1) + code(1) + checksum(2) = 4 bytes
    # type=134 (Router Advertisement), code=0
    # checksum=0 (kernel will fill if we use IPV6_CHECKSUM, otherwise we compute)
    icmpv6_type = 134
    icmpv6_code = 0

    payload = ra_fixed + prefix_opt
    icmpv6_hdr = struct.pack("!BBH", icmpv6_type, icmpv6_code, 0)

    return icmpv6_hdr + payload


def icmpv6_checksum(src, dst, icmpv6_data):
    """计算 ICMPv6 校验和（含伪首部）"""
    # Pseudo-header: src(16) + dst(16) + length(4) + zeros(3) + nh(1)
    length = len(icmpv6_data)
    pseudo = (
        socket.inet_pton(socket.AF_INET6, src) +
        socket.inet_pton(socket.AF_INET6, dst) +
        struct.pack("!I", length) +
        b'\x00\x00\x00\x3a'  # next header = 58 (ICMPv6)
    )

    data = pseudo + icmpv6_data
    if len(data) % 2:
        data += b'\x00'

    total = 0
    for i in range(0, len(data), 2):
        word = (data[i] << 8) + data[i + 1]
        total += word

    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)

    return ~total & 0xFFFF


def send_ra(iface, loop=False):
    """发送 RA 报文"""
    # 获取 tap0 的链路本地地址
    # 通常 tap0 会有自动生成的 fe80:: 地址
    router_ll = None
    addrs = os.popen(f"ip -6 addr show dev {iface} scope link").read()
    for line in addrs.split("\n"):
        line = line.strip()
        if line.startswith("inet6 fe80"):
            router_ll = line.split()[1].split("/")[0]
            break

    if not router_ll:
        # 如果没有链路本地地址，用手动配置的地址
        router_ll = ROUTER_ADDR
        print(f"[!] 未找到 {iface} 的链路本地地址，使用 {router_ll}")

    print(f"[*] 路由器地址: {router_ll}")
    print(f"[*] 发送 RA 到: {ALL_NODES}")
    print(f"[*] 前缀: {PREFIX}/{PREFIX_LEN}")

    # 构造 ICMPv6 RA
    icmpv6_data = build_icmpv6_ra()

    # 计算校验和
    csum = icmpv6_checksum(router_ll, ALL_NODES, icmpv6_data)

    # 填入校验和
    icmpv6_data = icmpv6_data[:2] + struct.pack("!H", csum) + icmpv6_data[4:]

    # 创建 raw IPv6 socket
    try:
        sock = socket.socket(socket.AF_INET6, socket.SOCK_RAW, socket.IPPROTO_ICMPV6)
    except PermissionError:
        print("[!] 需要 root 权限: sudo python3 send_ra.py")
        sys.exit(1)

    # 绑定到指定接口
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BINDTODEVICE, iface.encode())

    # 设置 hop_limit = 255 (RFC 4861 要求)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_MULTICAST_HOPS, 255)
    sock.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_UNICAST_HOPS, 255)

    # 目的地址：所有节点多播
    dst = (ALL_NODES, 0, 0, 0)

    count = 0
    try:
        while True:
            sock.sendto(icmpv6_data, dst)
            count += 1
            print(f"[+] RA #{count} 已发送 (checksum=0x{csum:04x})")

            if not loop:
                break
            time.sleep(3)
    except KeyboardInterrupt:
        print(f"\n[*] 停止，共发送 {count} 个 RA")
    finally:
        sock.close()


if __name__ == "__main__":
    loop = "--loop" in sys.argv
    send_ra(IFACE, loop=loop)
