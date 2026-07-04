#ifndef ICMPV6_H
#define ICMPV6_H

#include "syshead.h"
#include "skbuff.h"
#include "ipv6.h"

/*
 * ICMPv6 type values (RFC 4443)
 */

/* Error messages (type 0-127) */
#define ICMPV6_DST_UNREACH      1
#define ICMPV6_PKT_TOOBIG       2
#define ICMPV6_TIME_EXCEEDED    3
#define ICMPV6_PARAM_PROBLEM    4

/* Informational messages (type 128-255) */
#define ICMPV6_ECHO_REQUEST     128
#define ICMPV6_ECHO_REPLY       129

/* NDP messages (RFC 4861, type 133-137) */
#define ICMPV6_ROUTER_SOLICIT   133
#define ICMPV6_ROUTER_ADVERT    134
#define ICMPV6_NEIGHBOR_SOLICIT 135
#define ICMPV6_NEIGHBOR_ADVERT  136
#define ICMPV6_REDIRECT         137

/*
 * NDP option types (RFC 4861 §4.6)
 */
#define NDP_OPT_SLLA    1   /* Source Link-Layer Address */
#define NDP_OPT_TLLA    2   /* Target Link-Layer Address */
#define NDP_OPT_PREFIX  3   /* Prefix Information */
#define NDP_OPT_REDIRECT 4  /* Redirected Header */
#define NDP_OPT_MTU     5   /* MTU */

/*
 * ICMPv6 generic header — 8 bytes (RFC 4443 §2.1)
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |     Type      |     Code      |          Checksum             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                         Message Body                          |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 */
struct icmpv6_hdr {
    uint8_t type;
    uint8_t code;
    uint16_t csum;
    uint32_t body;
} __attribute__((packed));

/*
 * ICMPv6 Echo Request/Reply header (RFC 4443 §4.1)
 *
 * Follows the generic ICMPv6 header; identifier and sequence are
 * after type/code/checksum, then arbitrary data follows.
 */
struct icmpv6_echo_hdr {
    uint16_t id;
    uint16_t seq;
    uint8_t data[];
} __attribute__((packed));

/*
 * NDP option generic header (RFC 4861 §4.6)
 *
 * length is in units of 8 octets, including the type and length fields.
 */
struct ndp_opt_hdr {
    uint8_t type;
    uint8_t length;
    uint8_t data[];
} __attribute__((packed));

/*
 * ICMPv6 debug macro
 */
#ifdef DEBUG_ICMPV6
#define icmpv6_dbg(msg, icmph)                                          \
    do {                                                                \
        print_debug("icmpv6 "msg" (type=%d code=%d csum=0x%04x)",      \
                    (icmph)->type, (icmph)->code, ntohs((icmph)->csum)); \
    } while (0)
#else
#define icmpv6_dbg(msg, icmph)
#endif

/*
 * ICMPv6 checksum — includes IPv6 pseudo-header (RFC 8200 §8.1).
 * @saddr: source IPv6 address
 * @daddr: destination IPv6 address
 * @data:  pointer to the ICMPv6 message
 * @len:   upper-layer packet length (host byte order)
 *
 * Returns the computed checksum; a valid message yields 0.
 */
uint16_t icmpv6_checksum(struct in6_addr *saddr, struct in6_addr *daddr,
                         uint8_t *data, uint16_t len);

/*
 * ICMPv6 receive entry point — dispatches by type.
 * @skb:     packet buffer
 * @payload: first byte of the ICMPv6 message (after IPv6 + ext headers)
 */
void icmpv6_incoming(struct sk_buff *skb, uint8_t *payload);

/*
 * Send an ICMPv6 Echo Reply in response to an Echo Request.
 * @skb:   the incoming Echo Request packet
 * @ip6h:  IPv6 header of the incoming packet
 * @icmph: ICMPv6 header of the incoming Echo Request
 */
void icmpv6_echo_reply(struct sk_buff *skb, struct ipv6hdr *ip6h,
                       struct icmpv6_hdr *icmph);

/*
 * ICMPv6 error message senders (RFC 4443 §3).
 * Each function builds an error message containing the IPv6 header + as
 * much payload as possible from the offending packet (up to 1280 bytes),
 * then sends it back to the original source via ipv6_output().
 *
 * @skb:   the offending packet (freed by each function)
 */
void icmpv6_send_dst_unreach(struct sk_buff *skb, uint8_t code);
void icmpv6_send_pkt_toobig(struct sk_buff *skb, uint32_t mtu);
void icmpv6_send_time_exceeded(struct sk_buff *skb);

/*
 * Handle a received ICMPv6 error message (type 1/2/3).
 * Currently logs type/code/body; T29 will extract MTU for PMTUD.
 * @skb:   the error packet (freed by this function)
 * @ip6h:  IPv6 header of the error packet
 * @icmph: ICMPv6 header of the error message
 */
void icmpv6_handle_error(struct sk_buff *skb, struct ipv6hdr *ip6h,
                         struct icmpv6_hdr *icmph);

/*
 * Process ICMPv6 Packet Too Big for PMTUD (RFC 8201).
 * Extracts the MTU and embedded destination address, updates route PMTU.
 * @skb:   the error packet (freed by this function)
 * @ip6h:  IPv6 header of the error packet
 * @icmph: ICMPv6 header (type=2, body=MTU in network byte order)
 */
void icmpv6_pmtu_update(struct sk_buff *skb, struct ipv6hdr *ip6h,
                        struct icmpv6_hdr *icmph);

#endif /* ICMPV6_H */
