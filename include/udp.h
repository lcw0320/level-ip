#ifndef UDP_H_
#define UDP_H_
#include "syshead.h"
#include "ip.h"
#include "utils.h"

#define UDP_HDR_LEN sizeof(struct udphdr)

#define udp_sk(sk) ((struct udp_sock *)sk)
#define udp_dlen(udp) (udp->length << 2 - 32)

#ifdef DEBUG_UDP
#define udp_in_dbg(hdr, sk, skb)                                        \
    do {                                                                \
        print_debug("UDP in %u > %u: checksum: %04x", \
                    ntohs(hdr->dport), ntohs(hdr->sport), hdr->csum); \
    } while (0) 

#define udp_out_dbg(hdr, sk, skb)                                       \
    do {                                                                \
        print_debug("UDP out %u > %u: checksum: %04x", \
                    sk->sport, sk->dport, hdr->csum); \
    } while (0)

#define udpsock_dbg(msg, sk)                                            \
    do {                                                                \
        print_debug("UDP x:%u > %u.%u.%u.%u.%u %s: "msg, \
                    sk->sport, (uint8_t)(sk->daddr >> 24), (uint8_t)(sk->daddr >> 16), (uint8_t)(sk->daddr >> 8), (uint8_t)(sk->daddr >> 0), \
                    sk->dport);                         \
    } while (0)
#else
#define udp_in_dbg(hdr, sk, skb)
#define udp_out_dbg(hdr, sk, skb)
#define udpsock_dbg(msg, sk)
#endif

struct udphdr {
    uint16_t sport;
    uint16_t dport;
    uint16_t length;
    uint16_t csum;
    uint8_t data[];
} __attribute__((packed));

struct udp_sock {
    struct sock sk;        

};

static inline struct udphdr *udp_hdr(const struct sk_buff *skb)
{
    return (struct udphdr *)(skb->head + ETH_HDR_LEN + IP_HDR_LEN);
}

void udp_in(struct sk_buff *skb);
int udp_checksum_right(struct iphdr *ihdr, struct udphdr *thdr);
uint16_t calcuate_udp_checksum(uint32_t saddr, uint32_t daddr, struct udphdr *uhdr);

struct sock *udp_alloc_sock();
int udp_init_sock(struct sock *sk);
int udp_v4_init_sock(struct sock *sk);
int udp_connect(struct sock *sk);
int udp_v4_connect(struct sock *sk, const struct sockaddr *addr, socklen_t addrlen, int flags);
int udp_disconnect(struct sock *sk, int flags);
int udp_write(struct sock *sk, const void *buf, int len);
int udp_read(struct sock *sk, void *buf, int len);
int udp_recv_notify(struct sock *sk);
int udp_close(struct sock *sk);
int udp_abort(struct sock *sk);
int udp_bind(struct sock *sk, const struct sockaddr *addr, socklen_t addr_len);
int udp_sendto(struct sock *sk, const void *buf, int len, int flags, const struct sockaddr *addr, socklen_t addr_len);
int udp_recvfrom(struct sock *sk, void *buf, int len, int flags, struct sockaddr *addr, socklen_t *addr_len);

int udp_send(struct udp_sock *usk, const void *buf, int len);
int udp_receive(struct udp_sock *usk, void *buf, int len, int flags, struct sockaddr *addr, socklen_t *addr_len);

int udp_out_send(struct udp_sock *usk, const void *buf, int len);
#endif
