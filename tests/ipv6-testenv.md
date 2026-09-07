# IPv6 测试环境与问题跟踪（tap0 ↔ enp4s0）

> 状态：2026-09-03 实验结论固化。**网络机制已全链路验证通过**，端到端尚未握手成功，
> 卡在 daemon 自身的 3 个缺陷（见 §4 遗留缺陷清单，后续跟踪交接）。

## 1. 目标与结论摘要

目标：栈内进程（经 `./tools/level-ip` 运行）通过 tap0 与监听 enp4s0 的宿主进程实现 IPv6 通信。

| 方案 | 结论 |
| --- | --- |
| A. link-local 直连（fe80::） | **机制性死结**，栈侧路由策略问题，详见 §3 |
| B. ULA 全局地址（fd00::/8） | **机制全通**（NS→NA→SYN→回程NDP→SYN-ACK 全有抓包证据），差 daemon 缺陷修复，详见 §4 |

IPv4 能同机互通而 link-local 不能的根本差异：IPv4 的目的地址是本机地址（如 192.168.40.47），
内核 local 表直接本地交付，无 scope 限制；IPv6 link-local 有 scope（接口）语义，内核 NS 的
strict 检查要求 target 在接收接口上，且 bind link-local 必须带 `%zone`（导致 sk_bound_dev_if
与入接口不一致）。ULA 全局地址无 scope 问题，机制与 IPv4 完全对称。

## 2. 测试环境搭建步骤（可复现）

前提：sudo 免密；`tcpdump` 在当前 shell 环境会挂起，用 `tests/sniff_tap0.py` 替代。

```bash
# 1. 启动 daemon（注意：daemon 有崩溃缺陷，可能需要重启，见 §4）
cd ~/code/study/level-ip
sudo setsid nohup ./lvl-ip > /tmp/lvl-ip.log 2>&1 < /dev/null &
sleep 4                              # 等 tap0 创建 + 栈 LL 地址 DAD

# 2. tap0 内核侧配 ULA 网关地址（模拟路由器）
sudo ip -6 addr add fd00:1234::1/64 dev tap0

# 3. 发 RA，栈通过 SLAAC 拿全局地址 + 默认路由
sudo python3 tests/send_ra.py
sleep 3                              # 等栈 DAD（日志出现 2 次 "DAD passed"）
# 验证：grep -E "DAD passed|default route" /tmp/lvl-ip.log
# 栈全局地址 = fd00:1234::20c:29ff:fe6d:5025（EUI-64，随栈 MAC）

# 4. enp4s0 侧配 ULA 地址 + 起监听（宿主侧）
sudo ip -6 addr add fd00:40::47/64 dev enp4s0
sleep 2                              # 等内核 DAD（tentative 期间 bind 会失败）
python3 -m http.server 8090 --bind fd00:40::47 &

# 5. 抓包（可选）+ 客户端测试
sudo python3 tests/sniff_tap0.py 8 &
timeout 8 ./tools/level-ip ./apps/curl-v6/curl-v6 fd00:40::47 8090
```

关键事实：tap0 每次重建后内核会随机新 MAC → 新内核 LL → RA 源变化，需重发 RA。
当前 tap0 内核侧 LL = fe80::b47b:adff:fee6:1486（RA 源 = 默认路由网关）。

## 3. 实验 A：link-local 直连死结（方案否决依据）

现象：`curl-v6 fe80::be8b:4f5:8302:f5de 8080`（enp4s0 的 LL）→ SYN 无限重传，每次卡在
"ndp send NS"，内核永不回 NA。

根因链（代码位置见 §5）：

