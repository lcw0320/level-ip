# 拥塞控制机制测试指导

本文档覆盖 RFC 5681 慢启动、拥塞避免、快速重传、快速恢复四个机制的测试方法，对应提交 `3d73c87` 和 `acdcfa9`。

---

## 测试方向与工具选择

lvl-ip 的拥塞控制和重传逻辑只作用于它**发送**的数据，因此收发两个方向测的是完全不同的东西：

| 方向 | 工具 | 被测逻辑 |
|------|------|---------|
| 上传（lvl-ip 发数据） | `curl-verify -u` | 慢启动、拥塞避免、快速重传、快速恢复、RTO（场景一~六） |
| 下载（lvl-ip 收数据） | `curl-verify` | 接收路径：乱序缓存、dupACK 发送、接收窗口（场景七） |

注意：纯下载时 lvl-ip 的 `write_queue` 为空，`tcp_clean_rto_queue` 直接停 RTO 定时器，cwnd/RTO/快速重传都不会触发——下载方向丢包时做重传的是 server 侧协议栈。

### curl-verify 用法

```bash
# 下载：GET /，响应体落盘 download.bin，Content-Length + SHA-256 双重校验
tools/level-ip apps/curl-verify/curl-verify 10.0.0.5 8081 $(sha256sum /tmp/wwwroot/index.html | cut -d' ' -f1)

# 上传：发送 PUT <size>\n + 文件原始字节，比对服务端回传的 SHA-256
tools/level-ip apps/curl-verify/curl-verify -u 10.0.0.5 8081 /tmp/wwwroot/index.html
```

校验失败（字节数不符 / 哈希不一致）打印 `Verify FAILED` 并返回退出码 1。

### 上传服务端

python 的 `http.server` 不接受 PUT（返回 501），上传方向使用配套服务端：

```bash
python3 tests/upload-server.py 8081 10.0.0.5    # 或不带参数，默认 0.0.0.0:8081
```

协议：客户端先发一行 `PUT <size>\n`，随后发 size 字节；服务端收满后回 `OK <sha256>\n`。

---

## 测试环境准备

### 构建

```bash
# 带调试符号和全套协议调试日志（DEBUG_TCP 已包含）
make debug
```

### 网络环境（所有场景共用）

```bash
# TAP 设备
sudo mknod /dev/net/tap c 10 200
sudo chmod 0666 /dev/net/tap

# IP 转发 + iptables（替换 enp4s0 为实际出口网卡）
sudo sysctl -w net.ipv4.ip_forward=1
sudo iptables -I INPUT --source 10.0.0.0/24 -j ACCEPT
sudo iptables -t nat -I POSTROUTING --out-interface enp4s0 -j MASQUERADE
sudo iptables -I FORWARD --in-interface enp4s0 --out-interface tap0 -j ACCEPT
sudo iptables -I FORWARD --in-interface tap0 --out-interface enp4s0 -j ACCEPT

# tc 根 qdisc（供各场景挂 netem 子 qdisc 使用）
sudo tc qdisc add dev tap0 root handle 1: htb default 10
sudo tc class add dev tap0 parent 1: classid 1:10 htb rate 100mbit
```

丢包方向说明（重要）：

- **下行（server → lvl-ip）**：tap0 egress 上只有这部分流量（TAP 方向与真实网卡相反），filter 须匹配 **sport**；
- **上行（lvl-ip → server）**：10.0.0.5/32 配在 tap0 上，上行包进入内核后直接本地交付，**不经过任何 egress qdisc**（lo 上也抓不到），须用 ifb 把 tap0 ingress 重定向到 ifb0，在 ifb0 上丢包。

上行丢包环境（一次性准备，各场景共用）：

```bash
sudo modprobe ifb
sudo ip link set ifb0 up
```

注意：lvl-ip 每次重启都会销毁并重建 tap0，tap0 上挂的 htb/netem/ingress 全部丢失，重启后需重新执行各场景的 tc 命令（ifb0 是独立模块设备，不受影响）。

### 测试文件（所有场景共用）

curl-verify 下载固定请求 `GET /`（apps/curl-verify/curl-verify.c 中硬编码），`http.server` 对目录请求优先返回 `index.html`，因此把 1MB 测试文件命名为 `index.html`；上传直接指定该文件路径：

```bash
mkdir -p /tmp/wwwroot
dd if=/dev/urandom of=/tmp/wwwroot/index.html bs=1M count=1
```

### 监控工具

两个终端分别运行，整个测试过程保持开启：

