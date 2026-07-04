#include "syshead.h"
#include "utils.h"
#include "ipv6.h"
#include "icmpv6.h"
#include "ndp.h"
#include "route.h"
#include "socket.h"

/*
 * Maximum bytes of the offending packet to include in an ICMPv6 error
 * message.  RFC 4443 §3: error message must not exceed 1280 bytes total
 * (8-byte ICMPv6 header + up to 1272 bytes of original packet).
 */
#define ICMPV6_ERROR_MAX_PAYLOAD 1272

/*
 * icmpv6_handle_error - Process a received ICMPv6 error message.
 * @skb:   the error packet (freed by this function)
 * @ip6h:  IPv6 header of the error packet
 * @icmph: ICMPv6 header of the error message
 *
 * Currently logs type/code/body for diagnostics.
 * T29 will extend this to extract MTU for PMTUD (Packet Too Big).
 */
void icmpv6_handle_error(struct sk_buff *skb, struct ipv6hdr *ip6h,
                         struct icmpv6_hdr *icmph)
{
    (void)ip6h;
    print_err("ICMPv6 error: type=%d code=%d body=0x%08x\n",
              icmph->type, icmph->code, ntohl(icmph->body));
    free_skb(skb);
}

/*
 * icmpv6_pmtu_update - Process Packet Too Big for PMTUD (RFC 8201).
 * @skb:   the ICMPv6 error packet (freed by this function)
 * @ip6h:  IPv6 header of the error packet (source = router reporting the issue)
 * @icmph: ICMPv6 header with type=2; body contains next-hop MTU in network order
 *
 * Extracts the MTU from the ICMPv6 body and the destination address from the
 * embedded original IPv6 header, then updates the route's cached PMTU.
 */
void icmpv6_pmtu_update(struct sk_buff *skb, struct ipv6hdr *ip6h,
                        struct icmpv6_hdr *icmph)
{
    uint32_t mtu = 0;
    struct ipv6hdr *orig_ip6h = NULL;
    struct in6_addr orig_daddr;
    uint8_t *embedded = NULL;
    unsigned int payload_offset = 0;

    (void)ip6h;

    mtu = ntohl(icmph->body);

    /* The embedded original packet starts right after the ICMPv6 header */
    embedded = (uint8_t *)icmph + sizeof(struct icmpv6_hdr);
    payload_offset = sizeof(struct icmpv6_hdr);

    /* Verify we have at least an IPv6 header (40 bytes) in the payload */
    if (skb->len < ETH_HDR_LEN + IPV6_HDR_LEN + payload_offset + IPV6_HDR_LEN) {
        print_err("ICMPv6 PTB: embedded packet too short (%u bytes)\n", skb->len);
        free_skb(skb);
        return;
    }

    /* Extract the destination address from the embedded original IPv6 header */
    orig_ip6h = (struct ipv6hdr *)embedded;
    memcpy(&orig_daddr, &orig_ip6h->daddr, sizeof(struct in6_addr));

    print_debug("ICMPv6 PTB: MTU=%u for dst=%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                "%02x%02x:%02x%02x:%02x%02x:%02x%02x\n",
                mtu,
                orig_daddr.s6_addr[0], orig_daddr.s6_addr[1],
                orig_daddr.s6_addr[2], orig_daddr.s6_addr[3],
                orig_daddr.s6_addr[4], orig_daddr.s6_addr[5],
                orig_daddr.s6_addr[6], orig_daddr.s6_addr[7],
                orig_daddr.s6_addr[8], orig_daddr.s6_addr[9],
                orig_daddr.s6_addr[10], orig_daddr.s6_addr[11],
                orig_daddr.s6_addr[12], orig_daddr.s6_addr[13],
                orig_daddr.s6_addr[14], orig_daddr.s6_addr[15]);

    route6_update_pmtu(&orig_daddr, (uint16_t)mtu);

    /* Notify TCP sockets to adjust MSS for this destination */
    socket_adjust_pmtu6(&orig_daddr, (uint16_t)mtu);

    free_skb(skb);
}

/*
 * icmpv6_send_error - Build and send an ICMPv6 error message.
 * @skb:      offending packet that triggered the error (freed here)
 * @type:     ICMPv6 error type (1, 2, or 3)
 * @code:     ICMPv6 code
 * @type_body type-specific 32-bit field (network byte order)
 *
 * Constructs: [ICMPv6 hdr(8)] + [orig IPv6 hdr(40) + payload (≤1272)]
 * Sends back to the original source address via ipv6_output().
 */
