#ifndef NDP_H
#define NDP_H

#include "syshead.h"
#include "ipv6.h"
#include "icmpv6.h"
#include "skbuff.h"
#include "list.h"

/*
 * Neighbor Discovery Protocol (RFC 4861)
 *
 * NDP replaces ARP for IPv6 address resolution and provides
 * router discovery, prefix discovery, and neighbor unreachability
 * detection.
 */

/*
 * Neighbor cache entry states (RFC 4861 §7.3.2)
 */
#define NDP_INCOMPLETE  0
#define NDP_REACHABLE   1
#define NDP_STALE       2
#define NDP_DELAY       3
#define NDP_PROBE       4
#define NDP_FAILED      5

/*
 * Timer constants (RFC 4861 §10)
 */
#define NDP_REACHABLE_TIME_MS       30000   /* 30 seconds */
#define NDP_RETRANS_TIMER_MS        1000    /* 1 second */
#define NDP_DELAY_FIRST_PROBE_MS    5000    /* 5 seconds */
#define NDP_MAX_UNICAST_SOLICIT     3
#define NDP_MAX_MULTICAST_SOLICIT   3

/*
 * Neighbor cache limits
 */
#define NDP_MAX_NEIGHBORS   64

/*
 * NA flag masks (RFC 4861 §4.4, byte after csum)
 *
 * Bit 0 (MSB) = Router, Bit 1 = Solicited, Bit 2 = Override
 */
#define NDP_NA_FLAG_ROUTER      0x80
#define NDP_NA_FLAG_SOLICITED   0x40
#define NDP_NA_FLAG_OVERRIDE    0x20

/*
 * Neighbor Solicitation (RFC 4861 §4.3)
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |     Type      |     Code      |          Checksum             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                           Reserved                            |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                                                               |
 *  +                                                               +
 *  |                                                               |
 *  +                       Target Address                          +
 *  |                                                               |
 *  +                                                               +
 *  |                                                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |   Options ...
 *  +-+-+-+-+-+-+-+-+-+-+-+-
 *
 * Fixed size: 24 bytes (excluding options)
 */
struct ndp_ns {
    uint8_t type;            /* 135 */
    uint8_t code;            /* 0 */
    uint16_t csum;
    uint32_t reserved;
    struct in6_addr target;  /* Target IPv6 address */
    uint8_t options[];       /* Source Link-Layer Address option */
} __attribute__((packed));

/*
 * Neighbor Advertisement (RFC 4861 §4.4)
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |     Type      |     Code      |          Checksum             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |R|S|O|                     Reserved                            |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                                                               |
 *  +                                                               +
 *  |                                                               |
 *  +                       Target Address                          +
 *  |                                                               |
 *  +                                                               +
 *  |                                                               |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |   Options ...
 *  +-+-+-+-+-+-+-+-+-+-+-+-
 *
 * Fixed size: 24 bytes (excluding options)
 */
struct ndp_na {
    uint8_t type;            /* 136 */
    uint8_t code;            /* 0 */
    uint16_t csum;
    uint8_t r_flag : 1;      /* Router flag */
    uint8_t s_flag : 1;      /* Solicited flag */
    uint8_t o_flag : 1;      /* Override flag */
    uint8_t reserved_hi : 5;
    uint8_t reserved[3];
    struct in6_addr target;  /* Target IPv6 address */
    uint8_t options[];       /* Target Link-Layer Address option */
} __attribute__((packed));

/*
 * Router Solicitation (RFC 4861 §4.1)
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |     Type      |     Code      |          Checksum             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                            Reserved                           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |   Options ...
 *  +-+-+-+-+-+-+-+-+-+-+-+-
 *
 * Fixed size: 8 bytes (excluding options)
 */
struct ndp_rs {
    uint8_t type;            /* 133 */
    uint8_t code;            /* 0 */
    uint16_t csum;
    uint32_t reserved;
    uint8_t options[];       /* Source Link-Layer Address option */
} __attribute__((packed));

