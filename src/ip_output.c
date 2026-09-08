#include "syshead.h"
#include "skbuff.h"
#include "utils.h"
#include "udp.h"
#include "tcp.h"
#include "ip.h"
#include "dst.h"
#include "route.h"

void ip_send_check(struct iphdr *ihdr)
{
    uint32_t csum = checksum(ihdr, ihdr->ihl * 4, 0);
    ihdr->csum = csum;
}

/* IPv4 pseudo-header checksum, address part, folded on top of skb->tcpcsum */
static int tcp_v4_ip_partion_checksum(uint32_t saddr, uint32_t daddr, uint32_t csum)
{
    uint8_t pseudo[8] = {0};

    memcpy(pseudo, &saddr, 4);
    memcpy(pseudo + 4, &daddr, 4);

    return checksum(pseudo, 8, csum);
}

int ip_output(struct sock *sk, struct sk_buff *skb)
{
    struct rtentry *rt;
    struct iphdr *ihdr = ip_hdr(skb);
    struct tcphdr *thdr = NULL;

    rt = route_lookup(sk->daddr.v4);

    if (!rt) {
        // TODO: dest_unreachable
        print_err("IP output route lookup fail\n");
        return -1;
    }

    skb->dev = rt->dev;
    skb->rt = rt;

    skb_push(skb, IP_HDR_LEN);

    ihdr->version = IPV4;
    ihdr->ihl = 0x05;
    ihdr->tos = 0;
    ihdr->len = skb->len;
    ihdr->id = ihdr->id;
    ihdr->frag_offset = 0x4000;
    ihdr->ttl = 64;
    ihdr->proto = skb->protocol;
    ihdr->saddr = skb->dev->addr;
    ihdr->daddr = sk->daddr.v4;
    ihdr->csum = 0;

    ip_dbg("out", ihdr);

    ihdr->len = htons(ihdr->len);
    ihdr->id = htons(ihdr->id);
    ihdr->daddr = htonl(ihdr->daddr);
    ihdr->saddr = htonl(ihdr->saddr);
    ihdr->csum = htons(ihdr->csum);
    ihdr->frag_offset = htons(ihdr->frag_offset);

    if (ihdr->proto == IP_UDP) {
        struct udphdr *udphdr = udp_hdr(skb);
        udphdr->csum = calcuate_udp_checksum(ihdr->saddr, ihdr->daddr, udphdr);
    } else if (ihdr->proto == IP_TCP) {
        thdr = (struct tcphdr *)(skb->data + IP_HDR_LEN);
        thdr->csum = tcp_v4_ip_partion_checksum(ihdr->saddr, ihdr->daddr, skb->tcpcsum);
    }

    ip_send_check(ihdr);

    return dst_neigh_output(skb);
}
