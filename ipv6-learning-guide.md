# IPv6 协议知识学习手册

> 写给即将动手实现 IPv6 协议栈的你。先理解，再编码。
>
> 阅读顺序：按章节顺序，每章对应实现的一个 Phase。

---

## 目录

1. [为什么需要 IPv6](#1-为什么需要-ipv6)
2. [IPv6 地址体系](#2-ipv6-地址体系)
3. [IPv6 首部详解](#3-ipv6-首部详解)
4. [扩展首部链](#4-扩展首部链)
5. [ICMPv6](#5-icmpv6)
6. [NDP 邻居发现协议](#6-ndp-邻居发现协议)
7. [SLAAC 无状态地址自动配置](#7-slaac-无状态地址自动配置)
8. [路径 MTU 发现（PMTUD）](#8-路径-mtu-发现pmtud)
9. [TCP/UDP 适配 IPv6](#9-tcpudp-适配-ipv6)
10. [实现路线图：从 Phase 1 到 Phase 5](#10-实现路线图)

---

## 1. 为什么需要 IPv6

### 1.1 直接原因：地址耗尽

```
IPv4 地址空间：2^32 = 4,294,967,296 ≈ 43 亿

看起来很多？但：
  - 全球人口 ~80 亿
  - 每人平均手机 + 电脑 + 平板 + IoT 设备 = 5-10 个网络接口
  - 需要 ~400-800 亿个地址

IPv4 的补救措施（都不完美）：
  NAT（网络地址转换）：多个设备共享一个公网 IP
    → 破坏端到端原则，P2P 通信困难
    → 增加延迟和复杂度
  CIDR（无类域间路由）：更灵活的地址分配
    → 只是延缓耗尽，不能根本解决
```

**2011 年 2 月 3 日，IANA 分配完最后一批 IPv4 地址。**

### 1.2 深层原因：IPv4 的设计缺陷

IPv4 诞生于 1981 年（RFC 791），当时的假设到今天很多已经不成立：

| 1981 年的假设 | 今天的现实 | IPv6 的解决方式 |
|---|---|---|
| 主机数量少，地址够用 | 万物互联 | 128 位地址 |
| 路由器是软件，慢慢算 | 硬件线速转发 | 固定首部，简化处理 |
| 安全性不是问题 | 网络攻击泛滥 | IPsec 原生支持 |
| 不需要自动配置 | 设备即插即用 | SLAAC / DHCPv6 |
| 不需要 QoS | 视频/语音需要优先级 | Flow Label + Traffic Class |

### 1.3 IPv6 不是"更大地址的 IPv4"

这是最重要的认知：**IPv6 是一次重新设计**，不只是把地址从 32 位扩展到 128 位。

```
IPv4 的设计哲学："什么功能都放在 IP 层"
IPv6 的设计哲学："IP 层只做转发，其余交给端到端"

具体表现：
  IPv4 路由器：校验和 + 分片 + Options 解析 + 路由查找 = 慢
  IPv6 路由器：路由查找 = 快（其他全不做）
```

---

## 2. IPv6 地址体系

### 2.1 地址表示法

```
IPv4 点分十进制：192.168.1.1（4 组 × 8 位）

IPv6 冒号十六进制：2001:0db8:85a3:0000:0000:8a2e:0370:7334
                    （8 组 × 16 位 = 128 位）

缩写规则：
  1. 每组的前导零可省略：0db8 → db8, 0370 → 370
  2. 连续的全零组用 :: 替代（只能用一次）

示例：
  2001:0db8:0000:0000:0000:0000:0000:0001
  → 2001:db8::1

  fe80:0000:0000:0000:0000:0000:0000:0001
  → fe80::1
```

### 2.2 地址类型

IPv6 有三种主要地址类型，和 IPv4 对比：

```
IPv4:
  ┌──────────┐  ┌──────────┐  ┌──────────┐
  │  单播    │  │  广播    │  │  多播    │
  │ 1对1     │  │ 1对全体  │  │ 1对一组  │
  └──────────┘  └──────────┘  └──────────┘

IPv6:
  ┌──────────┐  ┌──────────┐  ┌──────────┐
  │  单播    │  │  多播    │  │  任播    │
  │ 1对1     │  │ 1对一组  │  │ 1对最近  │
  └──────────┘  └──────────┘  └──────────┘
       ↑             ↑             ↑
   没有广播！     更细粒度      全新概念
```

**关键变化：IPv6 没有广播。** 所有 IPv4 用广播做的事，IPv6 都用多播替代。

### 2.3 单播地址类型

| 类型 | 前缀 | 用途 | IPv4 类比 |
|---|---|---|---|
| 全局单播 | 2000::/3 | 公网通信 | 公网 IP |
| 链路本地 | fe80::/10 | 同链路通信，不需要配置 | 169.254.x.x（但 IPv6 的更重要） |
| 唯一本地 | fc00::/7 | 私网通信 | 10.x.x.x / 192.168.x.x |
| 回环 | ::1 | 本机通信 | 127.0.0.1 |
| 未指定 | :: | 尚未获得地址 | 0.0.0.0 |

### 2.4 链路本地地址（Link-Local）— IPv6 最重要的新概念

```
每个 IPv6 接口自动生成一个链路本地地址，无需任何配置：

  fe80:: + 接口标识符（通常由 MAC 地址生成）

生成方法（EUI-64）：
  MAC 地址：aa:bb:cc:dd:ee:ff（6 字节）
  
  步骤 1：中间插入 FF FE
    aa:bb:cc:FF:FE:dd:ee:ff（8 字节）
  
  步骤 2：翻转第 7 位（Universal/Local 位）
    aa → a8（二进制 10101010 → 10101000）
    a8:bb:cc:FF:FE:dd:ee:ff
  
  步骤 3：拼上前缀
    fe80::a8bb:ccff:fedd:e eff

为什么重要？
  - 接口一启动就有地址，可以立刻和同链路设备通信
  - NDP（邻居发现）就用链路本地地址
  - SLAAC 也先用链路本地，再获取全局地址
  - 路由器之间用链路本地地址交换路由信息
```

### 2.5 多播地址

```
IPv4 多播：224.0.0.0/4（224.0.0.0 ~ 239.255.255.255）
IPv6 多播：ff00::/8

IPv6 多播更精细，有 scope（范围）概念：

  ff0X::/8  中的 X = scope：
    X=1 → 接口本地（仅本机）
    X=2 → 链路本地（同链路）
    X=5 → 站点本地
    X=8 → 组织本地
    X=E → 全局

常用多播地址：
  ff02::1     — 所有节点（链路本地），类似 IPv4 的广播
  ff02::2     — 所有路由器（链路本地）
  ff02::1:ffXX:XXXX — solicited-node 多播（NDP 用）
```

### 2.6 Solicited-Node 多播地址

这个概念是 NDP 的核心，需要理解：

```
作用：把"广播打扰所有人"变成"多播只打扰少数人"

计算方法：
  IPv6 单播地址：2001:db8::1234:5678
  取最后 24 位：34:5678 → 拼到 ff02::1:ff 后面
  结果：ff02::1:ff34:5678

对应的 MAC 多播地址：
  33:33:ff:34:56:78（前 3 字节固定 33:33:ff，后 3 字节同 IPv6 后 24 位）

为什么"只打扰少数人"？
  一个 /64 子网有 2^64 个地址
  但最后 24 位相同的最多 2^24 ≈ 1600 万个
  实际上一个链路上通常只有几十个设备
  所以 solicited-node 多播几乎就是"单播"的精确度
```

### 2.7 地址作用域总结图

```
┌─────────────────────────────────────────────────┐
│                 全局单播                          │
│           2001:db8::1（可路由到全球）              │
│  ┌───────────────────────────────────────────┐   │
│  │            唯一本地                        │   │
│  │       fd00::1（私网内可达）                │   │
│  │  ┌─────────────────────────────────────┐  │   │
│  │  │          链路本地                    │  │   │
│  │  │     fe80::1（同链路可达）            │  │   │
│  │  │  ┌──────────────────────────────┐   │  │   │
│  │  │  │       回环                   │   │  │   │
│  │  │  │      ::1（仅本机）           │   │  │   │
│  │  │  └──────────────────────────────┘   │  │   │
│  │  └─────────────────────────────────────┘  │   │
│  └───────────────────────────────────────────┘   │
└─────────────────────────────────────────────────┘
```

---

## 3. IPv6 首部详解

### 3.1 固定首部（40 字节）

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|Version| Traffic Class |           Flow Label                 |
| (4)   |     (8)       |              (20)                    |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|         Payload Length        |  Next Header  |   Hop Limit   |
|            (16)               |      (8)      |      (8)      |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                         Source Address                        +
|                        (128 bits)                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                                                               |
+                      Destination Address                      +
|                        (128 bits)                             |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

总计：4 + 4 + 2 + 1 + 1 + 16 + 16 = 40 + 4 字节对齐 = 40 字节 ✓
```

### 3.2 与 IPv4 首部的核心差异

| 设计决策 | IPv4 | IPv6 | 为什么 |
|---|---|---|---|
| 首部长度 | 20-60 字节可变 | **固定 40 字节** | 路由器不用算 IHL，直接跳 40 字节 |
| 校验和 | 每跳重算 | **去掉** | 链路层 CRC + 传输层校验已够，端到端原则 |
| 分片 | 路由器可分片 | **仅端到端** | 路由器不分片，只发 Packet Too Big |
| 选项 | 内嵌在首部 | **扩展首部链** | 路由器不需要看的就不处理 |
| 地址长度 | 32 bits | **128 bits** | 地址空间 + 层次化路由聚合 |

### 3.3 Flow Label — IPv6 独有的 QoS 机制

```
场景：你在看视频会议，同时下载文件

IPv4 的做法（路由器视角）：
  收到一个包 → 解析五元组（源IP + 目的IP + 源端口 + 目的端口 + 协议）
  → 查 QoS 策略表 → 决定优先级
  问题：需要解析到 TCP/UDP 层才能看到端口号，开销大

IPv6 的做法（路由器视角）：
  收到一个包 → 只看 Flow Label（20 bits）
  → 查缓存 "Flow Label → QoS 策略" → 决定优先级
  优势：只看首部 4 字节，不用解析到传输层

使用方式：
  发送端为每个"流"随机生成一个 Flow Label
  同一个流的所有包用同一个 Flow Label
  路由器缓存映射关系，快速决策
```

---

## 4. 扩展首部链

### 4.1 设计思想

IPv4 的 Options 嵌在首部里，所有路由器都要解析。IPv6 用链式结构解决：

```
IPv4 Options（嵌在首部，可变长）：
┌──────────────────────────────┐
│ IPv4 Header (20-60 bytes)    │
│   ...Options 嵌在这里...     │
└──────────────────────────────┘
问题：路由器必须解析所有 Options

IPv6 Extension Headers（链式，按需挂载）：
┌─────────────────┐
│ IPv6 Fixed Hdr  │ Next Header = 0 (Hop-by-Hop)
├─────────────────┤
│ Hop-by-Hop Opts │ Next Header = 44 (Fragment)
├─────────────────┤
│ Fragment Header │ Next Header = 6 (TCP)
├─────────────────┤
│ TCP Header      │
├─────────────────┤
│ Payload         │
└─────────────────┘
优势：路由器只看 Hop-by-Hop，其余直接跳过
```

### 4.2 Next Header 字段的双重角色

```
在 IPv6 固定首部中：
  Next Header = 上层协议类型（和 IPv4 的 Protocol 字段一样）
  
  但如果值是 0/43/44/60，表示后面跟着扩展首部
  扩展首部内部也有 Next Header，指向下一个

遍历算法（伪代码）：
  ptr = 固定首部后面
  next = ipv6hdr->next_header
  
  while (next 是扩展首部类型) {
      exthdr = (struct ext_hdr *)ptr
      next = exthdr->next_header    // 跳到下一个
      ptr += exthdr->length         // 移动指针
  }
  // 最终 next = 6(TCP) / 17(UDP) / 58(ICMPv6)
```

### 4.3 各种扩展首部

| Next Header 值 | 类型 | 谁处理 | 长度 |
|---|---|---|---|
| 0 | Hop-by-Hop Options | **每个中间路由器** | TLV 可变 |
| 43 | Routing Header | 仅目的节点 | 固定 + 可变 |
| 44 | Fragment Header | 仅目的节点 | **固定 8 字节** |
| 60 | Destination Options | 仅目的节点 | TLV 可变 |
| 59 | No Next Header | — | 后面没有了 |

**关键规则（RFC 8200）**：
- Hop-by-Hop Options 如果存在，**必须紧跟固定首部**
- 路由器**只需要处理** Hop-by-Hop Options，其余全部跳过
- 未知类型的扩展首部按 TLV 规则处理（Type 高位决定行为）

### 4.4 Fragment Header 详解

```
 0                   1                   2                   3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|  Next Header  |   Reserved    |      Fragment Offset    |Res|M|
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
|                         Identification                        |
+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

  Next Header (8): 分片后面的协议
  Fragment Offset (13): 偏移，单位 8 字节
  M flag (1): 1=还有后续分片, 0=最后一个
  Identification (32): 同一原始包的分片共享此 ID
  Reserved (8+2): 保留位

和 IPv4 分片的差异：
  IPv4: 路由器可以分片，Identification 只有 16 bits（容易回绕）
  IPv6: 只有发送端可以分片，Identification 32 bits（足够用）
```

### 4.5 扩展首部的 TLV 编码

Hop-by-Hop / Destination Options 使用 TLV（Type-Length-Value）编码：

```
┌──────┬────────┬─────────────────────┐
│ Type │ Length │      Value          │
│(1B)  │  (1B)  │  (Length 字节)      │
└──────┴────────┴─────────────────────┘

Type 字段的最高 2 bits 决定未知选项的处理方式：
  00 → 跳过此选项，继续处理
  01 → 跳过此选项，继续处理
  10 → 丢弃此包，发 ICMPv6 Parameter Problem
  11 → 丢弃此包，如果目的不是多播则发 ICMPv6

这保证了：未来新增的扩展选项，不认识也能安全处理
```

---

## 5. ICMPv6

### 5.1 ICMPv6 不只是 "ping"

IPv4 的 ICMP 主要做两件事：echo（ping）+ 错误报告。
IPv6 的 ICMPv6 承担更多职责：

```
ICMPv6 的三大类功能：

1. 诊断功能（和 ICMPv4 类似）
   - Echo Request / Reply（ping6）
   - 目的不可达
   - Packet Too Big（PMTUD 核心！）
   - Time Exceeded

2. 邻居发现 NDP（替代 ARP！）
   - Neighbor Solicitation (NS)
   - Neighbor Advertisement (NA)
   - Router Solicitation (RS)
   - Router Advertisement (RA)
   - Redirect

3. 组播监听 MLD（替代 IGMP）
   - 管理多播组成员关系
```

### 5.2 ICMPv6 报文类型

```
错误消息（Type 0-127）：
  1   — Destination Unreachable（目的不可达）
  2   — Packet Too Big（包太大，PMTUD 核心）
  3   — Time Exceeded（TTL 超时）
  4   — Parameter Problem（参数问题）
  127 — 未分配

信息消息（Type 128-255）：
  128 — Echo Request（ping 请求）
  129 — Echo Reply（ping 回复）
  130-137 — MLD 消息
  133 — Router Solicitation (RS)
  134 — Router Advertisement (RA)
  135 — Neighbor Solicitation (NS)
  136 — Neighbor Advertisement (NA)
  137 — Redirect

注意：ICMPv6 Type 编号和 ICMPv4 完全不同！
  ICMPv4 Echo = Type 8/0
  ICMPv6 Echo = Type 128/129
```

### 5.3 ICMPv6 校验和 — 和 ICMPv4 的关键差异

```
ICMPv4 校验和：只覆盖 ICMP 报文本身
ICMPv6 校验和：覆盖 ICMPv6 报文 + 伪首部（和 TCP/UDP 一样！）

伪首部结构：
  Source Address (16 bytes)
  Destination Address (16 bytes)
  Upper-Layer Packet Length (4 bytes)
  Zeros (3 bytes)
  Next Header = 58 (1 byte)

为什么？因为 ICMPv6 承担了 NDP 这种关键功能，
必须确保源/目的地址没有被篡改。伪首部提供了端到端的完整性保护。
```

---

## 6. NDP 邻居发现协议

### 6.1 为什么 NDP 替代 ARP

```
ARP 的问题：
  1. 用广播 → 打扰链路上所有设备
  2. 只有 IPv4 → 每种网络层协议都需要自己的 ARP
  3. 无认证 → ARP 欺骗攻击很容易
  4. 功能单一 → 只做地址解析

NDP 的优势：
  1. 用多播（solicited-node）→ 只打扰目标设备
  2. 协议无关 → 通过 ICMPv6 实现，和 IP 版本解耦
  3. 可扩展 → 预留了安全扩展（SEND, RFC 3971）
  4. 功能丰富 → 地址解析 + 路由发现 + 地址自动配置 + DAD
```

### 6.2 NDP 的 5 种消息

| 消息 | Type | 方向 | 作用 |
|---|---|---|---|
| Router Solicitation (RS) | 133 | 主机 → 路由器 | "有没有路由器？告诉我你的信息" |
| Router Advertisement (RA) | 134 | 路由器 → 主机 | "我是路由器，这是网络参数" |
| Neighbor Solicitation (NS) | 135 | 任意 → 目标 | "这个 IPv6 地址的 MAC 是什么？" |
| Neighbor Advertisement (NA) | 136 | 目标 → 请求者 | "我的 MAC 是 XX:XX:XX:XX:XX:XX" |
| Redirect | 137 | 路由器 → 主机 | "去这个目的地走这条路更近" |

### 6.3 NS/NA 交互流程（替代 ARP 请求/应答）

```
ARP 流程（IPv4）：
  A 想知道 B(10.0.0.5) 的 MAC：
    A → 广播 FF:FF:FF:FF:FF:FF : "谁是 10.0.0.5？"
    B → 单播给 A : "我是 10.0.0.5，MAC 是 bb:bb:bb:bb:bb:bb"

NDP 流程（IPv6）：
  A 想知道 B(2001:db8::5) 的 MAC：

  步骤 1：计算 B 的 solicited-node 多播地址
    2001:db8::5 → ff02::1:ff00:0005
    对应 MAC：33:33:ff:00:00:05

  步骤 2：发送 NS（Neighbor Solicitation）
    A → 多播 33:33:ff:00:00:05 : "谁是 2001:db8::5？"
    NS 报文中包含 A 的链路层地址（源 LLA Option）

  步骤 3：B 回复 NA（Neighbor Advertisement）
    B → 单播给 A : "我是 2001:db8::5，MAC 是 bb:bb:bb:bb:bb:bb"
    NA 报文中包含 B 的链路层地址（目标 LLA Option）

关键差异：
  ARP 广播 → 打扰所有设备
  NDP 多播 → 只打扰 MAC 后 24 位匹配的设备（通常就 1 个）
```

### 6.4 邻居缓存状态机

NDP 维护的邻居缓存比 ARP 缓存复杂得多：

```
                    ┌────────────┐
         NS 发送    │            │  NA 收到
        ┌──────────→│ INCOMPLETE │──────────┐
        │           │            │           │
        │           └─────┬──────┘           │
        │                 │                  │
        │            超时/无响应              │
        │                 │                  │
        │                 ▼                  ▼
        │           ┌──────────┐     ┌───────────┐
        │           │  FAILED  │     │ REACHABLE │
        │           └──────────┘     │ (可达)     │
        │                            └─────┬─────┘
        │                                  │ 可达时间到
        │                                  ▼
        │                            ┌──────────┐
        │                            │  STALE   │
        │                            │ (过期)    │
        │                            └─────┬────┘
        │                                  │ 需要发包
        │                                  ▼
        │                            ┌──────────┐
        │                 NS 发送    │  DELAY   │
        │              ┌────────────→│ (延迟)    │
        │              │             └─────┬────┘
        │              │                   │ 延迟超时
        │              │                   ▼
        │              │             ┌──────────┐
        │              │             │  PROBE   │
        │              │             │ (探测)    │
        │              │             └─────┬────┘
        │              │                   │ NA 收到
        │              │                   ▼
        │              └─────────── REACHABLE
        │
        └──── FAILED 后重试

状态含义：
  INCOMPLETE : NS 已发，等 NA（对应 ARP 的 PENDING）
  REACHABLE  : 确认邻居可达（有正向确认）
  STALE      : 可达时间过了，但没理由认为不可达
  DELAY      : 有包要发，等一下看有没有上层确认
  PROBE      : DELAY 超时，主动发 NS 探测
  FAILED     : 多次探测无响应
```

### 6.5 DAD 重复地址检测

```
在使用一个地址之前，必须先确认没人用这个地址：

  1. 给地址标记为 TENTATIVE（暂定）
  2. 发送 NS，目标地址 = 自己的待定地址
     源地址 = ::（未指定地址，因为还没正式拥有）
     目标 = 自己的 solicited-node 多播地址
  3. 等待一段时间：
     - 如果收到 NA → 地址冲突！不能使用
     - 如果超时没收到 → 地址可用，标记为 PREFERRED

这就像在房间里喊"有人叫张三吗？"——没人应就是你的了。
```

---

## 7. SLAAC 无状态地址自动配置

### 7.1 核心思想

```
DHCP（IPv4）：中心化服务器分配地址
  客户端 → DHCP Discover → DHCP Offer → DHCP Request → DHCP ACK
  需要专门的服务器，有状态管理

SLAAC（IPv6）：去中心化，每个设备自己算地址
  路由器定期广播 RA（Router Advertisement）
  RA 包含：网络前缀 + 前缀长度 + 其他参数
  设备自己用 前缀 + 接口标识符 = 完整地址
  不需要服务器，无状态

类比：
  DHCP = 公司 HR 给每个员工分配工号
  SLAAC = 公司发一个部门编号前缀，员工自己用身份证号拼出工号
```

### 7.2 SLAAC 完整流程

```
阶段 1：生成链路本地地址
  ┌──────┐
  │ 主机  │
  └──┬───┘
     │ 1. 用 EUI-64 从 MAC 生成接口标识符
     │ 2. 拼上 fe80::/10 前缀
     │ 3. 做 DAD（重复地址检测）
     │ 4. 确认无冲突 → 链路本地地址生效
     │    fe80::a8bb:ccff:fedd:eeff

阶段 2：发现路由器
     │ 5. 发送 RS（Router Solicitation）
     │    → 多播 ff02::2（所有路由器）
     │

阶段 3：获取网络参数
  ┌──┴───┐
  │路由器 │
  └──┬───┘
     │ 6. 回复 RA（Router Advertisement）
     │    包含：
     │    - 网络前缀：2001:db8:1::/64
     │    - 前缀有效期
     │    - M flag（是否用 DHCPv6）
     │    - O flag（是否用 DHCPv6 获取其他配置）
     │    - A flag（是否用 SLAAC）

阶段 4：生成全局地址
  ┌──────┐
  │ 主机  │
  └──┬───┘
     │ 7. 用 RA 中的前缀 + 自己的接口标识符
     │    2001:db8:1:: + a8bb:ccff:fedd:eeff
     │    = 2001:db8:1:a8bb:ccff:fedd:eeff
     │ 8. 做 DAD
     │ 9. 确认无冲突 → 全局地址生效
     │
     │ 10. 添加默认路由指向路由器
```

### 7.3 RA 中的关键标志位

```
RA 报文中有一个 8-bit 的标志字段：

  0 1 2 3 4 5 6 7
  ┌─┬─┬─┬─────────┐
  │M│O│ │ 保留     │
  └─┴─┴─┴─────────┘

  M (Managed) = 1 : 请使用 DHCPv6 获取地址（不用 SLAAC）
  O (Other) = 1   : 地址用 SLAAC，但 DNS 等其他配置用 DHCPv6
  M=0, O=0        : 纯 SLAAC，所有配置从 RA 获取

常见组合：
  SLAAC 模式：M=0, O=0（最简单，纯无状态）
  SLAAC + DHCPv6 DNS：M=0, O=1（地址自己算，DNS 问 DHCPv6）
  全 DHCPv6：M=1（像 IPv4 一样全部由服务器分配）
```

---

## 8. 路径 MTU 发现（PMTUD）

### 8.1 为什么需要 PMTUD

```
TCP 在建立连接时协商 MSS（最大段大小），基于本地 MTU。
但路径上可能有更小的 MTU：

  发送端 (MTU=1500) ──→ 路由器1 (MTU=1500) ──→ 路由器2 (MTU=1280) ──→ 接收端
  
  如果发送端发 1500 字节的包，到路由器2 就过不去了。

IPv4 的做法：
  路由器2 做分片 → 把 1500 拆成 1280 + 220 → 接收端重组
  问题：分片增加路由器负担，重组可能出错

IPv6 的做法：
  路由器2 不分片 → 丢弃包 → 发 ICMPv6 Packet Too Big 给发送端
  → 发送端收到后降低 PMTU → 后续发小包
  优势：路由器简单，端到端处理
```

### 8.2 PMTUD 流程

```
初始状态：PMTU = 本地接口 MTU（如 1500）

发送 1500 字节包：
  发送端 ──[1500B]──→ 路由器1 ──[1500B]──→ 路由器2
                                                │
                                           MTU=1280
                                           包太大！
                                                │
                                                ▼
  发送端 ←──[ICMPv6 Packet Too Big, MTU=1280]──┘

更新 PMTU = 1280
后续包按 1280 发送：
  发送端 ──[1280B]──→ 路由器1 ──[1280B]──→ 路由器2 ──[1280B]──→ 接收端 ✓

PMTUD 老化：
  每隔一段时间（如 10 分钟），尝试增大 PMTU
  如果成功 → PMTU 增大
  如果失败 → 保持当前 PMTU
```

### 8.3 PMTUD 黑洞问题

```
如果 ICMPv6 Packet Too Big 被防火墙拦截：
  发送端发了 1500 字节包
  路由器丢弃但 ICMPv6 被防火墙挡了
  发送端不知道 → TCP 重传 → 还是 1500 → 还是被丢弃
  → 死循环（PMTUD 黑洞）

解决方案（RFC 8027 等）：
  1. 管理员修复防火墙（最佳）
  2. TCP 层检测：连续 N 次重传同一数据 → 降级到 1280
  3. 发送端一开始就用 1280（保守但安全）

我们的实现选择（Q-003）：纯 PMTUD 无降级
  学习项目遇到黑洞概率极低，先不做复杂处理
```

---

## 9. TCP/UDP 适配 IPv6

### 9.1 TCP 和 UDP 本身不需要改

```
好消息：TCP 和 UDP 首部完全不关心 IP 版本

  TCP Header: 源端口 | 目的端口 | 序号 | 确认号 | ... | 校验和
  UDP Header: 源端口 | 目的端口 | 长度 | 校验和

这些字段和 IPv4/IPv6 无关，完全复用。
```

### 9.2 伪首部校验和需要适配

```
TCP/UDP 的校验和覆盖"伪首部"，伪首部包含 IP 地址：

IPv4 伪首部（12 字节）：
  Source Address (4 bytes)
  Destination Address (4 bytes)
  Zero (1 byte)
  Protocol (1 byte)
  TCP/UDP Length (2 bytes)

IPv6 伪首部（40 字节）：
  Source Address (16 bytes)
  Destination Address (16 bytes)
  Upper-Layer Packet Length (4 bytes)
  Zeros (3 bytes)
  Next Header (1 byte) = 6(TCP) / 17(UDP) / 58(ICMPv6)

差异：
  地址从 4 字节 → 16 字节
  长度字段从 2 字节 → 4 字节
  协议号位置变了

你的 checksum 算法（sum_every_16bits）完全不用改，
只需要把不同长度的伪首部喂给它就行。
```

### 9.3 Socket API 扩展

```
IPv4 socket 编程：
  struct sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_port = htons(80);
  addr.sin_addr.s_addr = inet_addr("10.0.0.5");
  connect(fd, (struct sockaddr *)&addr, sizeof(addr));

IPv6 socket 编程：
  struct sockaddr_in6 addr;
  addr.sin6_family = AF_INET6;
  addr.sin6_port = htons(80);
  inet_pton(AF_INET6, "2001:db8::1", &addr.sin6_addr);
  connect(fd, (struct sockaddr *)&addr, sizeof(addr));

你需要在 liblevelip.so 中：
  1. 拦截 AF_INET6 的 socket() 调用
  2. 把 sockaddr_in6（28 字节）通过 IPC 传给守护进程
  3. 守护进程内部用 IPv6 路径处理
```

---

## 10. 实现路线图

### Phase 1：IPv6 数据面（对应本文 §3-§4）

```
学习目标：理解 IPv6 首部结构、扩展首部链、收发路径

实现内容：
  ipv6.h        — IPv6 首部结构体 + 常量定义
  ipv6_input.c  — 收包：固定首部解析 + 扩展首部链遍历 + 分发
  ipv6_output.c — 发包：填充 40 字节首部 + 发送
  ipv6_exthdr.c — 扩展首部链遍历器

验证方法：
  构造 IPv6 Echo Request → 协议栈收到 → 解析首部 → 打印日志
  协议栈构造 IPv6 Echo Reply → 发送 → tcpdump 抓到
```

### Phase 2：ICMPv6 + NDP（对应本文 §5-§6）

```
学习目标：理解 NDP 替代 ARP 的过程、邻居缓存状态机

实现内容：
  icmpv6.c  — ICMPv6 基础框架 + Echo + 校验和（含伪首部）
  ndp.c     — NS/NA 发送接收 + 邻居缓存 + 状态机

验证方法：
  ping6 → 协议栈触发 NDP 地址解析 → NS/NA 交互 → ping6 通
  观察邻居缓存状态变迁：INCOMPLETE → REACHABLE → STALE
```

### Phase 3：SLAAC（对应本文 §7）

```
学习目标：理解去中心化地址配置、DAD 过程

实现内容：
  ipv6_addrconf.c — 链路本地地址生成 + RS/RA + DAD

验证方法：
  协议栈启动 → 自动生成 fe80:: 地址 → ping6 链路本地通
  配置路由器发 RA → 协议栈自动获得全局地址
```

### Phase 4：TCP over IPv6（对应本文 §9）

```
学习目标：理解协议栈分层适配、伪首部校验和

实现内容：
  sock.h 联合体扩展 — saddr/daddr 支持 v4/v6
  tcp.c 适配 — 伪首部校验和参数化
  socket.c 适配 — AF_INET6 地址族
  liblevelip.c 适配 — 拦截 AF_INET6

验证方法：
  curl -6 http://[ipv6-address] → 通过协议栈建立 TCP IPv6 连接
```

### Phase 5：PMTUD + 集成测试（对应本文 §8）

```
学习目标：理解 PMTUD 工作原理、端到端调试

实现内容：
  ICMPv6 Packet Too Big 处理
  TCP MSS 根据 PMTU 动态调整

验证方法：
  tc 工具模拟小 MTU 链路 → 观察 TCP MSS 自动调整
```

---

## 参考文献

| RFC | 内容 | 对应 Phase |
|---|---|---|
| RFC 8200 | IPv6 协议规范 | Phase 1 |
| RFC 4443 | ICMPv6 | Phase 2 |
| RFC 4861 | 邻居发现协议（NDP） | Phase 2 |
| RFC 4862 | SLAAC | Phase 3 |
| RFC 8201 | 路径 MTU 发现 | Phase 5 |
| RFC 8027 | PMTUD 黑洞处理 | Phase 5 |
| RFC 6437 | Flow Label | 扩展 |
| RFC 2474 | DSCP | 扩展 |
