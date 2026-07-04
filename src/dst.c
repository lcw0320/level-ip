#include "syshead.h"
#include "dst.h"
#include "ip.h"
#include "ipv6.h"
#include "arp.h"
#include "ndp.h"

/* IPv4 neighbour output — unchanged logic, gateway field updated to union */
int dst_neigh_output(struct sk_buff *skb)
{
    struct iphdr *iphdr = ip_hdr(skb);
    struct netdev *netdev = skb->dev;
    struct rtentry *rt = skb->rt;
    uint32_t daddr = ntohl(iphdr->daddr);
    uint32_t saddr = ntohl(iphdr->saddr);

    uint8_t *dmac;

    if (rt->flags & RT_GATEWAY) {
        daddr = rt->gateway.v4;
    }

    dmac = arp_get_hwaddr(daddr);

    if (dmac) {
        return netdev_transmit(skb, dmac, ETH_P_IP);
    } else {
        arp_request(saddr, daddr, netdev);

        /* Inform upper layer that traffic was not sent, retry later */
        return -1;
    }
}

/*
 * IPv6 neighbour output.
 *
 * NDP (Neighbour Discovery Protocol) replaces ARP for IPv6.
 * On cache hit the packet is transmitted immediately.
 * On cache miss a Neighbor Solicitation is sent and the packet is
 * queued in the neighbour entry until resolution completes.
 */
int dst6_neigh_output(struct sk_buff *skb)
{
    struct ipv6hdr *ip6h = NULL;
    struct rtentry *rt = NULL;
    struct ndp_entry *entry = NULL;
    struct in6_addr next_hop;
    struct in6_addr src_addr;
    uint8_t *dmac = NULL;

    memset(&next_hop, 0, sizeof(next_hop));
    memset(&src_addr, 0, sizeof(src_addr));

    ip6h = (struct ipv6hdr *)skb->data;
    rt = skb->rt;

    /* Copy saddr out of packed struct to avoid alignment issues */
    memcpy(&src_addr, &ip6h->saddr, sizeof(struct in6_addr));

    /* 1. Determine next-hop address (gateway or final destination) */
    if (rt->flags & RT_GATEWAY) {
        memcpy(&next_hop, &rt->gateway.v6, sizeof(struct in6_addr));
    } else {
        memcpy(&next_hop, &ip6h->daddr, sizeof(struct in6_addr));
    }

    /* 2. NDP neighbour cache lookup */
    dmac = ndp_get_hwaddr(&next_hop);
    if (dmac != NULL) {
        return netdev_transmit(skb, dmac, ETH_P_IPV6);
    }

    /* 3. Cache miss — trigger NS resolution and queue the packet */
    ndp_send_ns(&next_hop, &src_addr, skb->dev);

    entry = ndp_lookup(&next_hop);
    if (entry == NULL) {
        entry = ndp_entry_alloc(&next_hop);
    }

    if (entry != NULL) {
        ndp_queue_skb(&next_hop, skb);
    }

    return -1;
}