```bash
# 终端 1：抓包，观察 cwnd 推进与重传行为
sudo tcpdump -i any host 10.0.0.4 -n

# 终端 2：lvl-ip 调试日志
./lvl-ip 2>&1 | grep -E "cwnd|ssthresh|inflight|dupACK|recovery|RTO"
```

---

## 场景一：慢启动 (Slow Start)

### 目标

验证连接建立后 cwnd 从 IW=4×SMSS(=4×536=2144 B) 开始，每收到一个 ACK 按 `min(acked, SMSS)` 增长，直至触及 ssthresh。

### 步骤

```bash
# 1. 启动上传服务端（host 侧）
python3 tests/upload-server.py 8081 10.0.0.5 &

# 2. 启动 lvl-ip
./lvl-ip &
sleep 2

# 3. 上传 1MB 文件（约 1900×SMSS，覆盖完整慢启动爬升）
tools/level-ip apps/curl-verify/curl-verify -u 10.0.0.5 8081 /tmp/wwwroot/index.html
```

### 观测要点

| 阶段 | tcpdump 特征 | 期望日志关键字 |
|------|-------------|--------------|
| 握手完成，首批数据 | 前 4 段 seq 连续，无等待 | `cwnd=2144` |
| 每 ACK 到达 | 下一轮发送量翻倍 | `cwnd` 值递增 |
| cwnd ≥ ssthresh（初始 0xFFFFFFFF，不会触发） | 进入拥塞避免（正常流无丢包不会进入） | — |

### 验收标准

- 首批 `inflight` 不超过 2144 字节（4×536）。
- 每收到一个全量 ACK，`cwnd` 增量 ≤ 536 字节。
- 上传完成，`verify OK`，连接正常关闭，无挂起。

---

## 场景二：拥塞避免 (Congestion Avoidance)

### 目标

在人为触发 RTO 后，ssthresh 被压低，cwnd 从 1×SMSS 重新慢启动，越过 ssthresh 后自动切换拥塞避免模式（每 cwnd 字节 ACK 才增 1×SMSS）。

### 步骤

```bash
# 1. 先建立传输（连接 + 部分数据），再加丢包，保证 RTO 发生在数据阶段
python3 tests/upload-server.py 8011 10.0.0.5 &
./lvl-ip &
sleep 2
tools/level-ip apps/curl-verify/curl-verify -u 10.0.0.5 8011 /tmp/wwwroot/index.html &

sleep 1
# 2. 双向 100% 丢包：tap0 egress（sport 8011）拦 server 下行，tap0 ingress→ifb0 拦 lvl-ip 上行
sudo tc class add dev tap0 parent 1: classid 1:11 htb rate 100mbit
sudo tc filter add dev tap0 parent 1: protocol ip prio 3 u32 flowid 1:11 match ip sport 8011 0xffff
sudo tc qdisc add dev tap0 parent 1:11 handle 110: netem loss 100% limit 10
sudo tc qdisc add dev tap0 handle ffff: ingress
sudo tc filter add dev tap0 parent ffff: protocol ip prio 1 u32 \
    match ip protocol 6 0xff match ip src 10.0.0.4 match ip dport 8011 0xffff \
    action mirred egress redirect dev ifb0
sudo tc qdisc add dev ifb0 root handle 2: netem loss 100% limit 10

sleep 3
# 3. 恢复网络（改为不丢包），观察 cwnd 从 536 重新爬升直至传输完成
sudo tc qdisc change dev tap0 parent 1:11 netem loss 0% limit 10
sudo tc qdisc change dev ifb0 root handle 2: netem loss 0% limit 10
wait
```

### 观测要点

| 时间点 | 期望行为 |
|--------|---------|
| RTO 触发 | `ssthresh = max(inflight/2, 2×536)`；`cwnd = 536` |
| RTO 后慢启动 | cwnd 每 ACK 增 536，直到 cwnd < ssthresh |
| cwnd ≥ ssthresh | 改为每积累 cwnd 字节 ACK 增 536（增速明显变慢） |

### 验收标准

- RTO 后日志出现 `ssthresh` 被设为合理值（≥ 1072）。
- `cwnd` 从 536 开始增长，慢启动阶段每 ACK 翻倍、拥塞避免阶段线性。
- 切换拥塞避免后，每轮 RTT 内 `cwnd` 增量 ≤ 536 字节。
- 最终 `verify OK`。

---

## 场景三：快速重传 (Fast Retransmit)

### 目标

上传方向 25% 丢包使 server 产生 3 个重复 ACK，触发 `tcp_fast_retransmit`，验证不等 RTO 即重传丢失段。

