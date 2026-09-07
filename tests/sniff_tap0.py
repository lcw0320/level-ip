#!/usr/bin/env python3
"""sniff_tap0.py — 在 tap0 上用 AF_PACKET 抓 IPv6/ARP 帧,打印原始字节"""
import socket
import struct
import sys
import time

DURATION = int(sys.argv[1]) if len(sys.argv) > 1 else 8

s = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.ntohs(0x0003))
s.bind(('tap0', 0))
s.settimeout(DURATION)

deadline = time.time() + DURATION
count = 0
while time.time() < deadline:
    try:
        data, _ = s.recvfrom(2048)
    except socket.timeout:
        break
    count += 1
    eth_type = struct.unpack('!H', data[12:14])[0]
    if eth_type == 0x86DD:
        nxt = data[20]
        if nxt == 58:
            icmp_type = data[54]
            names = {133: 'RS', 134: 'RA', 135: 'NS', 136: 'NA', 129: 'EchoRep',
                     128: 'EchoReq', 143: 'MLD'}
            print(f"[{time.time()%1000:7.1f}] ICMPv6 {names.get(icmp_type, str(icmp_type))} "
                  f"len={len(data)}")
            print("  " + data[:min(len(data), 96)].hex(' '))
        else:
            print(f"[{time.time()%1000:7.1f}] IPv6 nexthdr={nxt} len={len(data)}")
    elif eth_type == 0x0806:
        print(f"[{time.time()%1000:7.1f}] ARP len={len(data)}")
        print("  " + data[:min(len(data), 60)].hex(' '))
print(f"total frames: {count}")
