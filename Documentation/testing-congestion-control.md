# 拥塞控制机制测试指导

本文档覆盖 RFC 5681 慢启动、拥塞避免、快速重传、快速恢复四个机制的测试方法，对应提交 `3d73c87` 和 `acdcfa9`。

---

## 测试环境准备

### 构建

```bash
# 带调试符号和 TCP 调试日志
make clean
CFLAGS+="-DDEBUG_TCP" make debug
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

### 监控工具

两个终端分别运行，整个测试过程保持开启：

```bash
# 终端 1：抓包，观察 cwnd 推进与重传行为
sudo tcpdump -i tap0 -nn -S tcp

# 终端 2：lvl-ip 调试日志（DEBUG_TCP 已编入，直接看 stderr）
./lvl-ip 2>&1 | grep -E "cwnd|ssthresh|inflight|dupACK|recovery|RTO"
```

---

## 场景一：慢启动 (Slow Start)

### 目标

验证连接建立后 cwnd 从 IW=4×SMSS(=4×536=2144 B) 开始，每收到一个 ACK 按 `min(acked, SMSS)` 增长，直至触及 ssthresh。

### 步骤

```bash
# 1. 启动 HTTP 服务端（host 侧）
python2.7 -m SimpleHTTPServer 8010 &

# 2. 启动 lvl-ip
./lvl-ip &
sleep 2

# 3. 发起大文件下载（至少 10×SMSS 才能观察到慢启动爬升过程）
tools/level-ip apps/curl/curl 10.0.0.5 8010
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
- 传输完成，连接正常关闭，无挂起。

---

## 场景二：拥塞避免 (Congestion Avoidance)

### 目标

在人为触发 RTO 后，ssthresh 被压低，cwnd 从 1×SMSS 重新慢启动，越过 ssthresh 后自动切换拥塞避免模式（每 cwnd 字节 ACK 才增 1×SMSS）。

### 步骤

```bash
# 1. 先对 tap0 加大延迟，迫使 RTO 触发（配合 loss 制造丢包后的 RTO）
sudo tc class add dev tap0 parent 1: classid 1:11 htb rate 100mbit
sudo tc filter add dev tap0 parent 1: protocol ip prio 3 u32 \
    flowid 1:11 match ip dport 8011 0xffff
sudo tc qdisc add dev tap0 parent 1:11 netem loss 100% limit 10  # 先全丢

python2.7 -m SimpleHTTPServer 8011 &
./lvl-ip &
sleep 2
tools/level-ip apps/curl/curl 10.0.0.5 8011 &    # 会 RTO

sleep 3
# 2. 恢复网络，让 RTO 后的传输完成
sudo tc qdisc change dev tap0 parent 1:11 netem loss 0%
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
- `cwnd` 从 536 开始线性（慢启动阶段指数）增长。
- 切换拥塞避免后，每轮 RTT 内 `cwnd` 增量 ≤ 536 字节。

---

## 场景三：快速重传 (Fast Retransmit)

### 目标

使用 `netem duplicate` 或 `netem reorder` 制造 3 个重复 ACK，触发 `tcp_fast_retransmit`，验证不等 RTO 即重传丢失段。

### 步骤

```bash
# 制造 25% 丢包（使对端产生足够多乱序/重复 ACK）
sudo tc class add dev tap0 parent 1: classid 1:12 htb rate 100mbit
sudo tc filter add dev tap0 parent 1: protocol ip prio 2 u32 \
    flowid 1:12 match ip dport 8012 0xffff
sudo tc qdisc add dev tap0 parent 1:12 netem loss 25%

python2.7 -m SimpleHTTPServer 8012 &
./lvl-ip &
sleep 2
tools/level-ip apps/curl/curl 10.0.0.5 8012
```

也可直接复用现有测试套件：

```bash
cd tests && ./suites/tcp/suite-packet-loss
```

### 观测要点

tcpdump 中查找：

```
# 快速重传特征：同一 seq 在 RTO 超时前第二次出现
IP 10.0.0.4.XXXX > 10.0.0.5.8012: Flags [P.], seq S:E  ← 原始
IP 10.0.0.5.8012 > 10.0.0.4.XXXX: Flags [.], ack S     ← dupACK ×3
IP 10.0.0.4.XXXX > 10.0.0.5.8012: Flags [P.], seq S:E  ← 快速重传
```

lvl-ip 日志关键字：`dupacks=3`、`fast_retransmit`、`enter_recovery`。

### 验收标准

- 出现 3 个重复 ACK 后，重传在 RTO 超时前（< `rto` 毫秒）发生。
- `ssthresh` 被设为 `max(inflight/2, 1072)`。
- `cwnd` 被 inflate 到 `ssthresh + 3×536`。
- 传输最终成功完成（无挂起）。

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

验证 RTO 超时路径（`src/tcp_output.c:490`）正确更新 `ssthresh` 和 `cwnd`。

### 步骤

```bash
# 100% 丢包，强制 RTO（默认初始 RTO ~1s，backoff 指数退避）
sudo tc qdisc add dev tap0 parent 1:13 netem loss 100%
# ... 启动传输后等待约 3 秒 ...
# 恢复后观察 ssthresh/cwnd 恢复情况
```

### 验收标准

- RTO 触发时：`ssthresh = max(inflight/2, 1072)`，`cwnd = 536`。
- backoff 指数增长（1s → 2s → 4s …），最终达 `TCP_USER_TIMEOUT`(180s) 断开。
- 若网络恢复，cwnd 从 536 重新慢启动，传输继续完成。

---

## 现有测试套件使用方式

```bash
# 正常环境（验证基线）
cd tests && ./suites/tcp/suite-curl

# 25% 丢包（触发快速重传/恢复）
./suites/tcp/suite-packet-loss

# 2000ms 延迟（触发 RTO 和 backoff）
./suites/tcp/suite-packet-delay

# 50% 重复包（触发 dupACK，间接测试 Fast Retransmit）
./suites/tcp/suite-packet-duplication
```

全部套件一次运行：

```bash
make test
```

---

## 已知局限

| 限制 | 影响 |
|------|------|
| IW 固定为 4×SMSS，未按 RFC 5681 §3.1 分档 | 慢启动初始窗口偏大，但不影响后续行为验证 |
| 同一段多次 RTO 重传不抑制 ssthresh 二次缩减 | 极端丢包下 ssthresh 可能被压至最小值 |
| 无 SACK，Fast Recovery 仅重传队首段 | 多段同时丢包时恢复效率低 |
| TCP 接收窗口固定 512 字节 | 会限制 cwnd 增长上限（`min(cwnd, rwnd)` 受 rwnd 约束） |

---

## 参考

- RFC 5681 §3.1 — Slow Start and Congestion Avoidance  
- RFC 5681 §3.2 — Fast Retransmit / Fast Recovery  
- 实现代码：[src/tcp_input.c](../src/tcp_input.c)、[src/tcp_output.c](../src/tcp_output.c)