/*
 * Router Advertisement (RFC 4861 §4.2)
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |     Type      |     Code      |          Checksum             |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  | Cur Hop Limit |M|O|  Reserved |       Router Lifetime         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                         Reachable Time                        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                          Retrans Timer                        |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |   Options ...
 *  +-+-+-+-+-+-+-+-+-+-+-+-
 *
 * Fixed size: 16 bytes (excluding options)
 */
struct ndp_ra {
    uint8_t type;            /* 134 */
    uint8_t code;            /* 0 */
    uint16_t csum;
    uint8_t cur_hop_limit;
    uint8_t m_flag : 1;      /* Managed address configuration */
    uint8_t o_flag : 1;      /* Other configuration */
    uint8_t reserved : 6;
    uint16_t router_lifetime;
    uint32_t reachable_time;
    uint32_t retrans_timer;
    uint8_t options[];       /* Prefix / MTU / SLLA options */
} __attribute__((packed));

/*
 * Prefix Information option (RFC 4861 §4.6.2)
 *
 * Fixed size: 32 bytes
 */
struct ndp_opt_prefix {
    uint8_t type;            /* 3 */
    uint8_t length;          /* 4 (= 32 bytes) */
    uint8_t prefix_len;      /* Prefix length */
    uint8_t l_flag : 1;      /* On-Link flag */
    uint8_t a_flag : 1;      /* Autonomous address configuration */
    uint8_t reserved1 : 6;
    uint32_t valid_lifetime;
    uint32_t preferred_lifetime;
    uint32_t reserved2;
    struct in6_addr prefix;
} __attribute__((packed));

/*
 * Neighbor cache entry
 *
 * Tracks the state of a neighboring node's link-layer address
 * resolution.  During INCOMPLETE state, outgoing skbs are queued
 * in @queue and flushed once resolution completes.
 */
struct ndp_entry {
    struct list_head list;
    struct in6_addr ip6;             /* Neighbor IPv6 address */
    uint8_t hwaddr[6];               /* Link-layer MAC address */
    uint8_t state;                   /* NDP_INCOMPLETE/REACHABLE/STALE/DELAY/PROBE */
    uint32_t state_timestamp;        /* Time of last state change (timer_get_tick) */
    uint8_t solicit_count;           /* NS retransmission counter */
    struct sk_buff_head queue;       /* Pending skbs during INCOMPLETE */
};

/*
 * NDP cache management (T12)
 */
void ndp_init(void);
struct ndp_entry *ndp_entry_alloc(struct in6_addr *ip6);
struct ndp_entry *ndp_lookup(struct in6_addr *ip6);
void ndp_flush_queue(struct ndp_entry *entry);
int ndp_queue_skb(struct in6_addr *ip6, struct sk_buff *skb);
uint8_t *ndp_get_hwaddr(struct in6_addr *ip6);

/*
 * NDP message sending (T13, T15)
 */
int ndp_send_ns(struct in6_addr *target, struct in6_addr *src,
                struct netdev *dev);
int ndp_send_rs(struct in6_addr *src, struct netdev *dev);

/*
 * NDP message receiving (T14)
 */
void ndp_incoming(struct sk_buff *skb, struct ipv6hdr *ip6h,
                  struct icmpv6_hdr *icmph);
void ndp_na_process(struct sk_buff *skb, struct ipv6hdr *ip6h,
                    struct ndp_na *na);
void ndp_ns_process(struct sk_buff *skb, struct ipv6hdr *ip6h,
                    struct ndp_ns *ns);

#ifdef DEBUG_NDP
#define ndp_dbg(msg, ...) \
    do { \
        print_debug("ndp " msg, ##__VA_ARGS__); \
    } while (0)
#else
#define ndp_dbg(msg, ...)
#endif

#endif /* NDP_H */
