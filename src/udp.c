#include "udp.h"
#include "udp_data.h"
#include "inet.h"

struct udp_pseudo_hdr {
    uint32_t src_ip;
    uint32_t dst_ip;
    uint8_t  zero;      // always 0
    uint8_t  proto;     // 17 for UDP
    uint16_t udp_len;   // in network byte order
}__attribute__((packed));

struct net_ops udp_ops = {
    .alloc_sock = &udp_alloc_sock,
    .init = &udp_v4_init_sock,
    .connect = &udp_v4_connect,
    .disconnect = &udp_disconnect,
    .write = &udp_write,
    .read = &udp_read,
    .recv_notify = &udp_recv_notify,
    .close = &udp_close,
    .abort = &udp_abort,
    .bind = &udp_bind,
    .sendto = &udp_sendto,
    .recvfrom = &udp_recvfrom
};

void udp_init_head(struct udphdr* uhdr, struct sk_buff *skb)
{
    uhdr->dport = ntohs(uhdr->dport);
    uhdr->sport = ntohs(uhdr->sport);
    uhdr->length = ntohs(uhdr->length);
    skb->dlen = uhdr->length - UDP_HDR_LEN;
    skb->payload = uhdr->data;
}

void udp_in(struct sk_buff *skb)
{
    struct sock *sk = NULL;
    struct iphdr *iph;
    struct udphdr *th;

    iph = ip_hdr(skb);
    th = (struct udphdr*) iph->data;

    if (udp_checksum_right(iph, th) != 1) {
        print_err("UDP checksum error, discard\n");
        goto discard;
    }
    udp_init_head(th, skb);
    sk = inet_lookup(skb, th->sport, th->dport);

    if (sk == NULL) {
        print_err("No UDP socket for sport %d dport %d\n",
                  th->sport, th->dport);
        free_skb(skb);
        return;
    }
    socket_wr_acquire(sk->sock);

    udp_in_dbg(th, sk, skb);
    udp_data_queue(udp_sk(sk), th, skb);
    socket_release(sk->sock);
    return;

discard:
    free_skb(skb);
    if (sk) {
        socket_release(sk->sock);
    }
    return;
}

uint16_t calcuate_udp_checksum(uint32_t saddr, uint32_t daddr, struct udphdr *uhdr)
{
    // 假设最大 UDP 包不超过 1500 字节（常见 MTU）
    #define MAX_UDP_BUF (sizeof(struct udp_pseudo_hdr) + 1500)
    static uint8_t buf[MAX_UDP_BUF];  // 或用 alloca，但注意栈大小

    struct udp_pseudo_hdr pseudo = {
        .src_ip = saddr,
        .dst_ip = daddr,
        .zero   = 0,
        .proto  = IP_UDP,
        .udp_len = uhdr->length
    };

    size_t total_len = sizeof(pseudo) + ntohs(uhdr->length);

    if (total_len > MAX_UDP_BUF) {
        // 安全兜底（实际 UDP 最大 65535，但通常不会这么大）
        return 0;
    }

    // 拼接：伪头部 + UDP头+数据
    memcpy(buf, &pseudo, sizeof(pseudo));
    memcpy(buf + sizeof(pseudo), uhdr, ntohs(uhdr->length));

    // 临时清零校验和字段（必须！）
    ((struct udphdr *)(buf + sizeof(pseudo)))->csum = 0;

    print_buffer_hexdump(buf, total_len);

    // 直接对整个 buffer 计算校验和
    return checksum(buf, total_len, 0);
}

int udp_checksum_right(struct iphdr *ihdr, struct udphdr *uhdr)
{
    uint32_t checksum = calcuate_udp_checksum(htonl(ihdr->saddr), htonl(ihdr->daddr), uhdr);
    print_debug("cal_checksum: %04x, checkum: %04x\n", checksum, uhdr->csum);
    // 直接对整个 buffer 计算校验和
    return checksum == uhdr->csum;
}

struct sock *udp_alloc_sock()
{
    struct udp_sock *usk = malloc(sizeof(struct udp_sock));

    memset(usk, 0, sizeof(struct udp_sock));
    usk->sk.state = SS_UNCONNECTED;
 
    return (struct sock *)usk;
}

int udp_init_sock(struct sock *sk)
{
    return 0;
}

int udp_v4_init_sock(struct sock *sk)
{
    return udp_init_sock(sk);
}

int udp_connect(struct sock *sk)
{
    return 0;
}

int udp_v4_connect(struct sock *sk, const struct sockaddr *addr, socklen_t addrlen, int flags)
{
    // todo: set dst port and dst address
    sk->state = SS_CONNECTED;
    return udp_connect(sk);
}

int udp_disconnect(struct sock *sk, int flags)
{
    // todo: clear dst port and dst address
    sk->state = SS_UNCONNECTED;
    return 0;
}

int udp_write(struct sock *sk, const void *buf, int len)
{
    struct udp_sock *usk = udp_sk(sk);
    int ret = sk->err;

    if (ret != 0) goto out;

    switch (sk->state) {
    case SS_UNCONNECTED:
    case SS_CONNECTED:
        break;
    default:
        ret = -EBADF;
        goto out;
    }

    return udp_out_send(usk, buf, len);    

out: 
    return ret;
}

int udp_read(struct sock *sk, void *buf, int len)
{
    return 0;
}

int udp_recv_notify(struct sock *sk)
{
    if (!sk) {
        return -1;
    }

    return wait_wakeup(&sk->recv_wait);
}

int udp_close(struct sock *sk)
{
    return 0;
}

int udp_abort(struct sock *sk)
{
    return 0;
}

int udp_bind(struct sock *sk, const struct sockaddr *addr, socklen_t addr_len)
{
    struct udp_sock *usk = udp_sk(sk);
    int ret = 0;
    uint16_t sport = ((struct sockaddr_in *)addr)->sin_port;
    uint32_t saddr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;
    sport = ntohs(sport);
    saddr = ntohl(saddr);

    if (socker_find_protocol_port(sport, IPPROTO_UDP)) {
        ret = EADDRINUSE;
        return ret;
    }
    usk->sk.sport = sport;
    usk->sk.saddr = saddr;

    return ret;
}

int udp_sendto(struct sock *sk, const void *buf, int len, int flags, const struct sockaddr *addr, socklen_t addr_len)
{
    struct udp_sock *usk = udp_sk(sk);
    int ret = sk->err;

    if (ret != 0) goto out;

    uint16_t dport = ((struct sockaddr_in *)addr)->sin_port;
    uint32_t daddr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;

    usk->sk.dport = ntohs(dport);
    usk->sk.daddr = ntohl(daddr);

    return udp_out_send(usk, buf, len);    

out: 
    return ret;
}

int udp_recvfrom(struct sock *sk, void *buf, int len, int flags,
                    struct sockaddr *addr, socklen_t *addr_len)
{
    struct udp_sock *usk = udp_sk(sk);

    return udp_receive(usk, buf, len, flags, addr, addr_len);    
}