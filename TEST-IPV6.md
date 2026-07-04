# IPv6 手动测试用例

> ⚠️ **工作流教训**：之前所有任务只验证了编译通过（L3），从未做过运行时验证（L2/L1），
> 导致 ping6 明明不通却标记为"完成"。
> 本测试文档要求**逐条手动执行**，每条都必须看到预期结果才算通过。

---

## 测试方法论

```
每条测试的判定标准：
  ✅ PASS  = 看到预期输出（不是编译通过，是运行时行为正确）
  ❌ FAIL  = 没看到预期输出 → 停下来调试，不要跳过
  ⚠️ GATE  = 必须通过才能继续后续测试（前置依赖）
```

---

## 准备工作

### 环境配置

```bash
cd /home/ms/code/study/level-ip

# 编译
make clean 2>/dev/null; make debug

# 设权限
sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip

# 创建 TAP 设备节点（首次）
sudo mkdir -p /dev/net
sudo mknod /dev/net/tap c 10 200 2>/dev/null
sudo chmod 600 /dev/net/tap
```

### 终端 1：启动协议栈

```bash
sudo ./lvl-ip 2>&1 | tee /tmp/lvl-ip.log
```

**⚠️ GATE 检查点 #0**：启动日志必须包含：
```
addrconf: link-local address fe80::XXXX:XXXX:XXXX:XXXX (MAC XX:XX:XX:XX:XX:XX)
```
如果看不到 → 检查 `ipv6_addrconf.c` 的 `ipv6_addrconf_init()` 是否被 `main.c` 调用。

**记下你的地址**（后面用 `$STACK_LL` 代替），例如：
```bash
export STACK_LL="fe80::20c:29ff:fe6d:5025"
```

### 终端 2：配置宿主机

```bash
sudo ip -6 addr add fe80::1/64 dev tap0
ip -6 addr show tap0
```

**⚠️ GATE 检查点 #0.5**：必须看到 `inet6 fe80::1/64 scope link`。

### 终端 3：抓包

```bash
sudo tcpdump -i tap0 -n icmp6
```

---

## 测试 1：ping6 基本连通 ⚠️ GATE

> 这是最核心的测试。不通就说明收包路径或发包路径有 bug。

```bash
# 终端 2 执行
ping6 -c 1 -I tap0 $STACK_LL
```

### 判定标准

**✅ PASS**：
```
64 bytes from fe80::20c:29ff:fe6d:5025: icmp_seq=1 ttl=64 time=X.XX ms
1 packets transmitted, 1 received, 0% packet loss
```

**❌ FAIL — 100% packet loss**：

检查以下每一项：

```bash
# 1. 协议栈收到 Echo Request 了吗？
grep "icmpv6 in" /tmp/lvl-ip.log | tail -5
# 预期：看到 type=128（Echo Request）

# 2. 协议栈尝试回复了吗？
grep "ipv6_output" /tmp/lvl-ip.log | tail -5
# 如果看到 "route lookup failed" → 路由问题，看下方调试

# 3. 抓包看到什么？
# 终端 3 应该看到 Echo Request 进来
# 如果没看到 Echo Reply 出去 → 发包路径有问题
```

### 常见失败原因和修复方向

| 日志现象 | 原因 | 修复方向 |
|---|---|---|
| `route lookup failed` | 路由表没有到宿主机地址的路由 | `route6_lookup()` 找不到匹配条目，检查路由表初始化 |
| `double free or corruption` | 路由失败后 skb 被释放两次 | `ipv6_output` 失败时不应该 free_skb，让调用方处理 |
| `version mismatch` | 帧数据偏移错误 | 检查 `skb->len` 是否在 `netdev_rx_loop` 中正确设置 |
| `payload_len + 40 exceeds skb len` | skb->len 没设 | `netdev_rx_loop` 中 `skb->len = ret` |
| 抓包看不到 Echo Request | 包没到协议栈 | 检查 netdev ETH_P_IPV6 分派 |
| 看到 Echo Request 但没有 Reply | 收包或回复逻辑有 bug | 检查 `icmpv6_echo_reply` 和 `ipv6_output` |

---

## 测试 2：NDP 邻居缓存

> ping6 通了才能做这个测试。

```bash
# 终端 2 执行
ip -6 neigh show dev tap0
```

### 判定标准

**✅ PASS**：
```
<STACK_LL> dev tap0 lladdr XX:XX:XX:XX:XX:XX REACHABLE
```

**❌ FAIL — 没有条目或状态不是 REACHABLE**：

```bash
# 抓包看 NS/NA 交互
# 终端 3 应该看到：
# 1. 宿主机 → 多播 : NS "谁是 STACK_LL？"
# 2. 协议栈 → 宿主机 : NA "我是，MAC 是 XX:XX..."
# 如果只看到 NS 没看到 NA → 协议栈没回复 NA
```

---

## 测试 3：tcpdump 完整报文分析

> 理解 ping6 过程中每个报文的含义。

```bash
# 先清缓存，重新触发完整交互
sudo ip -6 neigh flush dev tap0

# 抓包（只抓 ICMPv6）
sudo tcpdump -i tap0 -n -v icmp6

# 然后 ping
ping6 -c 1 -I tap0 $STACK_LL
```

### 判定标准

**✅ PASS**：按顺序看到以下报文：

