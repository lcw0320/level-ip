#ifndef IPV6_H
#define IPV6_H

#include "syshead.h"
#include "ethernet.h"
#include "skbuff.h"

#define IPV6_VERSION   6
#define IPV6_HDR_LEN   sizeof(struct ipv6hdr)
#define IPV6ADDR_LEN     16
#define IPV6_ADDR_STRLEN 40  /* max "xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx:xxxx" + NUL */
#define IPV6_DEFAULT_HOPLIMIT 64

#ifdef DEBUG_IPV6
#define ipv6_dbg(msg, hdr)                                              \
    do {                                                                \
        char _s[IPV6_ADDR_STRLEN] = {0};                                \
        char _d[IPV6_ADDR_STRLEN] = {0};                                \
        struct in6_addr _sa = {.s6_addr = {0}};                         \
        struct in6_addr _da = {.s6_addr = {0}};                         \
        memcpy(_sa.s6_addr, &(hdr)->saddr, 16);                         \
        memcpy(_da.s6_addr, &(hdr)->daddr, 16);                         \
        ipv6_addr_to_str(&_sa, _s, sizeof(_s));                         \
        ipv6_addr_to_str(&_da, _d, sizeof(_d));                         \
        print_debug("ipv6 "msg" (version=%d plen=%d nexthdr=%d "       \
                    "hop=%d saddr=%s daddr=%s)",                        \
                    ipv6_hdr_version(hdr), ntohs((hdr)->payload_len),   \
                    (hdr)->nexthdr, (hdr)->hop_limit, _s, _d);          \
    } while (0)
#else
#define ipv6_dbg(msg, hdr)
#endif

/*
 * Next Header / Extension Header type values (RFC 8200, IANA registry)
 */
#define NEXTHDR_HOP      0   /* Hop-by-Hop Options Header */
#define NEXTHDR_TCP      6   /* TCP Segment */
#define NEXTHDR_UDP      17  /* UDP Datagram */
#define NEXTHDR_ROUTING  43  /* Routing Header */
#define NEXTHDR_FRAGMENT 44  /* Fragment Header */
#define NEXTHDR_NONE     59  /* No Next Header */
#define NEXTHDR_DEST     60  /* Destination Options Header */
#define NEXTHDR_ICMPV6   58  /* ICMPv6 */

/*
 * struct in6_addr is provided by <netinet/in.h> (included via syshead.h).
 * It supplies s6_addr[16], s6_addr16[8], and s6_addr32[4].
 */

/*
 * IPv6 Fixed Header - 40 bytes (RFC 8200 §3)
 *
 * The first 32 bits (version + traffic class + flow label) are stored
 * as a raw uint32_t in network byte order.  Bitfield layout depends on
 * host endianness, so we use explicit shift/mask helpers instead.
 */
struct ipv6hdr {
    uint32_t vtc_flow;
    uint16_t payload_len;
    uint8_t nexthdr;
    uint8_t hop_limit;
    uint32_t saddr[4];
    uint32_t daddr[4];
} __attribute__((packed));

/* Compile-time check: RFC 8200 §3 mandates exactly 40 bytes */
_Static_assert(sizeof(struct ipv6hdr) == 40,
              "struct ipv6hdr must be exactly 40 bytes");

/*
 * Accessor helpers for the vtc_flow field (network byte order on wire).
 * ntohl/htonl convert to/from host byte order so the bit shifts are
 * endian-correct on both little-endian and big-endian hosts.
 */
static inline uint8_t ipv6_hdr_version(const struct ipv6hdr *h)
{
    return (uint8_t)(ntohl(h->vtc_flow) >> 28);
}

static inline uint8_t ipv6_hdr_tclass(const struct ipv6hdr *h)
{
    return (uint8_t)((ntohl(h->vtc_flow) >> 20) & 0xFF);
}

static inline uint32_t ipv6_hdr_flowlabel(const struct ipv6hdr *h)
{
    return ntohl(h->vtc_flow) & 0x000FFFFF;
}

static inline void ipv6_hdr_set_vtc_flow(struct ipv6hdr *h,
                                          uint8_t version,
                                          uint8_t tclass,
                                          uint32_t flow_label)
{
    uint32_t v = 0;

    v = ((uint32_t)version << 28) |
        ((uint32_t)tclass << 20) |
        (flow_label & 0x000FFFFF);
    h->vtc_flow = htonl(v);
}

/*
 * Generic Extension Header (RFC 8200 §4)
 *
 * Applies to Hop-by-Hop, Routing, and Destination Options headers.
 * hdrlen is in 8-octet units, not including the first 8 octets.
 */
