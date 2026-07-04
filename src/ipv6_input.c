#include "ipv6.h"
#include "icmpv6.h"
#include "tcp.h"
#include "skbuff.h"
#include "utils.h"

/*
 * Upper-layer protocol stubs.
 * UDP stub still frees the skb; TCP is now handled by tcp_in_v6().
 */

static void udp_in_v6_stub(struct sk_buff *skb)
{
    print_err("IPv6: UDP-v6 handler not yet implemented\n");
    free_skb(skb);
}

int ipv6_rcv(struct sk_buff *skb)
{
    struct ipv6hdr *ip6h = NULL;
    struct ipv6_exthdr_result ext_result;
    uint16_t plen = 0;
    int ret = 0;

    ip6h = ipv6_hdr(skb);

    printf("ipv6h: version=%d, traffic_class=%d, flow_label=%d, payload_len=%d, nexthdr=%d, hop_limit=%d\n",
           ipv6_hdr_version(ip6h), ipv6_hdr_tclass(ip6h), ipv6_hdr_flowlabel(ip6h),
           ntohs(ip6h->payload_len), ip6h->nexthdr, ip6h->hop_limit);
    /* 1. Validate version (RFC 8200 §3) */
    if (ipv6_hdr_version(ip6h) != IPV6_VERSION) {
        print_err("IPv6: version mismatch %d\n", ipv6_hdr_version(ip6h));
        goto drop_pkt;
    }

    /* 2. Validate payload length against skb */
    plen = ntohs(ip6h->payload_len);
    if (plen + IPV6_HDR_LEN > skb->len) {
        print_err("IPv6: payload_len %u + 40 exceeds skb len %u\n",
                  plen, skb->len);
        goto drop_pkt;
    }

    /* 3. Validate hop_limit (RFC 8200: must drop if 0) */
    if (ip6h->hop_limit == 0) {
        /* icmpv6_send_time_exceeded frees skb internally */
        icmpv6_send_time_exceeded(skb);
        return 0;
    }

    ipv6_dbg("in", ip6h);

    /* 4. Walk the extension header chain */
    memset(&ext_result, 0, sizeof(ext_result));
    ret = ipv6_parse_exthdrs(skb, ip6h->nexthdr,
                             (uint8_t *)(ip6h + 1),
                             plen,
                             &ext_result);
    if (ret < 0) {
        print_err("IPv6: extension header parse error\n");
        goto drop_pkt;
    }

    /* 5. Fragment reassembly not supported — drop non-first fragments */
    if (ext_result.has_frag && (ext_result.frag_off != 0)) {
        print_err("IPv6: fragment reassembly not supported, dropping\n");
        goto drop_pkt;
    }

    /* 6. Dispatch to upper-layer protocol */
    switch (ext_result.final_nexthdr) {
    case NEXTHDR_ICMPV6:
        icmpv6_incoming(skb, (uint8_t *)(ip6h + 1) + ext_result.consumed);
        return 0;
    case NEXTHDR_TCP:
        tcp_in_v6(skb, (uint8_t *)(ip6h + 1) + ext_result.consumed);
        return 0;
    case NEXTHDR_UDP:
        udp_in_v6_stub(skb);
        return 0;
    case NEXTHDR_NONE:
        /* RFC 8200 §4.7: no next header — nothing to dispatch */
        free_skb(skb);
        return 0;
    default:
        print_err("IPv6: unsupported next header %d\n",
                  ext_result.final_nexthdr);
        goto drop_pkt;
    }

drop_pkt:
    free_skb(skb);
    return 0;
}
