# Level-IP 功能扩展开发指导文档

## 📚 文档说明

本文档为在 Level-IP 基础上实现 **ICMPv6**、**UDP** 协议以及**完善 TCP** 协议提供开发指导。

**核心原则：** 授人以渔，不授人以鱼
- 提供设计思路和架构指导
- 指出关键难点和解决思路
- 不直接给出完整代码实现

**适用人群：** 网络编程新手，具有 C 语言基础

---

## 📖 目录

1. [前置知识与学习资源](#1-前置知识与学习资源)
2. [开发环境准备](#2-开发环境准备)
3. [实现优先级与路线图](#3-实现优先级与路线图)
4. [UDP 协议实现指导](#4-udp-协议实现指导)
5. [ICMPv6 协议实现指导](#5-icmpv6-协议实现指导)
6. [TCP 协议完善指导](#6-tcp-协议完善指导)
7. [测试方法与验证](#7-测试方法与验证)
8. [调试技巧与工具](#8-调试技巧与工具)

---

## 1. 前置知识与学习资源

### 1.1 必读 RFC 文档

#### UDP 相关
- **RFC 768**: User Datagram Protocol (UDP)
  - 🔗 https://www.rfc-editor.org/rfc/rfc768.html
  - 📄 非常短（只有 3 页），是最简单的传输层协议
  - ⏱️ 阅读时间：30 分钟
  - 💡 重点：第 1 节（格式）、第 3 节（用户接口）

#### ICMPv6 相关
- **RFC 4443**: Internet Control Message Protocol (ICMPv6) for IPv6
  - 🔗 https://www.rfc-editor.org/rfc/rfc4443.html
  - 📄 定义了 ICMPv6 基本消息格式
  - ⏱️ 阅读时间：2 小时
  - 💡 重点：第 2 节（消息格式）、第 4 节（消息处理规则）

- **RFC 4861**: Neighbor Discovery for IP version 6 (NDP)
  - 🔗 https://www.rfc-editor.org/rfc/rfc4861.html
  - 📄 IPv6 的邻居发现协议（类似 IPv4 的 ARP）
  - ⏱️ 阅读时间：3 小时
  - 💡 重点：第 4 节（邻居发现消息）、第 7 节（邻居缓存）

- **RFC 8200**: Internet Protocol, Version 6 (IPv6) Specification
  - 🔗 https://www.rfc-editor.org/rfc/rfc8200.html
  - 📄 IPv6 基本规范
  - ⏱️ 阅读时间：2 小时
  - 💡 重点：第 3 节（IPv6 头部格式）

#### TCP 完善相关
- **RFC 793**: Transmission Control Protocol (TCP)
  - 🔗 https://www.rfc-editor.org/rfc/rfc793.html
  - 📄 TCP 基本规范（Level-IP 已部分实现）
  - 💡 重点：第 3.9 节（事件处理）- 服务端相关

- **RFC 5681**: TCP Congestion Control
  - 🔗 https://www.rfc-editor.org/rfc/rfc5681.html
  - 📄 TCP 拥塞控制算法
  - ⏱️ 阅读时间：3 小时
  - 💡 重点：第 3 节（拥塞控制算法）

- **RFC 7323**: TCP Extensions for High Performance
  - 🔗 https://www.rfc-editor.org/rfc/rfc7323.html
  - 📄 TCP 窗口缩放、时间戳等扩展
  - ⏱️ 阅读时间：2 小时

- **RFC 2018**: TCP Selective Acknowledgment Options (SACK)
  - 🔗 https://www.rfc-editor.org/rfc/rfc2018.html
  - 📄 TCP 选择性确认（Level-IP 已部分实现）
  - ⏱️ 阅读时间：1.5 小时

### 1.2 推荐学习资源

#### 在线教程
- **Beej's Guide to Network Programming**
  - 🔗 https://beej.us/guide/bgnet/html/
  - 📄 经典的网络编程入门教程，讲解 Socket API

- **The TCP/IP Guide**
  - 🔗 http://www.tcpipguide.com/
  - 📄 全面的 TCP/IP 协议讲解

#### 书籍
- **《TCP/IP 详解 卷1：协议》** - W. Richard Stevens
  - 📖 TCP/IP 协议的经典教材
  - 重点章节：第 11 章（UDP）、第 17-24 章（TCP）

- **《Linux 内核源码剖析：TCP/IP 实现》** - 樊东东
  - 📖 深入理解 Linux TCP/IP 实现

### 1.3 参考实现

#### Linux 内核源码

```bash
# 克隆 Linux 内核（可选浅克隆节省空间）
git clone --depth 1 https://github.com/torvalds/linux.git

# 关键文件位置
linux/net/ipv4/udp.c          # UDP 实现
linux/net/ipv6/icmp.c         # ICMPv6 实现
linux/net/ipv6/ndisc.c        # 邻居发现协议
linux/net/ipv4/tcp.c          # TCP 核心
linux/net/ipv4/tcp_input.c    # TCP 输入处理
linux/net/ipv4/tcp_output.c   # TCP 输出处理
linux/net/ipv4/tcp_cong.c     # TCP 拥塞控制
```

#### 其他开源实现
- **lwIP** (Lightweight IP): https://savannah.nongnu.org/projects/lwip/
  - 精简的 TCP/IP 栈，代码简洁易读
- **picoTCP**: https://github.com/tass-belgium/picotcp
  - 另一个教学用的 TCP/IP 实现

---

## 2. 开发环境准备

### 2.1 必需工具安装

```bash
# 基础编译工具
sudo apt-get install build-essential gcc make git

# 调试工具
sudo apt-get install gdb valgrind

# 网络工具
sudo apt-get install tcpdump wireshark iproute2 iputils-ping net-tools

# IPv6 工具
sudo apt-get install ndisc6  # 包含 ndisc6, rdisc6 等工具

# 性能分析
sudo apt-get install linux-tools-common linux-tools-generic strace
```

### 2.2 IPv6 环境配置

```bash
# 检查 IPv6 是否启用
ip -6 addr show

# 启用 IPv6（如果被禁用）
sudo sysctl -w net.ipv6.conf.all.disable_ipv6=0
sudo sysctl -w net.ipv6.conf.default.disable_ipv6=0

# 永久启用
echo "net.ipv6.conf.all.disable_ipv6=0" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p

# 验证
ping6 ::1  # 应该能 ping 通
```

### 2.3 Wireshark 权限配置

```bash
# 允许非 root 用户抓包
sudo dpkg-reconfigure wireshark-common
# 选择 "Yes" 允许非超级用户捕获数据包

# 将当前用户加入 wireshark 组
sudo usermod -a -G wireshark $USER

# 重新登录生效
```

---

## 3. 实现优先级与路线图

### 3.1 总体实现顺序

```
阶段 1: UDP 协议（基础，22 小时）
    ↓
阶段 2: TCP 服务端（核心，44 小时）
    ↓
阶段 3: ICMPv6 基础（扩展，24 小时）
    ↓
阶段 4: TCP 优化（进阶，60 小时）
    ↓
阶段 5: ICMPv6 NDP（高级，40 小时）
```

### 3.2 详细任务分解与时间估算

| 优先级 | 任务 | 难度 | 预估时间 | 累计 |
|--------|------|------|----------|------|
| **阶段 1: UDP 协议** |
| P0.1 | UDP 数据结构设计 | ⭐ | 2h | 2h |
| P0.2 | UDP 接收流程实现 | ⭐⭐ | 4h | 6h |
| P0.3 | UDP 发送流程实现 | ⭐⭐ | 4h | 10h |
| P0.4 | UDP Socket API 集成 | ⭐⭐⭐ | 6h | 16h |
| P0.5 | UDP 校验和实现 | ⭐ | 2h | 18h |
| P0.6 | UDP 测试与验证 | ⭐⭐ | 4h | 22h |
| **阶段 2: TCP 服务端** |
| P1.1 | bind() 实现 | ⭐⭐ | 4h | 26h |
| P1.2 | listen() 实现 | ⭐⭐ | 4h | 30h |
| P1.3 | accept() 实现 | ⭐⭐⭐⭐ | 12h | 42h |
| P1.4 | 连接队列管理 | ⭐⭐⭐ | 8h | 50h |
| P1.5 | 被动打开状态机 | ⭐⭐⭐⭐ | 10h | 60h |
| P1.6 | 服务端测试 | ⭐⭐⭐ | 6h | 66h |
| **阶段 3: ICMPv6 基础** |
| P2.1 | IPv6 基础结构 | ⭐⭐ | 4h | 70h |
| P2.2 | IPv6 接收处理 | ⭐⭐⭐ | 6h | 76h |
| P2.3 | ICMPv6 结构定义 | ⭐⭐ | 3h | 79h |
| P2.4 | Echo Request/Reply | ⭐⭐ | 5h | 84h |
| P2.5 | ICMPv6 错误消息 | ⭐⭐⭐ | 6h | 90h |
| P2.6 | ICMPv6 测试 | ⭐⭐ | 4h | 94h |
| **阶段 4: TCP 优化** |
| P3.1 | 滑动窗口实现 | ⭐⭐⭐⭐ | 10h | 104h |
| P3.2 | 慢启动算法 | ⭐⭐⭐⭐ | 12h | 116h |
| P3.3 | 拥塞避免算法 | ⭐⭐⭐⭐ | 10h | 126h |
| P3.4 | 快速重传 | ⭐⭐⭐⭐ | 8h | 134h |
| P3.5 | 快速恢复 | ⭐⭐⭐⭐ | 8h | 142h |
| P3.6 | SACK 完善 | ⭐⭐⭐⭐⭐ | 12h | 154h |
| P3.7 | 性能测试与调优 | ⭐⭐⭐ | 6h | 160h |
| **阶段 5: ICMPv6 NDP** |
| P4.1 | NDP 数据结构 | ⭐⭐⭐ | 6h | 166h |
| P4.2 | 邻居请求/通告 | ⭐⭐⭐⭐ | 10h | 176h |
| P4.3 | 路由器发现 | ⭐⭐⭐⭐ | 10h | 186h |
| P4.4 | 邻居缓存管理 | ⭐⭐⭐ | 8h | 194h |
| P4.5 | 重定向消息 | ⭐⭐⭐ | 6h | 200h |
| P4.6 | NDP 集成测试 | ⭐⭐⭐ | 6h | 206h |

**总计：约 206 小时**（按每周 10 小时计算，约需 **5 个月**）

### 3.3 里程碑规划

#### 里程碑 1: UDP 可用（第 1 个月）
- ✅ 可以发送和接收 UDP 数据包
- ✅ 通过 `nc -u` 测试通过
- ✅ 支持 bind()、sendto()、recvfrom()

#### 里程碑 2: TCP 服务端（第 2-3 个月）
- ✅ 可以创建 TCP 服务器
- ✅ 通过 `nc` 连接测试通过
- ✅ 支持多个并发连接

#### 里程碑 3: ICMPv6 基础（第 3-4 个月）
- ✅ 支持 IPv6 ping
- ✅ 通过 `ping6` 测试
- ✅ 支持基本错误消息

#### 里程碑 4: TCP 优化（第 4-5 个月）
- ✅ 拥塞控制实现
- ✅ 吞吐量达到合理水平
- ✅ 通过 iperf3 测试

#### 里程碑 5: ICMPv6 NDP（第 5 个月）
- ✅ 邻居发现协议
- ✅ 自动地址配置
- ✅ 完整的 IPv6 支持

---

## 4. UDP 协议实现指导

### 4.1 UDP 协议概述

**特点：**
- 无连接：不需要握手
- 不可靠：不保证送达
- 无序：数据包可能乱序
- 简单：头部只有 8 字节

**UDP 头部格式：**
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          源端口 (16)           |        目标端口 (16)          |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|          长度 (16)             |        校验和 (16)            |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                            数据                                |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

### 4.2 设计思路

#### 4.2.1 数据结构设计（P0.1 - 2小时）

**需要定义：**
1. UDP 头部结构体
2. UDP Socket 结构体（可以继承 `struct sock`）
3. UDP 操作函数表（类似 TCP 的 `net_ops`）

**参考：**
- 查看 [include/tcp.h](include/tcp.h) 了解如何定义协议头
- 查看 [include/sock.h](include/sock.h) 了解如何扩展 socket 结构

**设计要点：**
- UDP 头部使用 `__attribute__((packed))` 确保内存对齐
- 考虑字节序问题（网络序 vs 主机序）
- UDP Socket 需要哪些特殊字段？（提示：UDP 比 TCP 简单很多）

#### 4.2.2 接收流程（P0.2 - 4小时）

**处理步骤：**
1. 从 IP 层接收数据包
2. 解析 UDP 头部
3. 验证校验和（可选）
4. 查找对应的 Socket
5. 放入接收队列
6. 唤醒等待的接收者

**关键问题：**

❓ **Q1: 如何将 UDP 集成到 IP 层？**
- 提示：查看 [src/ip_input.c](src/ip_input.c) 中的 `ip_rcv()` 函数
- 提示：参考 TCP 如何集成（`case IP_TCP`）
- 需要在哪里添加 `case IP_UDP`？

❓ **Q2: 如何查找对应的 Socket？**
- 提示：查看 [src/socket.c](src/socket.c) 中的 `socket_lookup()` 函数
- UDP 只需要匹配目标端口（不像 TCP 需要四元组）
- 是否需要修改现有的查找函数？

❓ **Q3: 如何验证校验和？**
- UDP 校验和包含**伪头部**（源IP、目标IP、协议号、UDP长度）
- 参考 [src/tcp.c](src/tcp.c) 中的 `tcp_udp_checksum()` 函数
- 注意：UDP 校验和为 0 表示不进行校验

❓ **Q4: 收到的数据包如何存储？**
- 使用 `sk_buff` 结构
- 放入 `sk->receive_queue` 队列
- 参考 TCP 的 `skb_queue_tail()` 用法

#### 4.2.3 发送流程（P0.3 - 4小时）

**处理步骤：**
1. 从用户空间接收数据
2. 构造 UDP 头部
3. 计算校验和
4. 调用 IP 层发送

**关键问题：**

❓ **Q5: UDP 需要分片吗？**
- UDP 单个数据报最大 65507 字节（65535 - IP头20 - UDP头8）
- 如果超过 MTU，由 IP 层负责分片
- UDP 层需要检查长度限制吗？

❓ **Q6: 如何构造 UDP 头部？**
- 使用 `skb_push()` 在数据前添加头部
- 参考 [src/tcp_output.c](src/tcp_output.c) 中的头部构造
- 注意字节序转换（`htons`、`htonl`）

❓ **Q7: 发送时如何获取源地址？**
- 如果 Socket 已经 bind()，使用绑定的地址
- 如果没有 bind()，需要查找路由表确定源地址
- 参考 [src/route.c](src/route.c)

#### 4.2.4 Socket API 集成（P0.4 - 6小时）

**需要实现的函数：**
- `udp_bind()` - 绑定地址和端口
- `udp_sendto()` / `udp_sendmsg()` - 发送数据
- `udp_recvfrom()` / `udp_recvmsg()` - 接收数据
- `udp_close()` - 关闭 socket

**关键问题：**

❓ **Q8: bind() 需要做什么？**
- 保存本地地址和端口到 Socket 结构
- 检查端口是否已被占用？（可选）
- 是否支持绑定到 `INADDR_ANY` (0.0.0.0)？

❓ **Q9: sendto() 和 send() 的区别？**
- `sendto()` 可以指定目标地址（适合 UDP）
- `send()` 使用 socket 预设的目标地址（需要先 connect()）
- UDP 的 connect() 只是保存目标地址，不建立连接

❓ **Q10: recvfrom() 如何返回发送方地址？**
- 从 `sk_buff` 中提取源 IP 和源端口
- 填充到 `struct sockaddr_in` 结构
- 如何从 IP 头获取源地址？

❓ **Q11: 如何集成到 IPC 系统？**
- 参考 [src/ipc.c](src/ipc.c)
- 需要添加 `IPC_SENDTO`、`IPC_RECVFROM` 消息类型
- 需要修改 [tools/liblevelip.c](tools/liblevelip.c) 拦截 `sendto()`、`recvfrom()`

#### 4.2.5 校验和实现（P0.5 - 2小时）

**伪头部格式：**
```
 0                   1                   2                   3
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                       源 IP 地址                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                      目标 IP 地址                              |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|      零       |   协议 (17)   |         UDP 长度               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**关键问题：**

❓ **Q12: 如何计算校验和？**
- UDP 和 TCP 使用相同的校验和算法
- 可以重用 [src/tcp.c](src/tcp.c) 中的 `tcp_udp_checksum()` 函数
- 校验和计算包括：伪头部 + UDP头部 + 数据

❓ **Q13: 发送时校验和为 0 可以吗？**
- IPv4 中，UDP 校验和是可选的（0 表示不校验）
- IPv6 中，UDP 校验和是**必须**的
- 建议总是计算校验和

### 4.3 实现建议

**开发顺序：**
1. 先定义数据结构和头文件
2. 实现接收流程（更简单）
3. 实现发送流程
4. 集成 Socket API
5. 最后完善校验和

**编码技巧：**
- 先实现最简版本，能收发数据即可
- 然后逐步添加错误检查和边界情况处理
- 使用调试宏（`#ifdef DEBUG_UDP`）输出详细日志

**常见陷阱：**
- ⚠️ 字节序错误（忘记 `htons`/`ntohs`）
- ⚠️ 长度计算错误（是否包含头部？）
- ⚠️ 指针偏移错误（`skb->data` vs `skb->payload`）
- ⚠️ 内存泄漏（忘记 `free_skb()`）

### 4.4 测试方法

#### 测试 1: 基本接收测试

```bash
# 终端 1: 启动 lvl-ip
sudo ./lvl-ip

# 终端 2: 发送 UDP 数据
echo "test message" | nc -u 10.0.0.4 8888

# 终端 3: 抓包验证
sudo tcpdump -i tap0 udp port 8888 -n -vvv
```

**预期结果：**
- lvl-ip 应该收到数据包
- 调试输出显示 UDP 头部信息

#### 测试 2: 基本发送测试

```bash
# 使用 nc 作为服务器接收
nc -u -l 8888 &

# 编写简单的测试程序发送数据
# （需要先实现 Socket API）

# 抓包验证
sudo tcpdump -i tap0 udp port 8888 -X
```

#### 测试 3: Echo 测试

```bash
# 实现一个简单的 UDP echo 服务器
# 收到数据后原样返回

# 使用 nc 测试
echo "hello" | nc -u 10.0.0.4 8888
```

### 4.5 调试技巧

**查看 UDP 头部：**
```bash
sudo tcpdump -i tap0 udp -n -vvv -X
```

**使用 Wireshark：**
- 过滤器：`udp.port == 8888`
- 查看校验和是否正确：右键 → Protocol Preferences → Validate checksum

**GDB 调试：**
```bash
gdb ./lvl-ip
(gdb) break udp_in
(gdb) run
# 发送 UDP 包触发断点
(gdb) print *uh   # 打印 UDP 头部
(gdb) print/x uh->csum  # 查看校验和
```

---

## 5. ICMPv6 协议实现指导

### 5.1 ICMPv6 协议概述

ICMPv6 在 IPv6 中的作用比 ICMPv4 更重要，集成了：
- 错误报告
- 诊断功能（ping6）
- **邻居发现协议**（替代 ARP）
- 多播监听发现

**ICMPv6 消息类型：**

| 类型 | 名称 | 用途 |
|------|------|------|
| 1 | Destination Unreachable | 目标不可达 |
| 2 | Packet Too Big | MTU 发现 |
| 3 | Time Exceeded | TTL 超时 |
| 128 | Echo Request | Ping 请求 |
| 129 | Echo Reply | Ping 应答 |
| 133 | Router Solicitation | 请求路由器信息 |
| 134 | Router Advertisement | 路由器通告 |
| 135 | Neighbor Solicitation | 邻居请求（类似 ARP 请求）|
| 136 | Neighbor Advertisement | 邻居通告（类似 ARP 应答）|

### 5.2 设计思路

#### 5.2.1 IPv6 基础结构（P2.1 - 4小时）

**IPv6 头部格式：**
```
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Version| Traffic Class |           Flow Label                  |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Payload Length        |  Next Header  |   Hop Limit   |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                                                               +
|                                                               |
+                         Source Address                        +
|                                                               |
+                                                               +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                                                               +
|                                                               |
+                      Destination Address                      +
|                                                               |
+                                                               +
|                                                               |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**关键问题：**

❓ **Q14: IPv6 地址如何表示？**
- 128 位（16 字节）
- 使用 `struct in6_addr` (标准库定义)
- 如何比较两个 IPv6 地址？

❓ **Q15: IPv6 头部有哪些特点？**
- 固定 40 字节（没有选项字段）
- 没有校验和（由上层协议负责）
- 没有分片字段（主机端分片）
- Next Header 字段类似 IPv4 的 Protocol

❓ **Q16: 如何集成到以太网层？**
- 以太网类型：`0x86DD`
- 修改 [src/netdev.c](src/netdev.c) 添加 `case ETH_P_IPV6`
- 参考 IPv4 的处理流程

#### 5.2.2 ICMPv6 结构定义（P2.3 - 3小时）

**ICMPv6 头部格式：**
```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|     Type      |     Code      |          Checksum             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                        Message Body                           |
|                     (根据类型不同而变化)                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
```

**关键问题：**

❓ **Q17: 不同类型的 ICMPv6 消息如何定义？**
- Echo Request/Reply 有 ID 和序列号
- Neighbor Solicitation/Advertisement 有目标地址和选项
- 是否定义多个结构体还是用联合体？

❓ **Q18: ICMPv6 选项如何处理？**
- NDP 消息包含 TLV 格式的选项
- 如何解析变长选项？
- 参考 [src/tcp_input.c](src/tcp_input.c) 中的 TCP 选项解析

#### 5.2.3 Echo Request/Reply 实现（P2.4 - 5小时）

这是最简单的 ICMPv6 消息，适合作为入门。

**处理流程：**
1. 接收 Echo Request
2. 验证校验和
3. 构造 Echo Reply（源和目标地址互换）
4. 发送 Reply

**关键问题：**

❓ **Q19: ICMPv6 校验和如何计算？**
- 类似 UDP，包含伪头部
- 伪头部：源地址 + 目标地址 + ICMPv6 长度 + Next Header (58)
- 注意：IPv6 地址是 128 位，需要正确处理

❓ **Q20: 如何复用收到的数据包？**
- Echo Reply 可以直接修改 Echo Request 的数据包
- 只需要修改：Type、源/目标地址、重新计算校验和
- 还是重新分配一个 skb？

❓ **Q21: 如何发送 IPv6 数据包？**
- 需要实现 `ipv6_output()` 函数
- 参考 [src/ip_output.c](src/ip_output.c) 的 IPv4 实现
- 如何查找 IPv6 路由？（初期可以硬编码）

#### 5.2.4 错误消息实现（P2.5 - 6小时）

ICMPv6 错误消息用于报告处理问题。

**常见错误类型：**
- Destination Unreachable (类型 1)
  - 无路由 (Code 0)
  - 端口不可达 (Code 4)
- Packet Too Big (类型 2)
- Time Exceeded (类型 3)

**关键问题：**

❓ **Q22: 何时发送 ICMPv6 错误？**
- UDP 收到数据但没有对应 Socket → Port Unreachable
- IPv6 包 Hop Limit = 0 → Time Exceeded
- 包太大无法转发 → Packet Too Big
- 在代码哪些位置触发？

❓ **Q23: 错误消息包含什么内容？**
- ICMPv6 头部
- 引起错误的原始 IPv6 包（尽可能多，但不超过 MTU）
- 如何提取原始包数据？

❓ **Q24: 错误消息需要限速吗？**
- 避免 ICMPv6 风暴
- RFC 4443 建议限制错误消息发送速率
- 如何实现简单的令牌桶算法？

### 5.3 邻居发现协议 (NDP)（阶段5 - 40小时）

**⚠️ 这是高级功能，建议最后实现**

NDP 替代了 IPv4 中的 ARP、ICMP Redirect 等功能。

#### 5.3.1 NDP 概述

**主要功能：**
1. 地址解析：IPv6 地址 → MAC 地址（替代 ARP）
2. 路由器发现：自动找到默认网关
3. 前缀发现：获取网络前缀，实现地址自动配置
4. 地址重复检测：DAD (Duplicate Address Detection)
5. 重定向：通知更好的下一跳

**关键问题：**

❓ **Q25: NDP 和 ARP 有什么区别？**
- NDP 使用 ICMPv6 消息（而不是单独的协议）
- NDP 使用多播而非广播
- NDP 更安全（支持 SeND - Secure Neighbor Discovery）

❓ **Q26: 如何实现邻居缓存？**
- 参考 [src/arp.c](src/arp.c) 的 ARP 缓存实现
- 邻居缓存包含：IPv6 地址、MAC 地址、状态、生存时间
- 状态：INCOMPLETE、REACHABLE、STALE、DELAY、PROBE

❓ **Q27: 如何处理邻居请求 (NS)？**
- 收到 NS，检查目标地址是否是本机
- 如果是，发送邻居通告 (NA)
- NA 包含本机的 MAC 地址（作为选项）

❓ **Q28: 多播地址如何处理？**
- 邻居请求发送到"请求节点多播地址" (Solicited-Node Multicast)
- 格式：`ff02::1:ffXX:XXXX`（最后 24 位来自目标地址）
- 如何计算和匹配多播地址？

### 5.4 实现建议

**分步实现：**
1. 先实现 IPv6 接收框架（不处理扩展头）
2. 实现 Echo Request/Reply（测试 IPv6 通路）
3. 实现基本错误消息
4. 最后实现 NDP（最复杂）

**简化方案：**
- 初期可以不实现路由器发现，手动配置地址
- 可以不实现 DAD，假设地址不冲突
- 可以不实现重定向

**参考实现：**
- Linux 内核：`linux/net/ipv6/ndisc.c`（约 2000 行）
- lwIP：`lwip/src/core/ipv6/nd6.c`（约 1500 行）

### 5.5 测试方法

#### 测试 1: IPv6 Ping

```bash
# 配置 TAP 设备 IPv6 地址
sudo ip -6 addr add fe80::1/64 dev tap0
sudo ip -6 addr add 2001:db8::4/64 dev tap0

# 测试本地链路地址
ping6 -c 3 fe80::1%tap0

# 测试全局地址
ping6 -c 3 2001:db8::4

# 抓包验证
sudo tcpdump -i tap0 icmp6 -n -vvv
```

#### 测试 2: 邻居发现

```bash
# 清空邻居缓存
sudo ip -6 neigh flush dev tap0

# 发送 ping，触发邻居发现
ping6 2001:db8::4

# 查看邻居表
ip -6 neigh show dev tap0

# 抓包查看 NS/NA 消息
sudo tcpdump -i tap0 'icmp6 and (ip6[40] == 135 or ip6[40] == 136)' -n -vvv
```

#### 测试 3: 使用 ndisc6 工具

```bash
# 安装 ndisc6
sudo apt-get install ndisc6

# 发送邻居请求
ndisc6 2001:db8::4 tap0

# 发送路由器请求
rdisc6 tap0
```

### 5.6 调试技巧

**Wireshark 过滤器：**
```
# 所有 ICMPv6
icmpv6

# Echo Request/Reply
icmpv6.type == 128 || icmpv6.type == 129

# 邻居发现
icmpv6.type == 135 || icmpv6.type == 136

# 路由器发现
icmpv6.type == 133 || icmpv6.type == 134
```

**查看 IPv6 地址格式：**
```c
// 打印 IPv6 地址的辅助函数
void print_ipv6_addr(struct in6_addr *addr) {
    printf("%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
           "%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
           addr->s6_addr[0], addr->s6_addr[1], ...);
}
```

---

## 6. TCP 协议完善指导

### 6.1 TCP 服务端实现（阶段2 - 44小时）

#### 6.1.1 核心概念

**被动打开 vs 主动打开：**
- 主动打开：客户端调用 `connect()`（Level-IP 已实现）
- 被动打开：服务端调用 `bind()` → `listen()` → `accept()`

**连接队列：**
- SYN 队列（半连接队列）：收到 SYN，发送 SYN+ACK，等待 ACK
- Accept 队列（全连接队列）：三次握手完成，等待 `accept()` 取出

#### 6.1.2 bind() 实现（P1.1 - 4小时）

**功能：** 将 Socket 绑定到本地地址和端口

**关键问题：**

❓ **Q29: bind() 需要做什么检查？**
- 端口号是否有效（0-65535）？
- 端口是否已被占用？
- 是否允许绑定到 `INADDR_ANY`？
- 是否允许端口复用（`SO_REUSEADDR`）？

❓ **Q30: 如何检查端口占用？**
- 遍历所有 Socket，检查是否有相同的端口
- 需要区分不同状态的 Socket 吗？
- LISTEN 状态的 Socket 如何处理？

❓ **Q31: 绑定到 0.0.0.0 是什么意思？**
- 表示监听所有网络接口
- 收到任何目标 IP 的连接都接受
- 如何在代码中处理？

#### 6.1.3 listen() 实现（P1.2 - 4小时）

**功能：** 将 Socket 设为监听状态，准备接受连接

**关键问题：**

❓ **Q32: listen() 的 backlog 参数是什么？**
- 最大等待连接数
- Linux 实际含义：Accept 队列的最大长度
- SYN 队列长度 = max(backlog * 1.5, 某个系统值)
- 如何在代码中使用？

❓ **Q33: LISTEN 状态需要什么数据结构？**
- SYN 队列：存储半连接的 Socket
- Accept 队列：存储全连接的 Socket
- 使用现有的 `sk_buff_head` 还是新的结构？

❓ **Q34: 从 CLOSE 到 LISTEN 状态转换？**
- 只有 CLOSE 状态才能 listen
- 需要初始化哪些字段？
- TCB (TCP Control Block) 需要初始化吗？

#### 6.1.4 accept() 实现（P1.3 - 12小时）⚠️**最难**

**功能：** 从 Accept 队列取出一个已建立的连接

**关键问题：**

❓ **Q35: accept() 的整体流程？**
1. 检查 Accept 队列是否为空
2. 如果空，阻塞等待（或返回 EAGAIN）
3. 从队列取出一个新连接
4. 创建新的 Socket 结构
5. 返回新的文件描述符

❓ **Q36: 如何阻塞等待新连接？**
- 使用等待队列 (`wait_sleep()`)
- 参考 [src/tcp.c](src/tcp.c) 中的 `tcp_wait_connect()`
- 何时唤醒？（收到第三次握手的 ACK 时）

❓ **Q37: 新连接的 Socket 如何创建？**
- 需要分配新的 `struct socket` 和 `struct sock`
- 新 Socket 的地址信息从哪里来？（从 SYN 包中）
- 新 Socket 的初始状态是什么？（ESTABLISHED）

❓ **Q38: 父子 Socket 的关系？**
- 监听 Socket（父）：负责接受连接
- 连接 Socket（子）：负责数据传输
- 如何关联它们？需要吗？

#### 6.1.5 被动打开状态机（P1.5 - 10小时）

**状态转换：**
```
LISTEN
  │ 收到 SYN
  ▼
SYN_RECEIVED
  │ 收到 ACK
  ▼
ESTABLISHED
```

**关键问题：**

❓ **Q39: 收到 SYN 包如何处理？**
1. 检查 SYN 队列是否已满
2. 创建新的 Socket（子 Socket）
3. 初始化子 Socket 的 TCB（irs、iss、rcv_nxt、snd_nxt）
4. 发送 SYN+ACK
5. 将子 Socket 加入 SYN 队列
6. 启动重传定时器（如果 ACK 丢失需要重传 SYN+ACK）

❓ **Q40: SYN_RECEIVED 状态收到 ACK？**
1. 验证 ACK 号是否正确
2. 将 Socket 从 SYN 队列移到 Accept 队列
3. 状态设为 ESTABLISHED
4. 停止重传定时器
5. 唤醒等待 `accept()` 的线程

❓ **Q41: 如何防止 SYN Flood 攻击？**
- 限制 SYN 队列长度
- 使用 SYN Cookie（高级，可选）
- 设置超时时间，清理长时间未完成的连接

#### 6.1.6 并发连接处理（P1.4 - 8小时）

**关键问题：**

❓ **Q42: 如何支持多个并发连接？**
- 每个连接都有独立的 Socket
- 所有连接 Socket 共享同一个监听端口
- 如何在查找时区分？（四元组匹配）

❓ **Q43: Socket 查找函数需要修改吗？**
- 现有的 `socket_lookup()` 可能需要增强
- LISTEN Socket 只需匹配本地端口
- ESTABLISHED Socket 需要匹配四元组（源IP、源端口、目标IP、目标端口）

### 6.2 TCP 窗口管理（阶段4 - P3.1 - 10小时）

#### 6.2.1 滑动窗口概念

**发送窗口：**
```
已发送已确认  已发送未确认    可发送     不可发送
    │          │             │          │
    ▼          ▼             ▼          ▼
────┴──────────┴─────────────┴──────────┴────>
  snd_una    snd_nxt    snd_una+snd_wnd

    ◄─────────────────────────►
          发送窗口
```

**接收窗口：**
```
已接收已读取  已接收未读取    可接收     超出窗口
    │          │             │          │
    ▼          ▼             ▼          ▼
────┴──────────┴─────────────┴──────────┴────>
            rcv_nxt     rcv_nxt+rcv_wnd

             ◄────────────────►
                 接收窗口
```

**关键问题：**

❓ **Q44: 发送窗口的大小如何确定？**
- `snd_wnd` = min(对方通告的接收窗口, 拥塞窗口 cwnd)
- 对方接收窗口从哪里获取？（TCP 头部的 Window 字段）
- 拥塞窗口如何计算？（见拥塞控制部分）

❓ **Q45: 何时可以发送数据？**
- 条件：`snd_nxt < snd_una + snd_wnd`
- 可发送字节数：`snd_una + snd_wnd - snd_nxt`
- 如果窗口为 0 怎么办？（零窗口探测）

❓ **Q46: 接收窗口如何通告给对方？**
- 每个发送的 TCP 包都包含 Window 字段
- 窗口大小 = 接收缓冲区剩余空间
- 如何计算剩余空间？（总大小 - 队列长度）

❓ **Q47: 窗口更新何时发生？**
- 收到 ACK 时更新发送窗口
- 应用程序读取数据后更新接收窗口
- 需要发送窗口更新包吗？

### 6.3 TCP 拥塞控制（阶段4 - P3.2-P3.5 - 38小时）⚠️**最复杂**

#### 6.3.1 基本概念

**拥塞控制目标：**
- 避免网络拥塞
- 公平地共享带宽
- 最大化吞吐量

**关键变量：**
- `cwnd`：拥塞窗口（由发送方维护）
- `ssthresh`：慢启动阈值
- `snd_wnd`：接收方通告的窗口
- 实际发送窗口 = min(cwnd, snd_wnd)

**四个算法：**
1. 慢启动 (Slow Start)
2. 拥塞避免 (Congestion Avoidance)
3. 快速重传 (Fast Retransmit)
4. 快速恢复 (Fast Recovery)

#### 6.3.2 慢启动（P3.2 - 12小时）

**算法思想：**
- 初始 cwnd = 1 MSS（保守）
- 每收到一个 ACK，cwnd += 1 MSS
- 指数增长：1 → 2 → 4 → 8 → 16 ...
- 当 cwnd >= ssthresh 时，进入拥塞避免

**关键问题：**

❓ **Q48: 初始 cwnd 和 ssthresh 如何设置？**
- RFC 5681: 初始 cwnd = 1-10 MSS（推荐 10）
- 初始 ssthresh = 对方接收窗口大小（或无穷大）
- Level-IP 应该设置多少？

❓ **Q49: 如何实现指数增长？**
- 每收到一个 ACK，增加 MSS
- 一个 RTT 内收到多个 ACK，cwnd 翻倍
- 需要记录每个包的 ACK 吗？

❓ **Q50: 何时退出慢启动？**
- 条件1: cwnd >= ssthresh
- 条件2: 发生丢包（超时或重复 ACK）
- 丢包时 ssthresh = cwnd / 2

#### 6.3.3 拥塞避免（P3.3 - 10小时）

**算法思想：**
- 线性增长：每个 RTT 增加 1 MSS
- 实现：每收到一个 ACK，cwnd += MSS * MSS / cwnd
- 避免过快增长导致拥塞

**关键问题：**

❓ **Q51: 如何区分慢启动和拥塞避免？**
- 使用状态变量或 flag
- 根据 cwnd 和 ssthresh 的关系判断

❓ **Q52: 拥塞避免持续多久？**
- 直到发生丢包
- 丢包后重新设置 ssthresh 和 cwnd

#### 6.3.4 快速重传（P3.4 - 8小时）

**算法思想：**
- 收到 3 个重复 ACK（4 个相同 ACK）
- 立即重传丢失的包，不等超时
- 快速恢复丢失

**关键问题：**

❓ **Q53: 如何检测重复 ACK？**
- 记录上一个 ACK 号
- 如果连续收到相同的 ACK，计数器加 1
- 计数器达到 3 时触发快速重传

❓ **Q54: 为什么是 3 个重复 ACK？**
- 1-2 个可能是乱序导致的
- 3 个说明很可能是丢包
- RFC 5681 的标准

❓ **Q55: 重传哪个包？**
- 重传 ACK 号对应的包（即丢失的包）
- 如何从发送队列中找到这个包？
- 使用序列号匹配

#### 6.3.5 快速恢复（P3.5 - 8小时）

**算法思想：**
- 快速重传后进入快速恢复
- cwnd = ssthresh + 3 MSS（膨胀窗口）
- 每收到一个重复 ACK，cwnd += 1 MSS
- 收到新的 ACK，退出快速恢复

**关键问题：**

❓ **Q56: 为什么要膨胀窗口？**
- 重复 ACK 表示网络中还有数据在传输
- 允许发送新数据，维持管道满载

❓ **Q57: 何时退出快速恢复？**
- 收到新的 ACK（确认了重传的包）
- cwnd = ssthresh
- 进入拥塞避免阶段

#### 6.3.6 超时重传

**与快速重传的区别：**
- 快速重传：收到重复 ACK（网络还在工作）
- 超时重传：RTO 超时（可能网络严重拥塞）

**关键问题：**

❓ **Q58: 超时后如何处理？**
- ssthresh = cwnd / 2
- cwnd = 1 MSS（回到慢启动）
- 重传最早的未确认包
- 指数退避 RTO

### 6.4 SACK 完善（P3.6 - 12小时）

Level-IP 已经部分实现了 SACK，但可能需要完善。

**SACK 的作用：**
- 告诉发送方哪些数据已收到（即使不连续）
- 发送方只重传真正丢失的包

**关键问题：**

❓ **Q59: 如何生成 SACK 选项？**
- 从乱序队列中提取已收到的段
- 构造 SACK 块（起始序列号、结束序列号）
- 最多包含 3-4 个 SACK 块（因为 TCP 选项空间有限）

❓ **Q60: 如何使用收到的 SACK 信息？**
- 解析对方发来的 SACK 选项
- 标记哪些包已确认（不需要重传）
- 重传未被 SACK 的包

### 6.5 实现建议

**开发顺序：**
1. 先实现服务端（bind、listen、accept）
2. 测试基本的服务端功能
3. 实现窗口管理
4. 实现拥塞控制算法（一个一个来）
5. 最后完善 SACK

**测试驱动：**
- 每实现一个功能，立即测试
- 使用 `nc`、`iperf3` 等工具验证
- 使用 `tc` 模拟网络延迟和丢包

**参考实现：**
- Linux 内核：`linux/net/ipv4/tcp_cong.c`（约 500 行）
- RFC 5681：详细的伪代码

### 6.6 测试方法

#### 测试 1: 基本服务端

```bash
# 编写简单的 echo 服务器（伪代码）
# sockfd = socket()
# bind(sockfd, 8080)
# listen(sockfd, 5)
# while (1) {
#   client = accept(sockfd)
#   read(client, buf)
#   write(client, buf)
#   close(client)
# }

# 使用 nc 连接
echo "hello" | nc 10.0.0.4 8080
```

#### 测试 2: 并发连接

```bash
# 同时发起多个连接
for i in {1..10}; do
    (echo "client $i" | nc 10.0.0.4 8080) &
done
wait
```

#### 测试 3: 拥塞控制

```bash
# 使用 tc 添加延迟和丢包
sudo tc qdisc add dev tap0 root netem delay 100ms loss 1%

# 使用 iperf3 测试吞吐量
iperf3 -s &  # 服务器（使用 level-ip 运行）
iperf3 -c 10.0.0.4 -t 30  # 客户端，测试 30 秒

# 观察 cwnd 变化（需要在代码中添加日志）

# 清除规则
sudo tc qdisc del dev tap0 root
```

#### 测试 4: 窗口管理

```bash
# 发送大量数据，观察窗口是否正确更新
dd if=/dev/zero bs=1M count=10 | nc 10.0.0.4 8080

# 使用 Wireshark 查看 TCP Window 字段变化
```

---

## 7. 测试方法与验证

### 7.1 单元测试

#### 使用 Check 框架

```bash
# 安装
sudo apt-get install check

# 编写测试（示例结构）
tests/
  ├── test_udp.c        # UDP 测试
  ├── test_tcp.c        # TCP 测试
  ├── test_icmpv6.c     # ICMPv6 测试
  └── Makefile
```

**测试内容：**
- 数据结构大小验证
- 校验和计算验证
- 字节序转换验证
- 边界条件测试

### 7.2 集成测试

#### 测试脚本模板

```bash
#!/bin/bash
# tests/integration/test_udp.sh

set -e

echo "=== UDP Integration Test ==="

# 启动 lvl-ip
sudo ./lvl-ip &
LVL_IP_PID=$!
sleep 2

# 测试 1: UDP Echo
echo "Testing UDP echo..."
response=$(echo "test" | nc -u -w 1 10.0.0.4 8888)
if [ "$response" = "test" ]; then
    echo "✓ Test passed"
else
    echo "✗ Test failed"
    kill $LVL_IP_PID
    exit 1
fi

# 清理
kill $LVL_IP_PID
echo "All tests passed!"
```

### 7.3 性能测试

#### 吞吐量测试

```bash
# 使用 iperf3
# 服务端
./level-ip iperf3 -s

# 客户端
iperf3 -c 10.0.0.4 -t 30 -i 1

# 目标：
# - TCP: > 100 Mbps（取决于实现质量）
# - UDP: > 200 Mbps（无拥塞控制）
```

#### 延迟测试

```bash
# 使用 qperf
qperf 10.0.0.4 tcp_lat udp_lat

# 目标：
# - RTT < 5ms（本地网络）
```

#### 并发连接测试

```bash
# 使用 ab (Apache Bench)
ab -n 1000 -c 100 http://10.0.0.4:8080/

# 目标：
# - 支持 100+ 并发连接
# - 无连接失败
```

### 7.4 协议一致性测试

#### 使用 packetdrill

```bash
# 安装
git clone https://github.com/google/packetdrill.git
cd packetdrill/gtests/net/packetdrill
./configure && make

# 编写测试脚本（tcp_3way_handshake.pkt）
0   socket(..., SOCK_STREAM, IPPROTO_TCP) = 3
+0  bind(3, ..., ...) = 0
+0  listen(3, 1) = 0

+0  < S 0:0(0) win 32792 <mss 1460>
+0  > S. 0:0(0) ack 1 <...>
+.1 < . 1:1(0) ack 1 win 32792
+0  accept(3, ..., ...) = 4

# 运行测试
sudo ./packetdrill tcp_3way_handshake.pkt
```

### 7.5 压力测试

```bash
# SYN Flood 测试（测试 SYN 队列是否会溢出）
sudo hping3 -S -p 8080 --flood 10.0.0.4

# UDP Flood
sudo hping3 --udp -p 8888 --flood 10.0.0.4

# 观察 CPU 和内存使用
top -p $(pidof lvl-ip)
```

---

## 8. 调试技巧与工具

### 8.1 tcpdump 使用详解

#### 基础抓包

```bash
# 抓取所有包
sudo tcpdump -i tap0 -n

# 保存到文件
sudo tcpdump -i tap0 -w capture.pcap

# 从文件读取
tcpdump -r capture.pcap
```

#### 过滤器

```bash
# 只看 UDP
sudo tcpdump -i tap0 udp -n

# 只看特定端口
sudo tcpdump -i tap0 port 8888 -n

# 只看 SYN 包
sudo tcpdump -i tap0 'tcp[tcpflags] & tcp-syn != 0' -n

# 只看来自特定 IP
sudo tcpdump -i tap0 src 10.0.0.4 -n

# 组合条件
sudo tcpdump -i tap0 'tcp and port 8080 and src 10.0.0.4' -n

# ICMPv6
sudo tcpdump -i tap0 icmp6 -n

# 邻居发现
sudo tcpdump -i tap0 'icmp6 and (ip6[40] == 135 or ip6[40] == 136)' -n
```

#### 详细输出

```bash
# 显示详细信息
sudo tcpdump -i tap0 -n -vvv

# 显示 16 进制和 ASCII
sudo tcpdump -i tap0 -n -X

# 显示完整包内容
sudo tcpdump -i tap0 -n -XX
```

### 8.2 Wireshark 分析

#### 启动 Wireshark

```bash
sudo wireshark -i tap0 -k &
```

#### 常用过滤器

```
# UDP
udp
udp.port == 8888

# TCP
tcp
tcp.port == 8080
tcp.flags.syn == 1
tcp.analysis.retransmission

# ICMPv6
icmpv6
icmpv6.type == 128  # Echo Request
icmpv6.type == 135  # Neighbor Solicitation

# 错误分析
tcp.analysis.flags
tcp.analysis.duplicate_ack
```

#### 高级功能

**跟踪 TCP 流：**
- 右键点击包 → Follow → TCP Stream
- 可以看到完整的会话内容

**统计信息：**
- Statistics → Protocol Hierarchy（协议分布）
- Statistics → Conversations（会话统计）
- Statistics → IO Graphs（流量图）

**专家信息：**
- Analyze → Expert Information
- 自动识别协议问题（重传、乱序、校验和错误等）

### 8.3 GDB 调试

#### 基础调试

```bash
# 编译时添加调试符号
make clean
CFLAGS="-g" make debug

# 启动 GDB
gdb ./lvl-ip

# 常用命令
(gdb) break tcp_in         # 设置断点
(gdb) run                  # 运行
(gdb) continue             # 继续
(gdb) next                 # 单步（不进入函数）
(gdb) step                 # 单步（进入函数）
(gdb) print *sk            # 打印变量
(gdb) backtrace            # 查看调用栈
(gdb) info locals          # 查看局部变量
```

#### 多线程调试

```bash
# 查看所有线程
(gdb) info threads

# 切换线程
(gdb) thread 2

# 在所有线程设置断点
(gdb) break tcp_in thread all

# 只在特定线程设置断点
(gdb) break tcp.c:100 thread 1
```

#### 条件断点

```bash
# 条件断点
(gdb) break tcp_in if th->dport == 8080

# 观察点（变量变化时中断）
(gdb) watch tsk->cwnd

# 条件观察
(gdb) watch tsk->cwnd if tsk->cwnd > 10000
```

#### 查看数据结构

```bash
# 打印结构体
(gdb) print *sk
(gdb) print/x sk->saddr  # 16 进制

# 打印数组
(gdb) print sk->receive_queue@10

# 查看内存
(gdb) x/16xb skb->data  # 16 字节 16 进制
(gdb) x/10i $pc         # 10 条指令
```

### 8.4 性能分析

#### perf 工具

```bash
# 记录性能数据
sudo perf record -g ./lvl-ip

# Ctrl+C 停止后生成报告
sudo perf report

# 实时监控
sudo perf top

# 查看热点函数
sudo perf top -g -p $(pidof lvl-ip)
```

#### strace 系统调用跟踪

```bash
# 跟踪所有系统调用
sudo strace -p $(pidof lvl-ip)

# 只看网络相关
sudo strace -e trace=network -p $(pidof lvl-ip)

# 统计系统调用
sudo strace -c ./lvl-ip

# 跟踪特定调用
sudo strace -e read,write -p $(pidof lvl-ip)
```

#### valgrind 内存检查

```bash
# 检查内存泄漏
valgrind --leak-check=full --show-leak-kinds=all ./lvl-ip

# 检查线程问题
valgrind --tool=helgrind ./lvl-ip

# 检查数组越界
valgrind --tool=exp-sgcheck ./lvl-ip
```

### 8.5 日志系统

建议实现灵活的日志系统，便于调试。

**设计要点：**
- 多个日志级别：DEBUG、INFO、WARN、ERROR
- 可以动态调整日志级别
- 带时间戳和源文件位置
- 可以输出到文件或 syslog

**使用示例：**
```c
LOG_DEBUG("UDP: received packet, port=%d, len=%d", port, len);
LOG_INFO("TCP: connection established, fd=%d", fd);
LOG_WARN("Receive queue nearly full, len=%d", queue_len);
LOG_ERROR("Checksum verification failed");
```

---

## 9. 常见问题与解决方案

### 9.1 编译问题

**Q: 找不到头文件**
```
error: udp.h: No such file or directory
```
**A:** 检查 Makefile 中的 `-I include`

**Q: 未定义的引用**
```
undefined reference to `udp_init'
```
**A:** 确保源文件被加入到 Makefile：`src = $(wildcard src/*.c)`

### 9.2 运行时问题

**Q: 收不到数据包**

**诊断步骤：**
1. 检查 TAP 设备：`ip addr show tap0`
2. 检查路由：`ip route`
3. 使用 tcpdump：`sudo tcpdump -i tap0 -n`

**可能原因：**
- TAP 设备未启动
- 路由配置错误
- 防火墙规则

**Q: 程序崩溃 (Segmentation fault)**

**诊断步骤：**
```bash
# 使用 GDB 查看崩溃位置
gdb ./lvl-ip
(gdb) run
# 崩溃后
(gdb) backtrace
(gdb) frame 0
(gdb) list

# 使用 valgrind
valgrind ./lvl-ip
```

**常见原因：**
- 空指针访问
- 缓冲区溢出
- 释放后使用 (use-after-free)

**Q: 校验和错误**

**诊断步骤：**
- 使用 Wireshark 查看正确的校验和
- 对比自己的计算结果
- 检查是否包含了伪头部
- 检查字节序是否正确

### 9.3 协议问题

**Q: TCP 连接无法建立**

**检查点：**
1. 是否正确发送 SYN？
2. 是否正确接收 SYN+ACK？
3. 序列号是否正确？
4. 窗口大小是否设置？

**调试方法：**
```bash
sudo tcpdump -i tap0 'tcp and port 8080' -n -vvv
```

**Q: UDP 数据丢失**

**原因：**
- UDP 本身不可靠
- 接收队列满了
- 校验和错误被丢弃

**验证：**
- 检查日志输出
- 使用 Wireshark 确认包到达

**Q: IPv6 不工作**

**检查：**
```bash
# 系统是否支持 IPv6
cat /proc/sys/net/ipv6/conf/all/disable_ipv6  # 应该是 0

# 接口是否有 IPv6 地址
ip -6 addr show tap0

# 测试连通性
ping6 -c 1 ::1
```

### 9.4 性能问题

**Q: 吞吐量低**

**可能原因：**
1. 窗口太小
2. 没有实现拥塞控制
3. 重传太频繁
4. 缓冲区不足

**优化方法：**
- 增大窗口
- 实现拥塞控制
- 调整 MSS
- 使用更大的缓冲区

**Q: CPU 占用高**

**诊断：**
```bash
# 查看热点函数
sudo perf top -p $(pidof lvl-ip)
```

**常见原因：**
- 忙等待（应该用条件变量）
- 频繁的内存分配
- 低效的算法（线性查找）

---

## 10. 总结与建议

### 10.1 学习路线

**第 1-2 个月：UDP + 测试**
- 重点：理解协议基础，熟悉代码结构
- 目标：实现完整的 UDP 功能
- 验收：通过 `nc -u` 测试

**第 3-4 个月：TCP 服务端**
- 重点：理解状态机，掌握多线程
- 目标：实现 bind、listen、accept
- 验收：通过 `nc` 连接测试

**第 5-6 个月：ICMPv6**
- 重点：理解 IPv6，学习 NDP
- 目标：支持 ping6 和基本 NDP
- 验收：通过 `ping6` 测试

**第 7-9 个月：TCP 优化**
- 重点：拥塞控制算法，性能调优
- 目标：实现 Reno 算法
- 验收：通过 `iperf3` 测试，吞吐量 > 100 Mbps

**第 10 个月：完善与文档**
- Bug 修复
- 性能测试
- 代码重构
- 编写文档

### 10.2 关键成功因素

1. **循序渐进** - 不要跳跃，每个功能都要测试通过
2. **理解协议** - 阅读 RFC，理解为什么这样设计
3. **参考实现** - 遇到问题看 Linux 内核源码
4. **多调试** - 使用 tcpdump、Wireshark、GDB
5. **写测试** - 每个功能都有测试用例
6. **记录笔记** - 记录问题和解决方案

### 10.3 避免的坑

1. **字节序混淆** - 网络序 vs 主机序，时刻注意
2. **指针算术** - 小心缓冲区溢出
3. **多线程竞争** - 记得加锁
4. **内存泄漏** - 每个 malloc 都要 free
5. **过早优化** - 先实现功能，再优化性能

### 10.4 进度追踪

建议创建 `PROGRESS.md` 文件：

```markdown
# 开发进度

## 第 1 周（2026-01-20 - 2026-01-26）
- [x] 阅读 RFC 768
- [x] UDP 数据结构定义
- [ ] UDP 接收实现（进行中）

## 问题记录
1. 校验和计算错误 - 已解决（忘记伪头部）
2. 编译错误 - 已解决（Makefile 问题）

## 下周计划
- [ ] 完成 UDP 接收
- [ ] 开始 UDP 发送
```

---

## 附录：快速参考

### A. 常用命令

```bash
# 编译
make clean && make debug

# 运行
sudo ./lvl-ip

# 抓包
sudo tcpdump -i tap0 -n -vvv

# 测试 UDP
echo "test" | nc -u 10.0.0.4 8888

# 测试 TCP
nc 10.0.0.4 8080

# 测试 IPv6
ping6 fe80::1%tap0

# GDB 调试
gdb ./lvl-ip

# 性能测试
iperf3 -s  # 服务端
iperf3 -c 10.0.0.4  # 客户端
```

### B. 数据结构大小

```c
sizeof(struct eth_hdr)    = 14 bytes
sizeof(struct iphdr)      = 20 bytes (无选项)
sizeof(struct ipv6_hdr)   = 40 bytes
sizeof(struct tcphdr)     = 20 bytes (无选项)
sizeof(struct udp_hdr)    = 8 bytes
sizeof(struct icmpv6_hdr) = 8 bytes
```

### C. 协议号

```c
IPPROTO_ICMP    = 1
IPPROTO_TCP     = 6
IPPROTO_UDP     = 17
IPPROTO_ICMPV6  = 58
```

### D. 以太网类型

```c
ETH_P_IP   = 0x0800
ETH_P_ARP  = 0x0806
ETH_P_IPV6 = 0x86DD
```

### E. 参考资源链接

#### RFC 文档
- UDP: https://www.rfc-editor.org/rfc/rfc768.html
- IPv6: https://www.rfc-editor.org/rfc/rfc8200.html
- ICMPv6: https://www.rfc-editor.org/rfc/rfc4443.html
- NDP: https://www.rfc-editor.org/rfc/rfc4861.html
- TCP: https://www.rfc-editor.org/rfc/rfc793.html
- TCP 拥塞控制: https://www.rfc-editor.org/rfc/rfc5681.html

#### 在线工具
- RFC Editor: https://www.rfc-editor.org/
- IPv6 Calculator: https://www.ultratools.com/tools/ipv6CIDRToRange
- TCP/IP Guide: http://www.tcpipguide.com/

#### 开源实现
- Linux 内核: https://github.com/torvalds/linux
- lwIP: https://savannah.nongnu.org/projects/lwip/
- picoTCP: https://github.com/tass-belgium/picotcp

---

**祝开发顺利！** 🚀

记住：**理解原理比写代码更重要！**

遇到问题时：
1. 先查 RFC 文档
2. 看 Linux 内核实现
3. 使用调试工具
4. 在社区提问
