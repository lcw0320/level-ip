#include "syshead.h"
#include "utils.h"
#include "udp.h"
#include "udp_data.h"
#include "ip.h"
#include "skbuff.h"
#include "timer.h"

static struct sk_buff *udp_alloc_skb(int size)
{
    int reserved = ETH_HDR_LEN + IP_HDR_LEN + UDP_HDR_LEN + size;
    struct sk_buff *skb = alloc_skb(reserved);

    skb_reserve(skb, reserved);
    skb->protocol = IP_UDP;
    skb->dlen = size;
    skb->seq = 0;

    return skb;
}

static int udp_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    struct udphdr *thdr = udp_hdr(skb);

    skb_push(skb, UDP_HDR_LEN);

    thdr->sport = sk->sport;
    thdr->dport = sk->dport;
    thdr->length = skb->dlen + UDP_HDR_LEN;

    udp_out_dbg(thdr, sk, skb);

    thdr->sport = htons(thdr->sport);
    thdr->dport = htons(thdr->dport);
    thdr->length = htons(thdr->length);
    
    return ip_output(sk, skb);
}

int udp_out_send(struct udp_sock *usk, const void *buf, int len)
{
    struct sk_buff *skb;

    skb = udp_alloc_skb(len);
    skb_push(skb, len);
    memcpy(skb->data, buf, len);

    if (udp_transmit_skb(&usk->sk, skb) <= 0) {
        perror("Error on UDP skb send");
    }
   
    return len;
}