### 步骤

```bash
# 上行 25% 丢包：tap0 ingress→ifb0，在 ifb0 上丢 lvl-ip 发出的数据段
# （下行不丢：server 的 dupACK 能可靠到达，快速重传才能及时触发；如需同时模拟下行丢包，另对 tap0 parent 1:12 挂 netem）
sudo tc qdisc add dev tap0 handle ffff: ingress
sudo tc filter add dev tap0 parent ffff: protocol ip prio 1 u32 \
    match ip protocol 6 0xff match ip src 10.0.0.4 match ip dport 8012 0xffff \
    action mirred egress redirect dev ifb0
sudo tc qdisc add dev ifb0 root handle 2: netem loss 25% limit 10

python3 tests/upload-server.py 8012 10.0.0.5 &
./lvl-ip &
sleep 2
tools/level-ip apps/curl-verify/curl-verify -u 10.0.0.5 8012 /tmp/wwwroot/index.html
```

### 观测要点

tcpdump 中查找：

```
# 快速重传特征：同一 seq 在 RTO 超时前第二次出现
IP 10.0.0.4.XXXX > 10.0.0.5.8012: Flags [P.], seq S:E  ← 原始
IP 10.0.0.5.8012 > 10.0.0.4.XXXX: Flags [.], ack S     ← dupACK ×3（server 发出）
IP 10.0.0.4.XXXX > 10.0.0.5.8012: Flags [P.], seq S:E  ← 快速重传
```

lvl-ip 日志关键字：`dupacks=3`、`fast_retransmit`、`enter_recovery`。

### 验收标准

- 出现 3 个重复 ACK 后，重传在 RTO 超时前（< `rto` 毫秒）发生。
- `ssthresh` 被设为 `max(inflight/2, 1072)`。
- `cwnd` 被 inflate 到 `ssthresh + 3×536`。
- 传输最终 `verify OK`（无挂起）。

---

## 场景四：快速恢复 (Fast Recovery)

### 目标

在 Fast Recovery 期间，验证 cwnd inflate/deflate 行为：每多收一个 dupACK 额外 inflate 1×SMSS；收到新 ACK（推进 snd_una）后 deflate 回 ssthresh，退出恢复状态。

### 步骤

与场景三相同网络配置（25% 丢包），重点在日志观察。

```bash
# 使用调试构建，过滤拥塞控制相关日志
./lvl-ip 2>&1 | grep -E "cwnd|ssthresh|inflight|dupack|recovery"
```

启动传输后观察连续日志段：

```
enter_fast_recovery: ssthresh=X cwnd=X+3*536
dupack #4: cwnd=X+4*536
dupack #5: cwnd=X+5*536
...
new_ack (snd_una advances): exit_fast_recovery cwnd=ssthresh=X
```

### 验收标准

| 事件 | cwnd 变化 |
|------|---------|
| 进入 Fast Recovery | `ssthresh + 3×SMSS` |
| 每个后续 dupACK | `+= SMSS` |
| 收到推进 snd_una 的新 ACK | `= ssthresh`，`in_recovery = 0` |

- `in_recovery` 期间，`tcp_cong_avoid` 中的慢启动/拥塞避免逻辑不执行（守卫生效）。
- 退出恢复后，`dupacks` 归零。

---

## 场景五：Limited Transmit

### 目标

在收到前 2 个 dupACK 时（还未达到 3 个），验证 Limited Transmit 每次临时放行 `N×SMSS` 额外字节，允许新数据进入网络。

### 观测方式

该机制隐含在快速重传场景中，无需单独搭建网络。在场景三中观察日志：

```
dupack #1: extra=536   → tcp_send_next 发 1 个新段
dupack #2: extra=1072  → tcp_send_next 发 1 个新段
dupack #3: → enter fast recovery（不再走 Limited Transmit）
```

### 验收标准

- dupACK 1、2 时，`tcp_send_next` 带非零 `extra` 参数调用，且实际发出了新数据段（tcpdump 可见新 seq）。
- `cwnd` 在这两个 dupACK 期间**不变**（仅 `extra` 临时放行）。

---

## 场景六：RTO 触发的拥塞控制重置

### 目标

验证 RTO 超时路径（`src/tcp_output.c`）正确更新 `ssthresh` 和 `cwnd`。

### 步骤