static void icmpv6_send_error(struct sk_buff *skb, uint8_t type,
                              uint8_t code, uint32_t type_body)
{
    struct ipv6hdr *orig_ip6h = NULL;
    struct sk_buff *err = NULL;
    struct icmpv6_hdr *err_icmph = NULL;
    struct in6_addr err_saddr;
    struct in6_addr err_daddr;
    unsigned int copy_len = 0;
    unsigned int icmpv6_len = 0;
    unsigned int total_size = 0;
    uint8_t *p = NULL;

    orig_ip6h = ipv6_hdr(skb);

    /* Include IPv6 header + as much payload as fits in 1280 bytes */
    copy_len = skb->len - ETH_HDR_LEN;
    if (copy_len > ICMPV6_ERROR_MAX_PAYLOAD) {
        copy_len = ICMPV6_ERROR_MAX_PAYLOAD;
    }

    icmpv6_len = sizeof(struct icmpv6_hdr) + copy_len;
    total_size = ETH_HDR_LEN + IPV6_HDR_LEN + icmpv6_len;

    err = alloc_skb(total_size);
    if (err == NULL) {
        print_err("ICMPv6: alloc_skb failed for error type=%d\n", type);
        free_skb(skb);
        return;
    }

    /* Reserve headroom for ETH + IPv6, then write ICMPv6 data */
    skb_reserve(err, ETH_HDR_LEN + IPV6_HDR_LEN);
    p = skb_push(err, icmpv6_len);

    /* Fill ICMPv6 header */
    err_icmph = (struct icmpv6_hdr *)p;
    err_icmph->type = type;
    err_icmph->code = code;
    err_icmph->csum = 0;
    err_icmph->body = type_body;

    /* Copy original packet (IPv6 header + payload) after ICMPv6 header */
    memcpy(p + sizeof(struct icmpv6_hdr),
           skb->head + ETH_HDR_LEN, copy_len);

    /* Compute checksum over the full ICMPv6 error message */
    memcpy(&err_saddr, &orig_ip6h->daddr, sizeof(struct in6_addr));
    memcpy(&err_daddr, &orig_ip6h->saddr, sizeof(struct in6_addr));

    err_icmph->csum = icmpv6_checksum(&err_saddr, &err_daddr,
                                      p, (uint16_t)icmpv6_len);

    free_skb(skb);

    /* Send error message back to original source */
    err->protocol = ETH_P_IPV6;
    ipv6_output(err, NEXTHDR_ICMPV6, &err_saddr, &err_daddr);
}

uint16_t icmpv6_checksum(struct in6_addr *saddr, struct in6_addr *daddr,
                         uint8_t *data, uint16_t len)
{
    uint32_t sum = 0;
    /*
     * Build the IPv6 pseudo-header in a buffer (network byte order)
     * and compute its checksum with sum_every_16bits.  This avoids
     * endianness pitfalls with adding host-order values.
     *
     * Pseudo-header layout (40 bytes, RFC 8200 §8.1):
     *   Source Address       : 16 bytes
     *   Destination Address  : 16 bytes
     *   Upper-Layer Length   :  4 bytes (network order)
     *   Zeros (3 bytes) + NH :  4 bytes
     */
    uint8_t pseudo[40];

    memset(pseudo, 0, sizeof(pseudo));
    memcpy(pseudo, saddr->s6_addr, 16);
    memcpy(pseudo + 16, daddr->s6_addr, 16);
    /* Upper-layer length at offset 32 (4 bytes, network order) */
    pseudo[32] = (uint8_t)(len >> 24);
    pseudo[33] = (uint8_t)(len >> 16);
    pseudo[34] = (uint8_t)(len >> 8);
    pseudo[35] = (uint8_t)(len & 0xFF);
    /* pseudo[36..38] = 0 (zeros) */
    /* Next header at offset 39 */
    pseudo[39] = NEXTHDR_ICMPV6;

    /* Sum pseudo-header (40 bytes = 20 x 16-bit words) */
    sum += sum_every_16bits(pseudo, 40);

    /* Sum ICMPv6 message and fold */
    return checksum(data, len, sum);
}

/*
 * icmpv6_incoming - ICMPv6 receive entry point (04 §3.4.4).
 * @skb:     packet buffer (ownership transferred to this function)
 * @payload: first byte of the ICMPv6 message (after IPv6 + ext headers)
 *
 * Validates the checksum (including IPv6 pseudo-header), then dispatches
 * to the appropriate handler based on the ICMPv6 type field.
 */
void icmpv6_incoming(struct sk_buff *skb, uint8_t *payload)
{
    struct icmpv6_hdr *icmph = NULL;
    struct ipv6hdr *ip6h = NULL;
    struct in6_addr saddr;
    struct in6_addr daddr;
    uint16_t csum = 0;
    uint16_t plen = 0;

    ip6h = ipv6_hdr(skb);
    icmph = (struct icmpv6_hdr *)payload;
    plen = ntohs(ip6h->payload_len);

    /* Copy addresses to avoid unaligned access from packed struct */
    memcpy(&saddr, &ip6h->saddr, sizeof(struct in6_addr));
    memcpy(&daddr, &ip6h->daddr, sizeof(struct in6_addr));

    /* 1. Validate ICMPv6 checksum (RFC 8200 §8.1) */
    csum = icmpv6_checksum(&saddr, &daddr, payload, plen);
    if (csum != 0) {
        print_err("ICMPv6: checksum mismatch (got 0x%04x)\n", csum);
        free_skb(skb);
        return;
    }

    icmpv6_dbg("in", icmph);

    /* 2. Dispatch by type */
    switch (icmph->type) {
    case ICMPV6_ECHO_REQUEST:
        icmpv6_echo_reply(skb, ip6h, icmph);
        break;
    case ICMPV6_ECHO_REPLY:
        /* Learning phase: ignore unsolicited echo replies */
        free_skb(skb);
        break;
    case ICMPV6_DST_UNREACH:
    case ICMPV6_TIME_EXCEEDED:
        icmpv6_handle_error(skb, ip6h, icmph);
        break;
    case ICMPV6_PKT_TOOBIG:
        icmpv6_pmtu_update(skb, ip6h, icmph);
        break;
    case ICMPV6_ROUTER_SOLICIT:
    case ICMPV6_ROUTER_ADVERT:
    case ICMPV6_NEIGHBOR_SOLICIT:
    case ICMPV6_NEIGHBOR_ADVERT:
    case ICMPV6_REDIRECT:
        ndp_incoming(skb, ip6h, icmph);
        break;
    default:
        print_err("ICMPv6: unknown type %d\n", icmph->type);
        free_skb(skb);
        break;
    }
}