struct ipv6_opt_hdr {
    uint8_t nexthdr;
    uint8_t hdrlen;
    uint8_t data[];
} __attribute__((packed));

/*
 * Fragment Header (RFC 8200 §4.5) - fixed 8 bytes
 *
 * frag_off layout (16 bits, big-endian on wire):
 *   bits 15-3 : Fragment Offset (13 bits, in 8-octet units)
 *   bits  2-1 : Reserved (must be zero)
 *   bit   0   : More Fragments flag (M)
 */
/*
 * Fragment Header frag_off field masks (after ntohs conversion)
 *
 * bits 15-3 : Fragment Offset (13 bits, in 8-octet units)
 * bits  2-1 : Reserved
 * bit   0   : More Fragments (M) flag
 */
#define IPV6_FRAG_OFFSET_MASK 0xFFF8
#define IPV6_FRAG_MF_MASK     0x0001

struct ipv6_frag_hdr {
    uint8_t nexthdr;
    uint8_t reserved;
    uint16_t frag_off;
    uint32_t identification;
} __attribute__((packed));

/*
 * Extension header chain traversal result (returned by ipv6_parse_exthdrs)
 */
struct ipv6_exthdr_result {
    uint8_t final_nexthdr;
    uint8_t has_frag;
    uint16_t frag_off;
    uint32_t frag_id;
    uint32_t consumed;
};

static inline struct ipv6hdr *ipv6_hdr(const struct sk_buff *skb)
{
    return (struct ipv6hdr *)(skb->head + ETH_HDR_LEN);
}

/*
 * IPv6 address comparison
 */
static inline int ipv6_addr_equal(const struct in6_addr *a,
                                  const struct in6_addr *b)
{
    return memcmp(a->s6_addr, b->s6_addr, 16) == 0;
}

/*
 * Check for unspecified address ::/128
 */
static inline int ipv6_addr_is_unspecified(const struct in6_addr *a)
{
    static const struct in6_addr zero = {{{0}}};

    return ipv6_addr_equal(a, &zero);
}

/*
 * Check for multicast address ff00::/8
 */
static inline int ipv6_addr_is_multicast(const struct in6_addr *a)
{
    return a->s6_addr[0] == 0xff;
}

/*
 * Check for link-local address fe80::/10
 */
static inline int ipv6_addr_is_linklocal(const struct in6_addr *a)
{
    return (a->s6_addr[0] == 0xfe) && ((a->s6_addr[1] & 0xc0) == 0x80);
}

/*
 * Parse IPv6 address string into struct in6_addr.
 * Handles :: compression notation.
 * Returns 0 on success, -1 on error.
 */
int ipv6_parse_addr(const char *str, struct in6_addr *addr);

/*
 * Convert struct in6_addr to string using snprintf.
 * Caller must provide a buffer of at least IPV6_ADDR_STRLEN bytes.
 * Returns buf on success, NULL on error.
 */
const char *ipv6_addr_to_str(const struct in6_addr *addr, char *buf,
                             size_t buflen);

/*
 * Walk the IPv6 extension header chain starting at @start.
 * @skb:     packet buffer (kept for API symmetry, may be NULL)
 * @nexthdr: Next Header field from the IPv6 fixed header
 * @start:   first byte after the 40-byte fixed header
 * @plen:    payload_len from the fixed header (host byte order)
 * @result:  filled on success; final_nexthdr = upper-layer protocol,
 *           consumed = bytes of extension headers skipped
 *
 * Returns 0 on success, -1 on error (overflow, unknown header, or
 * more than 8 extension headers).
 */
int ipv6_parse_exthdrs(struct sk_buff *skb, uint8_t nexthdr,
                       uint8_t *start, uint16_t plen,
                       struct ipv6_exthdr_result *result);

/*
 * IPv6 receive entry point — called from netdev_receive().
 * Stub for now; full implementation in T05.
 */
int ipv6_rcv(struct sk_buff *skb);

/*
 * IPv6 output entry point.
 * @skb:       packet buffer with upper-layer payload already in place.
 * @nexthdr:   Next Header value (NEXTHDR_TCP, NEXTHDR_ICMPV6, …).
 * @saddr:     source IPv6 address (may be NULL for auto-select).
 * @daddr:     destination IPv6 address.
 *
 * Performs route lookup, pushes the 40-byte fixed header, then calls
 * dst6_neigh_output() for link-layer delivery.
 * Returns 0 on success, -1 on error.
 */
int ipv6_output(struct sk_buff *skb, uint8_t nexthdr,
                const struct in6_addr *saddr,
                const struct in6_addr *daddr);

#endif /* IPV6_H */