1. [src/ipv6_addrconf.c:349](../src/ipv6_addrconf.c#L349) addrconf 时加了 `fe80::/10` on-link
   路由（无网关）。
2. route6_lookup 是最长前缀匹配：`fe80::/10`（prefix 10）胜过默认路由 `::/0`（prefix 0）
   → 所有 fe80:: 目的（**包括 enp4s0 上的 LL**）被判定为 tap0 本链路直连。
3. [src/dst.c:62-66](../src/dst.c#L62-L66) 非 RT_GATEWAY 分支 → next_hop = 最终目的地址
   → 在 tap0 链路上解析 enp4s0 的 LL 地址。
4. 抓包实锤：NS 的 target = fe80::be8b...（最终目的）而非网关 fe80::b47b...。
5. 内核 strict 检查（target 必须在接收接口 tap0 上）拒绝 → 7 次 NS 全部无响应 → 永无 NA。

结论：除非内核 proxy NDP 或改栈路由逻辑（如去掉 fe80::/10 路由、或 LL 目的强制走网关），
link-local 跨链路在栈内无解。ULA 方案（实验 B）绕开此问题，是推荐路径。

## 4. 实验 B：ULA 方案机制验证 + 遗留缺陷清单

### 4.1 机制证据链（2026-09-03 抓包，tests/sniff_tap0.py 输出）

| 时间 | 帧 | 含义 |
| --- | --- | --- |
| 880.7 | NS target=fe80::b47b（网关LL） | ULA 走默认路由，NDP 解析对象正确 ✓ |
| 880.7 | NA TLLA=b6:7b:ad:e6:14:86 | 内核应答网关 MAC（daemon 日志 `entry -> REACHABLE`）✓ |
| 880.7~882.2 | 3× SYN (nexthdr=6) | SYN 正确发出，daddr=fd00:40::47 ✓ |
| 885.8 | 内核 NS target=fd00:1234::20c（栈地址） | 内核收到 SYN，解析回程邻居 ✓ |
| 885.8 | 栈 NA TLLA=00:0c:29:6d:50:25 | 栈响应回程 NDP（日志 "sending NA reply"）✓ |
| 888.3 | IPv6 nexthdr=6 len=74 | **内核发出的 SYN-ACK**（74=14eth+40ip6+20tcp）✓ |

去程路由/选源/校验和/NDP、回程 NDP 全部正确。未握手成功的原因全部落在 daemon 缺陷：

### 4.2 遗留缺陷（后续跟踪项，按优先级）

| # | 缺陷 | 证据 | 优先级 |
| --- | --- | --- | --- |
| D1 | **堆内存损坏**：`double free or corruption (out)` abort；另有 2 次 `segfault in libc`（printf 内部踩雷，同根因）。触发链疑似 tap0 上的 mDNS 噪声包处理路径（IPv4 UDP sport 5353 走 "No UDP socket" 丢弃路径 / IPv6 UDP 走 "UDP-v6 handler not yet implemented" 丢弃路径之后的 skb free） | /tmp/lvl-ip.log 尾部 + `sudo dmesg \| grep lvl-ip`（3 次崩溃记录） | **P0**：不定时杀死 daemon，一切测试的前提 |
| D2 | **888.3 的 SYN-ACK 帧未被栈处理**：日志无对应 `ipv6 in`。需区分是 D1 崩溃时序遮蔽，还是 ipv6_input 真丢帧 | /tmp/lvl-ip.log（只到 mDNS 处理即崩） | P1：D1 修复后复测 |
| D3 | **IPC 响应错乱**：`ERR: IPC msg response expected type 2, actual type 0, pid 0`，curl-v6 exit=141（SIGPIPE） | 第二轮重跑 | P1：可能与 D1 同源 |
| D4 | **时序余量小**：内核回程邻居解析 ~5s 后才发 SYN-ACK，curl 8s timeout 逼近；栈 SYN 重传只观察到 boff 0-2 | 抓包时间戳 | P2：D1/D2 修复后观察是否仍存在 |

定位建议：`make debug` 加 `-fsanitize=address` 重编（当前只有 tsan，抓不到 use-after-free
之外的堆越界；ASan 能在第一次越界处停下而不是崩溃远处）。重点查 UDP 丢弃路径的
`free_skb` / skb ownership。

### 4.3 备注

- git 工作区中 `src/icmpv6.c` 有 21 行未提交修改（+21/-3），与本文档实验无关，跟踪时注意区分。
- /tmp 下易失文件已固化：抓包工具 = [tests/sniff_tap0.py](sniff_tap0.py)（AF_PACKET，参数=持续秒数）。

## 5. 相关代码位置索引

| 位置 | 职责（本实验涉及） |
| --- | --- |
| [src/ipv6_addrconf.c:349](../src/ipv6_addrconf.c#L349) | fe80::/10 on-link 路由（实验 A 根因） |
| [src/dst.c:43-87](../src/dst.c#L43-L87) | dst6_neigh_output：网关/直连 next_hop 选择 + NDP |
| [src/tcp_output.c:140-186](../src/tcp_output.c#L140-L186) | tcp_transmit_skb IPv6 路径：route6_lookup → 选源 → 校验和 → ipv6_dbg（:184，崩溃现场之一） |
| [src/ndp.c:235](../src/ndp.c#L235) | ndp_send_ns |
| [src/ndp.c:343-384](../src/ndp.c#L343-L384) | ndp_ra_process：SLAAC + 默认路由 |
| [src/ndp.c:417-452](../src/ndp.c#L417-L452) | NA 接收 → REACHABLE |
| [src/ndp.c:520-525](../src/ndp.c#L520-L525) | NS 接收 → NA 响应（回程 NDP） |
| [src/ipv6_input.c:51](../src/ipv6_input.c#L51) | ipv6 in 日志（D2 排查入口） |
| [tests/send_ra.py](send_ra.py) | RA 发送（源=tap0 内核 LL，前缀 fd00:1234::/64） |
| [tests/sniff_tap0.py](sniff_tap0.py) | AF_PACKET 抓包（tcpdump 替代） |
| [tests/lib/g5-tcp6.sh](lib/g5-tcp6.sh) | G5 TDD 红灯测试（当前仍用 fe80::1，待 ULA 方案转正后改造） |
