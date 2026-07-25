#include "syshead.h"
#include "ipv6.h"
#include "dst.h"
#include "route.h"
#include "netdev.h"
#include "skbuff.h"

/*
 * IPv6 output entry point (04 §3.4.2).
 *
 * @headroom_reserved: if non-zero, caller already pushed space for the
 *   IPv6 header (e.g. TCP TX path).  ipv6_output will skip skb_push and
 *   fill the header in-place at skb->data.
 */
int ipv6_output(struct sk_buff *skb, uint8_t nexthdr,
                const struct in6_addr *saddr,
                const struct in6_addr *daddr)
{
    return ipv6_output_ex(skb, nexthdr, saddr, daddr, 0);
}

int ipv6_output_ex(struct sk_buff *skb, uint8_t nexthdr,
                   const struct in6_addr *saddr,
                   const struct in6_addr *daddr,
                   int headroom_reserved)
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

    /* 2. Reserve headroom for the IPv6 fixed header (skip if caller did it) */
    if (!headroom_reserved) {
        skb_push(skb, IPV6_HDR_LEN);
    }

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
        /* Write back the resolved source so TCP checksum uses the real addr */
        if (saddr != NULL) {
            memcpy((struct in6_addr *)saddr, &skb->dev->addr6_global,
                   sizeof(struct in6_addr));
        }
    } else {
        memcpy(&ip6h->saddr, &skb->dev->addr6_ll,
               sizeof(struct in6_addr));
        if (saddr != NULL) {
            memcpy((struct in6_addr *)saddr, &skb->dev->addr6_ll,
                   sizeof(struct in6_addr));
        }
    }

    memcpy(&ip6h->daddr, daddr, sizeof(struct in6_addr));

    ipv6_dbg("out", ip6h);

    /* 4. Link-layer delivery via NDP (stub) / broadcast fallback */
    return dst6_neigh_output(skb);
}
