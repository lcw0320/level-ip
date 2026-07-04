#include "syshead.h"
#include "ipv6.h"
#include "dst.h"
#include "route.h"
#include "netdev.h"
#include "skbuff.h"

/*
 * IPv6 output entry point (04 §3.4.2).
 *
 * Caller places the upper-layer payload in skb and sets skb->protocol.
 * This function:
 *   1. Looks up the IPv6 route for daddr.
 *   2. Pushes the 40-byte fixed header.
 *   3. Fills in the fixed header fields.
 *   4. Calls dst6_neigh_output() for link-layer delivery.
 *
 * Note: struct sock does not carry IPv6 addresses until T20 (sock union).
 *       Therefore the signature takes explicit saddr/daddr parameters
 *       instead of struct sock *.  T24 will adapt the TCP path accordingly.
 */
int ipv6_output(struct sk_buff *skb, uint8_t nexthdr,
                const struct in6_addr *saddr,
                const struct in6_addr *daddr)
{
    struct rtentry *rt = NULL;
    struct ipv6hdr *ip6h = NULL;

    /* 1. IPv6 route lookup */
    rt = route6_lookup(daddr);
    if (rt == NULL) {
        print_err("ipv6_output: route lookup failed\n");
        free_skb(skb);
        return -1;
    }

    skb->dev = rt->dev;
    skb->rt = rt;

    /* 2. Reserve headroom for the IPv6 fixed header */
    skb_push(skb, IPV6_HDR_LEN);

    /* 3. Fill the 40-byte fixed header (RFC 8200 §3) */
    ip6h = (struct ipv6hdr *)skb->data;
    ipv6_hdr_set_vtc_flow(ip6h, IPV6_VERSION, 0, 0);
    ip6h->payload_len = htons((uint16_t)(skb->len - IPV6_HDR_LEN));
    ip6h->nexthdr = nexthdr;
    ip6h->hop_limit = IPV6_DEFAULT_HOPLIMIT;

    /* Source address: caller-supplied, or auto-select from netdev */
    if (saddr != NULL && !ipv6_addr_is_unspecified(saddr)) {
        memcpy(&ip6h->saddr, saddr, sizeof(struct in6_addr));
    } else if (skb->dev->addr6_global_valid) {
        memcpy(&ip6h->saddr, &skb->dev->addr6_global,
               sizeof(struct in6_addr));
    } else {
        memcpy(&ip6h->saddr, &skb->dev->addr6_ll,
               sizeof(struct in6_addr));
    }

    memcpy(&ip6h->daddr, daddr, sizeof(struct in6_addr));

    ipv6_dbg("out", ip6h);

    /* 4. Link-layer delivery via NDP (stub) / broadcast fallback */
    return dst6_neigh_output(skb);
}
