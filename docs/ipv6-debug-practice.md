# IPv6 TCP 发送路径 Debug 练习

> 当前分支: `ipv6-debug-practice`
>
> 两个 bug 已被故意引入到 `src/tcp_output.c` 中。
> 你的任务是找到并修复它们，然后 `git commit --amend` 到你的修复提交中。

---

## 环境准备

```bash
# 确保在正确的分支上
git checkout ipv6-debug-practice

# 编译
make clean && make debug && make apps
```

## 复现步骤

```bash
# 启动协议栈
sudo setcap cap_setpcap,cap_net_admin=ep lvl-ip
sudo ./lvl-ip > /tmp/lvl-ip.log 2>&1 &
sleep 4

# 在主机端启动 IPv6 监听器
nc -6 -l fe80::1%tap0 8080 &

# 尝试 TCP/IPv6 连接
./tools/level-ip ./apps/curl-v6/curl-v6 fe80::1 8080

# 检查 daemon 是否存活
sudo kill -0 $(pgrep lvl-ip) && echo "alive" || echo "CRASHED"

# 查看 daemon 日志
tail -30 /tmp/lvl-ip.log

# 清理
sudo kill $(pgrep lvl-ip) $(pgrep nc) 2>/dev/null
```

---

## Bug #1: 缓冲区下溢 → `munmap_chunk(): invalid pointer`

### 症状
- daemon 在发送第一个 TCP/IPv6 SYN 时**立即崩溃**
- 日志最后一条: `munmap_chunk(): invalid pointer` 或 `Aborted (core dumped)`
- GDB 会显示崩溃在 `free_skb()` → `free(skb->head)` 中

### 提示
1. 看 `tcp_alloc_skb()` 函数。它预留了多少字节的 IP 头空间？
2. IPv4 头是 20 字节（`IP_HDR_LEN`），IPv6 头是 **40 字节**（`IPV6_HDR_LEN`）
3. 当 `ipv6_output()` 或 TCP 路径 push 40 字节 IPv6 头时，如果只预留了 20 字节头空间...
4. `skb_push()` 会把 `skb->data` 移到 `skb->head` **之前**，写入堆元数据

### 修复方向
让 `tcp_alloc_skb()` 根据 `family` 参数选择正确的 IP 头长度。

### 验证
修复后 daemon 不再崩溃，G5 client connect 测试不再触发 `munmap_chunk`。

---

## Bug #2: TCP 校验和使用错误的源地址

### 症状
- daemon 不崩溃了（Bug #1 已修），但 SYN 发出去后**主机不回 SYN-ACK**
- daemon 日志显示 SYN 被重复发送（重传: `boff 0`, `boff 1`, `boff 2`...）
- 用 tcpdump 可以看到 SYN 包到达主机，但主机静默丢弃

### 提示
1. 看 `tcp_transmit_skb()` 的 IPv6 分支
2. `tcp_v6_connect()` 将 `sk->saddr.v6` 设为 `::`（全零），期望后续自动选择
3. `tcp_v6_checksum()` 使用 `sk->saddr.v6` 构建伪头部计算校验和
4. 源地址选择（从 netdev 取真实地址）发生在校验和计算**之后**
5. 最终发出的 IPv6 头中源地址是真实的（如 `fd00:1234::...`），但 TCP 校验和是按 `::` 算的
6. 主机收到包后，用真实源地址重算校验和 → 不匹配 → 静默丢弃

### 修复方向
在计算 TCP 校验和**之前**，先完成源地址解析（route lookup → netdev addr selection）。

### 验证
修复后，tcpdump 可以看到 SYN-ACK 回来，三次握手完成。

---

## 工作流程

```bash
# 1. 修改代码修复 bug
vim src/tcp_output.c

# 2. 编译验证
make debug 2>&1 | tail -5

# 3. 运行测试
sudo make test

# 4. 提交 (amend 到上一个 commit)
git add src/tcp_output.c
git commit --amend
```

## 关键数据结构参考

```
skb 缓冲区布局 (发送前, IPv4):
  [ETH 14][IPv4 20][TCP 20+][payload...]
  ^head             ^data

skb 缓冲区布局 (发送前, IPv6):
  [ETH 14][IPv6 40][TCP 20+][payload...]
  ^head             ^data
          ^^^^^^^^
          这 40 字节是关键！
```

```c
// tcp_alloc_skb 当前代码 (有 bug):
int reserved = ETH_HDR_LEN + IP_HDR_LEN + TCP_HDR_LEN + optlen + size;
//                              ^^^^^^^^^^
//                              永远是 20 字节，IPv6 时需要 40！

// tcp_transmit_skb IPv6 路径 (有 bug):
thdr->csum = tcp_v6_checksum(skb, &sk->saddr.v6, &sk->daddr.v6);
//                                  ^^^^^^^^^^^^
//                                  此时还是 :: (全零)！

// ... 之后才解析源地址:
if (ipv6_addr_is_unspecified(&sk->saddr.v6)) {
    memcpy(&sk->saddr.v6, &skb->dev->addr6_global, ...);
}
// 太晚了！校验和已经算完了
```

## 答案验证

修复完成后，运行 `sudo make test` 应该看到:
- G5 client connect: 不再 crash (可能仍然 fail — 主机环境差异)
- G1-G4: 全部 PASS (不受影响)