```
# 宿主机做 NDP 地址解析
1. fe80::1 > ff02::1:ffXX:XXXX : ICMP6, neighbor solicitation, who is STACK_LL
2. STACK_LL > fe80::1          : ICMP6, neighbor advertisement, tgt is STACK_LL

# 协议栈可能也做 NDP（反向解析宿主机 MAC）
3. STACK_LL > ff02::1:ff00:0001 : ICMP6, neighbor solicitation, who is fe80::1
4. fe80::1 > STACK_LL          : ICMP6, neighbor advertisement, tgt is fe80::1

# 真正的 ping
5. fe80::1 > STACK_LL : ICMP6, echo request, id XXXX, seq 1
6. STACK_LL > fe80::1 : ICMP6, echo reply, id XXXX, seq 1
```

**❌ FAIL**：缺少上述任何一个步骤。

**理解练习**：
- 第 1-2 步的 NS 目标是多播地址 `ff02::1:ffXX:XXXX`，不是广播。为什么？
- NA 回复的 hop_limit 必须是 255。用 `tcpdump -v` 验证。
- 对比 IPv4 的 ARP：`arping -I tap0 10.0.0.4`，看广播 vs 多播的区别。

---

## 测试 4：IPv4 回归 ⚠️ GATE

> 确认 IPv6 改动没有破坏 IPv4。

```bash
ping -c 3 -I tap0 10.0.0.4
```

### 判定标准

**✅ PASS**：`3 packets transmitted, 3 received, 0% packet loss`

**❌ FAIL**：如果 IPv4 ping 不通了，说明 sock.h 联合体改造（T20-T21）破坏了 IPv4 路径。
检查 `sk->saddr.v4` 和 `sk->daddr.v4` 的赋值是否正确。

---

## 测试 5：DAD（重复地址检测）

```bash
# 重启协议栈，观察 DAD NS
sudo pkill lvl-ip
sudo tcpdump -i tap0 -n 'icmp6[0]==135' &
sudo ./lvl-ip
sleep 3
```

### 判定标准

**✅ PASS**：看到 1-2 个 NS 报文，**源地址为 `::`**。

```
:: > ff02::1:ffXX:XXXX : ICMP6, neighbor solicitation
```

**理解**：DAD NS 的源地址是 `::`，因为地址还没确认可用，不能用它发包。
如果没人回复 NA → 地址可用。如果有人回复 → 地址冲突。

---

## 测试 6：协议栈 RS 发送

```bash
# 在协议栈日志中找 RS 发送记录
grep "ndp send RS" /tmp/lvl-ip.log
```

### 判定标准

**✅ PASS**：看到 `ndp send RS to ff02::2`。

**注意**：`failed to send RS (ret=XX)` 说明路由查找失败。这是一个已知问题——
启动时 IPv6 路由表可能还没初始化完成，RS 发送时机需要调整。
不影响 ping6（ping6 用的是链路本地地址，走直连路由）。

---

## 测试 7：路由表检查

```bash
# 宿主机侧
ip -6 route show dev tap0
```

### 判定标准

**✅ PASS**：看到 `fe80::/64 dev tap0`。

**理解**：这是宿主机内核自动添加的直连路由。`fe80::/64` 表示
"所有以 fe80 开头的链路本地地址都在 tap0 上"。
这就是为什么 ping6 链路本地地址不需要额外配置路由。

---

## 测试 8：Wireshark 逐字段分析

```bash
# 保存抓包
sudo tcpdump -i tap0 -n -w /tmp/ipv6-detail.pcap icmp6 &
ping6 -c 1 -I tap0 $STACK_LL
sleep 1
sudo kill %1 2>/dev/null

# 用 Wireshark 打开
wireshark /tmp/ipv6-detail.pcap
```

### 逐字段验证清单

对照 `include/ipv6.h` 的 `struct ipv6hdr`，在 Wireshark 中验证：

| 字段 | 预期值 | Wireshark 显示 | 你的观察 |
|------|--------|---------------|---------|
| Version | 6 | | |
| Traffic Class | 0 | | |
| Flow Label | 0 | | |
| Payload Length | 64 (Echo) | | |
| Next Header | 58 (ICMPv6) | | |
| Hop Limit | 255 (NDP) 或 64 | | |
| Source Address | fe80::... | | |
| Destination Address | fe80::1 | | |
| ICMPv6 Type | 128/129 | | |
| ICMPv6 Code | 0 | | |
| ICMPv6 Checksum | 非零 | | |

---

## 测试结果记录

| # | 测试 | 结果 | 备注 |
|---|------|------|------|
| 0 | 协议栈启动 | ✅/❌ | |
| 0.5 | 宿主机 IPv6 配置 | ✅/❌ | |
| 1 | ping6 基本连通 | ✅/❌ | |
| 2 | NDP 邻居缓存 | ✅/❌ | |
| 3 | tcpdump 报文分析 | ✅/❌ | |
| 4 | IPv4 回归 | ✅/❌ | |
| 5 | DAD | ✅/❌ | |
| 6 | RS 发送 | ✅/❌ | |
| 7 | 路由表 | ✅/❌ | |
| 8 | Wireshark 字段 | ✅/❌ | |

---

## 调试技巧速查

```bash
# 查看协议栈日志
cat /tmp/lvl-ip.log | grep -E "ipv6|icmpv6|ndp|route"

# 实时看日志
tail -f /tmp/lvl-ip.log

# 开启所有调试宏重新编译
make clean; CFLAGS="-DDEBUG_IPV6 -DDEBUG_ICMPV6 -DDEBUG_NDP -DDEBUG_ADDRCONF" make debug

# GDB 调试
sudo -E gdb ./lvl-ip
(gdb) break ipv6_output
(gdb) break icmpv6_echo_reply
(gdb) break route6_lookup
(gdb) run
```