/*
 * icmpv6_echo_reply - Send an Echo Reply in response to an Echo Request.
 * @skb:   incoming Echo Request packet (freed by this function)
 * @ip6h:  IPv6 header of the incoming packet
 * @icmph: ICMPv6 header of the incoming Echo Request
 *
 * Allocates a new skb, copies the ICMPv6 payload with type changed to
 * ECHO_REPLY (129), swaps source/destination addresses, recalculates
 * the checksum, and sends via ipv6_output().
 */
void icmpv6_echo_reply(struct sk_buff *skb, struct ipv6hdr *ip6h,
                       struct icmpv6_hdr *icmph)
{
    struct sk_buff *reply = NULL;
    struct icmpv6_hdr *reply_icmph = NULL;
    struct in6_addr reply_saddr;
    struct in6_addr reply_daddr;
    struct netdev *dev = NULL;
    uint16_t icmpv6_len = 0;
    unsigned int total_size = 0;

    icmpv6_len = ntohs(ip6h->payload_len);

    /* Save dev before freeing original skb */
    dev = skb->dev;

    /* Swap addresses: reply src = incoming dst, reply dst = incoming src */
    memcpy(&reply_saddr, &ip6h->daddr, sizeof(struct in6_addr));
    memcpy(&reply_daddr, &ip6h->saddr, sizeof(struct in6_addr));

    /* Allocate skb with headroom for ETH + IPv6 headers */
    total_size = ETH_HDR_LEN + IPV6_HDR_LEN + icmpv6_len;
    reply = alloc_skb(total_size);
    if (reply == NULL) {
        print_err("ICMPv6: alloc_skb failed for echo reply\n");
        free_skb(skb);
        return;
    }

    /* Reserve headroom for ETH + IPv6 headers, then append ICMPv6 data */
    skb_reserve(reply, ETH_HDR_LEN + IPV6_HDR_LEN);
    memcpy(reply->data, (uint8_t *)icmph, icmpv6_len);
    reply->len = icmpv6_len;

    /* Change type to Echo Reply */
    reply_icmph = (struct icmpv6_hdr *)reply->data;
    reply_icmph->type = ICMPV6_ECHO_REPLY;
    reply_icmph->csum = 0;
    reply_icmph->csum = icmpv6_checksum(&reply_saddr, &reply_daddr,
                                        reply->data, icmpv6_len);

    /* Free the original request packet */
    free_skb(skb);

    /* Send reply via IPv6 output path */
    reply->protocol = ETH_P_IPV6;
    reply->dev = dev;
    ipv6_output(reply, NEXTHDR_ICMPV6, &reply_saddr, &reply_daddr);
}

/*
 * icmpv6_send_dst_unreach - Send Destination Unreachable (type=1).
 * @skb:  offending packet that could not be delivered (freed here)
 * @code: reason code (0=no route, 1=admin prohibited, 3=address unreachable,
 *        4=port unreachable, etc. per RFC 4443 §3.1)
 */
void icmpv6_send_dst_unreach(struct sk_buff *skb, uint8_t code)
{
    icmpv6_send_error(skb, ICMPV6_DST_UNREACH, code, 0);
}

/*
 * icmpv6_send_pkt_toobig - Send Packet Too Big (type=2).
 * @skb: oversized packet that triggered the error (freed here)
 * @mtu: next-hop MTU in host byte order (placed in the 32-bit body field
 *       in network byte order per RFC 4443 §3.2)
 */
void icmpv6_send_pkt_toobig(struct sk_buff *skb, uint32_t mtu)
{
    icmpv6_send_error(skb, ICMPV6_PKT_TOOBIG, 0, htonl(mtu));
}

/*
 * icmpv6_send_time_exceeded - Send Time Exceeded (type=3).
 * @skb: packet whose hop_limit reached zero (freed here)
 *
 * Called from ipv6_rcv() when a received packet has hop_limit == 0
 * (RFC 8200 §3 requires such packets to be discarded).
 */
void icmpv6_send_time_exceeded(struct sk_buff *skb)
{
    icmpv6_send_error(skb, ICMPV6_TIME_EXCEEDED, 0, 0);
}