```bash
python3 tests/upload-server.py 8081 10.0.0.5 &
./lvl-ip &
sleep 2
tools/level-ip apps/curl-verify/curl-verify -u 10.0.0.5 8081 /tmp/wwwroot/index.html &

sleep 1
# 双向 100% 丢包，RTO backoff 1s→2s→4s…
sudo tc class add dev tap0 parent 1: classid 1:13 htb rate 100mbit
sudo tc filter add dev tap0 parent 1: protocol ip prio 4 u32 flowid 1:13 match ip sport 8081 0xffff
sudo tc qdisc add dev tap0 parent 1:13 handle 130: netem loss 100% limit 10
sudo tc qdisc add dev tap0 handle ffff: ingress
sudo tc filter add dev tap0 parent ffff: protocol ip prio 1 u32 \
    match ip protocol 6 0xff match ip src 10.0.0.4 match ip dport 8081 0xffff \
    action mirred egress redirect dev ifb0
sudo tc qdisc add dev ifb0 root handle 2: netem loss 100% limit 10

sleep 5
# 恢复后观察 ssthresh/cwnd 重置与传输续传
sudo tc qdisc change dev tap0 parent 1:13 netem loss 0% limit 10
sudo tc qdisc change dev ifb0 root handle 2: netem loss 0% limit 10
wait
```

### 验收标准

- RTO 触发时：`ssthresh = max(inflight/2, 1072)`，`cwnd = 536`。
- backoff 指数增长（1s → 2s → 4s …），最终达 `TCP_USER_TIMEOUT`(180s) 断开。
- 若网络恢复，cwnd 从 536 重新慢启动，传输继续完成（`verify OK`）。

---

## 场景七：下载方向接收路径测试

### 目标

下载方向丢包测的是 lvl-ip 的**接收路径**：乱序段缓存、丢段检测、连续 dupACK 发送、接收窗口通告。快速重传由 server 侧（host 协议栈）完成。

### 步骤

```bash
# 下行 25% 丢包：tap0 egress 上只有 server 下行流量，filter 匹配 sport
sudo tc class add dev tap0 parent 1: classid 1:12 htb rate 100mbit
sudo tc filter add dev tap0 parent 1: protocol ip prio 2 u32 flowid 1:12 match ip sport 8012 0xffff
sudo tc qdisc add dev tap0 parent 1:12 handle 120: netem loss 25% limit 10
# 上行 ACK 不丢；如需同时丢上行，按场景三的 ifb0 方式另挂（端口 8012）

(cd /tmp/wwwroot && python3 -m http.server 8012 --bind 10.0.0.5) &
./lvl-ip &
sleep 2
tools/level-ip apps/curl-verify/curl-verify 10.0.0.5 8012 $(sha256sum /tmp/wwwroot/index.html | cut -d' ' -f1)
```

### 观测要点

tcpdump：server 同一 seq 段重复出现（重传）、lvl-ip 对同一 ack 值的重复 ACK。

### 验收标准

- 25% 丢包下传输仍能完成，`verify OK`（Content-Length 与 SHA-256 均通过，即乱序重组无差错）。
- 无挂起，连接正常关闭。

---

## 现有测试套件使用方式

统一 runner（G1 IPv4 回归 / G2 TCP+UDP / G3 netem / G4 IPv6 / G5 TCP over IPv6）：

```bash
sudo bash tests/test-run.sh           # 全部
sudo bash tests/test-run.sh --quick   # 跳过 G3 netem 组（delay/loss/duplication）
```

---

## 已知局限

| 限制 | 影响 |
|------|------|
| IW 固定为 4×SMSS，未按 RFC 5681 §3.1 分档 | 慢启动初始窗口偏大，但不影响后续行为验证 |
| 同一段多次 RTO 重传不抑制 ssthresh 二次缩减 | 极端丢包下 ssthresh 可能被压至最小值 |
| 无 SACK，Fast Recovery 仅重传队首段 | 多段同时丢包时恢复效率低 |
| 接收窗口固定 44477，无动态窗口管理 | 高延迟链路下吞吐受 `min(cwnd, rwnd)` 约束 |
| 上行丢包用 ifb ingress 重定向实现 | 上行包经 tap0 ingress 被镜像到 ifb0 丢弃，tap0 egress qdisc 看不到这些包 |
| netem 丢包发生在 AF_PACKET 抓包点之后 | tcpdump/Wireshark 仍会抓到被丢弃的包及后续重传，勿把抓到的包当作已送达 |

---

## 参考

- RFC 5681 §3.1 — Slow Start and Congestion Avoidance  
- RFC 5681 §3.2 — Fast Retransmit / Fast Recovery  
- 实现代码：[src/tcp_input.c](../src/tcp_input.c)、[src/tcp_output.c](../src/tcp_output.c)
