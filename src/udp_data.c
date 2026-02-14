#include "syshead.h"
#include "ip.h"
#include "udp.h"
#include "list.h"

void build_sockaddr_from_host_order(
    uint16_t srcport,          // 网络字节序端口
    uint32_t srcaddr,          // 网络字节序 IPv4 地址
    struct sockaddr *addr,     // 输出：sockaddr 指针
    socklen_t *addr_len              // 输出：地址结构长度指针
) {
    struct sockaddr_in *sin = (struct sockaddr_in *)addr;

    memset(sin, 0, sizeof(*sin));

    sin->sin_family = AF_INET;
    sin->sin_port   = htons(srcport);          
    sin->sin_addr.s_addr = htonl(srcaddr);      

    *addr_len = sizeof(struct sockaddr_in);
}

int udp_data_dequeue(struct udp_sock *usk, void *user_buf, int userlen, struct sockaddr *addr, socklen_t *addr_len)
{
    struct sock *sk = &usk->sk;
    struct udphdr *uhdr;
    struct iphdr *ihdr;
    uint16_t srcport = 0;
    uint32_t srcaddr = 0;
    int rlen = 0;

    if (!skb_queue_empty(&sk->receive_queue)) {
        struct sk_buff *skb = skb_peek(&sk->receive_queue);
        if (skb == NULL) return 0;
        
        uhdr = udp_hdr(skb);
        srcport = uhdr->sport;
        ihdr = ip_hdr(skb);
        srcaddr = ihdr->saddr;
        build_sockaddr_from_host_order(srcport, srcaddr, addr, addr_len);

        /* Guard datalen to not overflow userbuf */
        int dlen = skb->dlen > userlen ? userlen : skb->dlen;
        memcpy(user_buf, skb->payload, dlen);
        rlen = skb->dlen;
        print_debug("udp dequeue receive_queue\n");

        skb_dequeue(&sk->receive_queue);
        skb->refcnt--;
        free_skb(skb);
    } else {
        return 0;
    }

    if (skb_queue_empty(&sk->receive_queue)) {
        sk->poll_events &= ~POLLIN;
    }
    
    return rlen;
}

int udp_data_queue(struct udp_sock *usk, struct udphdr *th, struct sk_buff *skb)
{
    struct sock *sk = &usk->sk;
    int rc = 0;

    skb->refcnt++;
    skb_queue_tail(&sk->receive_queue, skb);

    print_debug("udp packet enqueue receive_queue\n");
    // There is new data for user to read
    sk->poll_events |= (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND);
    usk->sk.ops->recv_notify(&usk->sk);
    
    return rc;
}
