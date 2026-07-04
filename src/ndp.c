#include "ndp.h"
#include "icmpv6.h"
#include "ethernet.h"
#include "netdev.h"
#include "timer.h"
#include "dst.h"
#include "route.h"
#include "ipv6_addrconf.h"

extern struct netdev *netdev;

/*
 * NDP Neighbor Cache Management (RFC 4861)
 *
 * Manages the neighbor cache — a linked list of ndp_entry structs
 * tracking IPv6-to-MAC address resolution state.  Modelled after
 * the ARP cache in arp.c.
 */

static LIST_HEAD(ndp_cache);
static pthread_mutex_t ndp_lock = PTHREAD_MUTEX_INITIALIZER;
static int ndp_entry_count = 0;

void ndp_init(void)
{
    list_init(&ndp_cache);
    ndp_entry_count = 0;
}

struct ndp_entry *ndp_entry_alloc(struct in6_addr *ip6)
{
    struct ndp_entry *entry = NULL;

    if (ndp_entry_count >= NDP_MAX_NEIGHBORS) {
        print_err("NDP: neighbor cache full (%d entries)\n",
                  ndp_entry_count);
        return NULL;
    }

    entry = malloc(sizeof(struct ndp_entry));
    if (entry == NULL) {
        print_err("NDP: failed to allocate neighbor entry\n");
        return NULL;
    }

    list_init(&entry->list);
    memcpy(&entry->ip6, ip6, sizeof(struct in6_addr));
    memset(entry->hwaddr, 0, sizeof(entry->hwaddr));
    entry->state = NDP_INCOMPLETE;
    entry->state_timestamp = timer_get_tick();
    entry->solicit_count = 0;
    skb_queue_init(&entry->queue);

    /* Insert into the neighbor cache under lock */
    pthread_mutex_lock(&ndp_lock);
    list_add_tail(&entry->list, &ndp_cache);
    ndp_entry_count++;
    pthread_mutex_unlock(&ndp_lock);

    return entry;
}

struct ndp_entry *ndp_lookup(struct in6_addr *ip6)
{
    struct list_head *item = NULL;
    struct ndp_entry *entry = NULL;

    pthread_mutex_lock(&ndp_lock);
    list_for_each(item, &ndp_cache) {
        entry = list_entry(item, struct ndp_entry, list);
        if (ipv6_addr_equal(&entry->ip6, ip6)) {
            pthread_mutex_unlock(&ndp_lock);
            return entry;
        }
    }
    pthread_mutex_unlock(&ndp_lock);

    return NULL;
}

void ndp_flush_queue(struct ndp_entry *entry)
{
    struct sk_buff *skb = NULL;
    uint8_t *dmac = NULL;

    dmac = entry->hwaddr;

    while (!skb_queue_empty(&entry->queue)) {
        skb = skb_dequeue(&entry->queue);
        netdev_transmit(skb, dmac, ETH_P_IPV6);
        free_skb(skb);
    }
}

int ndp_queue_skb(struct in6_addr *ip6, struct sk_buff *skb)
{
    struct ndp_entry *entry = NULL;

    entry = ndp_lookup(ip6);
    if (entry == NULL) {
        print_err("NDP: cannot queue skb, no entry found\n");
        return -1;
    }

    if (skb_queue_len(&entry->queue) >= entry->queue.max_q) {
        print_err("NDP: queue full, dropping skb\n");
        return -1;
    }

    skb_queue_tail(&entry->queue, skb);

    return 0;
}

uint8_t *ndp_get_hwaddr(struct in6_addr *ip6)
{
    struct list_head *item = NULL;
    struct ndp_entry *entry = NULL;
    uint8_t *hwaddr = NULL;

    pthread_mutex_lock(&ndp_lock);
    list_for_each(item, &ndp_cache) {
        entry = list_entry(item, struct ndp_entry, list);
        if (ipv6_addr_equal(&entry->ip6, ip6) &&
            entry->state == NDP_REACHABLE) {
            hwaddr = entry->hwaddr;
            break;
        }
    }
    pthread_mutex_unlock(&ndp_lock);

    return hwaddr;
}

/*
 * Compute solicited-node multicast address (RFC 4291 §2.7.1).
 *
 * ff02::1:ffXX:XXXX — lower 24 bits of the target address are
 * appended to the ff02::1:ff prefix.
 */
static void ipv6_solnode_addr(const struct in6_addr *target,
                              struct in6_addr *mc)
{
    memset(mc, 0, sizeof(struct in6_addr));
    mc->s6_addr[0]  = 0xff;
    mc->s6_addr[1]  = 0x02;
    mc->s6_addr[11] = 0x01;
    mc->s6_addr[12] = 0xff;
    mc->s6_addr[13] = target->s6_addr[13];
    mc->s6_addr[14] = target->s6_addr[14];
    mc->s6_addr[15] = target->s6_addr[15];
}

/*
 * Compute Ethernet multicast MAC from an IPv6 multicast address.
 *
 * 33:33:xx:xx:xx:xx — lower 32 bits of the IPv6 multicast address
 * are copied into the last 4 bytes of the MAC.
 */
static void ipv6_multicast_mac(const struct in6_addr *mc6, uint8_t *mac)
{
    mac[0] = 0x33;
    mac[1] = 0x33;
    mac[2] = mc6->s6_addr[12];
    mac[3] = mc6->s6_addr[13];
    mac[4] = mc6->s6_addr[14];
    mac[5] = mc6->s6_addr[15];
}

/*
 * Send a Neighbor Solicitation (RFC 4861 §4.3).
 *
 * Builds the NS message manually (IPv6 header + ICMPv6 NS + SLLA option)
 * so that hop_limit is forced to 255 as required by RFC 4861 §11.
 * The packet is sent to the solicited-node multicast address via the
 * corresponding Ethernet multicast MAC.
 *
 * Total on-wire: ETH(14) + IPv6(40) + NS(24) + SLLA(8) = 86 bytes.
 */
int ndp_send_ns(struct in6_addr *target, struct in6_addr *src,
                 struct netdev *dev)
{
    struct sk_buff *skb = NULL;
    struct ipv6hdr *ip6h = NULL;
    struct ndp_ns *ns = NULL;
    uint8_t *opt = NULL;
    struct in6_addr dst_mc;
    uint8_t dst_mac[6] = {0};
    int pktlen = 0;
    int total = 0;

    /* NS payload: 24-byte NS header + 8-byte SLLA option */
    pktlen = (int)(sizeof(struct ndp_ns) + 8);
    total = ETH_HDR_LEN + (int)IPV6_HDR_LEN + pktlen;

    skb = alloc_skb((unsigned int)total);
    skb_reserve(skb, (unsigned int)total);
    skb->dev = dev;

    /* 1. Build the NS message */
    skb_push(skb, (unsigned int)pktlen);
    ns = (struct ndp_ns *)skb->data;
    ns->type = ICMPV6_NEIGHBOR_SOLICIT;
    ns->code = 0;
    ns->csum = 0;
    ns->reserved = 0;
    memcpy(&ns->target, target, sizeof(struct in6_addr));

    /* SLLA option: type=1, length=1 (in units of 8 bytes), MAC */
    opt = ns->options;
    opt[0] = NDP_OPT_SLLA;
    opt[1] = 1;
    memcpy(opt + 2, dev->hwaddr, 6);

    /* 2. Compute solicited-node multicast destination address */
    ipv6_solnode_addr(target, &dst_mc);

    /* 3. Build the IPv6 header (hop_limit = 255 per RFC 4861 §11) */
    skb_push(skb, IPV6_HDR_LEN);
    ip6h = (struct ipv6hdr *)skb->data;
    ipv6_hdr_set_vtc_flow(ip6h, IPV6_VERSION, 0, 0);
    ip6h->payload_len = htons((uint16_t)pktlen);
    ip6h->nexthdr = NEXTHDR_ICMPV6;
    ip6h->hop_limit = 255;
    memcpy(&ip6h->saddr, src, sizeof(struct in6_addr));
    memcpy(&ip6h->daddr, &dst_mc, sizeof(struct in6_addr));

    /* 4. Compute ICMPv6 checksum (includes pseudo-header).
     * Use src/dst_mc directly to avoid taking addresses of packed
     * struct members.
     */
    ns->csum = icmpv6_checksum(src, &dst_mc,
                               (uint8_t *)ns, (uint16_t)pktlen);

    ndp_dbg("send NS for target address");

    /* 5. Send to the Ethernet multicast MAC */
    ipv6_multicast_mac(&dst_mc, dst_mac);
    netdev_transmit(skb, dst_mac, ETH_P_IPV6);
    free_skb(skb);
    return 0;
}

/*
 * Send a Router Solicitation (RFC 4861 §4.1).
 *
 * Builds the RS message (IPv6 header + ICMPv6 RS + SLLA option) and
 * sends it to ff02::2 (all-routers multicast) with hop_limit = 255.
 *
 * Total on-wire: ETH(14) + IPv6(40) + RS(8) + SLLA(8) = 70 bytes.
 */
int ndp_send_rs(struct in6_addr *src, struct netdev *dev)
{
    struct sk_buff *skb = NULL;
    struct ipv6hdr *ip6h = NULL;
    struct ndp_rs *rs = NULL;
    uint8_t *opt = NULL;
    struct in6_addr dst_mc;
    uint8_t dst_mac[6] = {0};
    int pktlen = 0;
    int total = 0;

    /* RS payload: 8-byte RS header + 8-byte SLLA option = 16 bytes */
    pktlen = (int)(sizeof(struct ndp_rs) + 8);
    total = ETH_HDR_LEN + (int)IPV6_HDR_LEN + pktlen;

    skb = alloc_skb((unsigned int)total);
    if (skb == NULL) {
        print_err("NDP: alloc_skb failed for RS\n");
        return -1;
    }

    skb_reserve(skb, (unsigned int)total);
    skb->dev = dev;

    /* 1. Build the RS message */
    skb_push(skb, (unsigned int)pktlen);
    rs = (struct ndp_rs *)skb->data;
    rs->type = ICMPV6_ROUTER_SOLICIT;
    rs->code = 0;
    rs->csum = 0;
    rs->reserved = 0;

    /* SLLA option: type=1, length=1 (8 bytes), our MAC */
    opt = rs->options;
    opt[0] = NDP_OPT_SLLA;
    opt[1] = 1;
    memcpy(opt + 2, dev->hwaddr, 6);

    /* 2. Destination: ff02::2 (all-routers multicast) */
    memset(&dst_mc, 0, sizeof(struct in6_addr));
    dst_mc.s6_addr[0] = 0xff;
    dst_mc.s6_addr[1] = 0x02;
    dst_mc.s6_addr[15] = 0x02;

    /* 3. Build IPv6 header (hop_limit = 255 per RFC 4861 §11) */
    skb_push(skb, IPV6_HDR_LEN);
    ip6h = (struct ipv6hdr *)skb->data;
    ipv6_hdr_set_vtc_flow(ip6h, IPV6_VERSION, 0, 0);
    ip6h->payload_len = htons((uint16_t)pktlen);
    ip6h->nexthdr = NEXTHDR_ICMPV6;
    ip6h->hop_limit = 255;
    memcpy(&ip6h->saddr, src, sizeof(struct in6_addr));
    memcpy(&ip6h->daddr, &dst_mc, sizeof(struct in6_addr));

    /* 4. Compute ICMPv6 checksum */
    rs->csum = icmpv6_checksum(src, &dst_mc,
                               (uint8_t *)rs, (uint16_t)pktlen);

    ndp_dbg("send RS to ff02::2");

    /* 5. Send to Ethernet multicast MAC */
    ipv6_multicast_mac(&dst_mc, dst_mac);
    netdev_transmit(skb, dst_mac, ETH_P_IPV6);
    free_skb(skb);
    return 0;
}

/*
 * ndp_ra_process - Process a received Router Advertisement (RFC 4861 §4.2).
 * @skb:  packet buffer (freed by this function)
 * @ip6h: IPv6 header of the RA message
 * @ra:   pointer to the RA message within the packet
 *
 * 1. Extract cur_hop_limit, reachable_time, retrans_timer.
 * 2. Walk options looking for Prefix Information (type=3, length=4).
 * 3. Record prefix_len to netdev.
 * 4. Add a default route with gateway = RA source address.
 */
static void ndp_ra_process(struct sk_buff *skb, struct ipv6hdr *ip6h,
                           struct ndp_ra *ra)
{
    struct in6_addr gateway;
    struct in6_addr ra_prefix;
    uint8_t *opt = NULL;
    int optlen = 0;
    int skip = 0;
    struct ndp_opt_prefix *pfx = NULL;

    memset(&ra_prefix, 0, sizeof(struct in6_addr));
    memcpy(&gateway, &ip6h->saddr, sizeof(struct in6_addr));

    ndp_dbg("recv RA hop_limit=%d lifetime=%d",
            ra->cur_hop_limit, ntohs(ra->router_lifetime));

    /* Walk RA options looking for Prefix Information */
    opt = ra->options;
    optlen = ntohs(ip6h->payload_len) - (int)sizeof(struct ndp_ra);

    while (optlen >= 8) {
        skip = opt[1] * 8;
        if (skip == 0 || skip > optlen) {
            break;
        }

        if (opt[0] == NDP_OPT_PREFIX && opt[1] == 4) {
            pfx = (struct ndp_opt_prefix *)opt;
            netdev->prefix_len = pfx->prefix_len;

            ndp_dbg("RA prefix_len=%d valid=%u preferred=%u",
                    pfx->prefix_len,
                    ntohl(pfx->valid_lifetime),
                    ntohl(pfx->preferred_lifetime));

            /* Copy prefix to local to avoid packed-member alignment warning */
            memcpy(&ra_prefix, &pfx->prefix, sizeof(struct in6_addr));

            /* Pass prefix and lifetimes to addrconf for SLAAC */
            ipv6_addrconf_update_lifetime(
                &ra_prefix,
                pfx->prefix_len,
                ntohl(pfx->valid_lifetime),
                ntohl(pfx->preferred_lifetime),
                netdev);
        }

        opt += skip;
        optlen -= skip;
    }

    /* Add default route: gateway = RA source, prefix_len = 0 */
    route6_add_default(&gateway, RT_GATEWAY, 0, netdev);

    ndp_dbg("added default route via RA source");

    free_skb(skb);
}

/*
 * ndp_na_process - Process a received Neighbor Advertisement (RFC 4861 §4.4).
 * @skb:  packet buffer (freed by this function)
 * @ip6h: IPv6 header of the NA message
 * @na:   pointer to the NA message within the packet
 *
 * 1. Parse the Target Link-Layer Address (TLLA) option.
 * 2. Look up the neighbor cache entry for the target address.
 * 3. If INCOMPLETE -> REACHABLE, flush queued skbs.
 * 4. If already REACHABLE/STALE/DELAY/PROBE, update per O/S flags.
 * 5. If source is :: (DAD), notify addrconf of DAD failure.
 */
void ndp_na_process(struct sk_buff *skb, struct ipv6hdr *ip6h,
                    struct ndp_na *na)
{
    struct ndp_entry *entry = NULL;
    struct in6_addr target;
    struct in6_addr saddr;
    uint8_t *opt = NULL;
    int optlen = 0;
    uint8_t tlla[6] = {0};
    int found_tlla = 0;
    int skip = 0;

    /* Copy addresses from packed structs to local variables */
    memcpy(&target, &na->target, sizeof(struct in6_addr));
    memcpy(&saddr, &ip6h->saddr, sizeof(struct in6_addr));

    ndp_dbg("recv NA S=%d O=%d", na->s_flag, na->o_flag);

    /* 1. Parse Target Link-Layer Address option */
    opt = na->options;
    optlen = ntohs(ip6h->payload_len) - sizeof(struct ndp_na);
    while (optlen >= 8) {
        if (opt[0] == NDP_OPT_TLLA && opt[1] == 1) {
            memcpy(tlla, opt + 2, 6);
            found_tlla = 1;
        }
        skip = opt[1] * 8;
        if (skip == 0) {
            break;
        }
        opt += skip;
        optlen -= skip;
    }

    if (found_tlla == 0) {
        print_err("NDP: NA without TLLA option, dropping\n");
        free_skb(skb);
        return;
    }

    /* 2. Look up neighbor cache entry */
    entry = ndp_lookup(&target);

    if (entry != NULL) {
        if (entry->state == NDP_INCOMPLETE) {
            /* INCOMPLETE -> REACHABLE: first NA received */
            memcpy(entry->hwaddr, tlla, 6);
            entry->state = NDP_REACHABLE;
            entry->state_timestamp = timer_get_tick();
            entry->solicit_count = 0;

            ndp_dbg("entry -> REACHABLE");
            ndp_flush_queue(entry);
        } else {
            /* REACHABLE/STALE/DELAY/PROBE: update per O/S flags */
            if (na->o_flag ||
                memcmp(entry->hwaddr, tlla, 6) != 0) {
                memcpy(entry->hwaddr, tlla, 6);
            }
            if (na->s_flag) {
                entry->state = NDP_REACHABLE;
            } else if (na->o_flag ||
                       memcmp(entry->hwaddr, tlla, 6) != 0) {
                entry->state = NDP_STALE;
            }
            entry->state_timestamp = timer_get_tick();
        }
    }

    /* 3. DAD check: if source is ::, someone else claims this address */
    if (ipv6_addr_is_unspecified(&saddr)) {
        ipv6_addrconf_dad_failed(&target);
    }

    free_skb(skb);
}

/*
 * ndp_ns_process - Process a received Neighbor Solicitation (RFC 4861 §4.3).
 * @skb:  packet buffer (freed by this function)
 * @ip6h: IPv6 header of the NS message
 * @ns:   pointer to the NS message within the packet
 *
 * Checks if the target address belongs to this node (addr6_ll or
 * addr6_global).  If so, constructs and sends a Neighbor Advertisement
 * reply with solicited=1 and the TLLA option.
 *
 * NA reply: IPv6(40) + NA(24) + TLLA(8) = 72 bytes on wire after ETH.
 */
void ndp_ns_process(struct sk_buff *skb, struct ipv6hdr *ip6h,
                    struct ndp_ns *ns)
{
    struct sk_buff *reply = NULL;
    struct ndp_na *na = NULL;
    struct ipv6hdr *ip6h_reply = NULL;
    uint8_t *opt = NULL;
    uint8_t dst_mac[6] = {0};
    int pktlen = 0;
    int total = 0;
    struct in6_addr saddr;
    struct in6_addr daddr;
    struct in6_addr target;
    struct in6_addr reply_daddr;
    int is_ours = 0;

    /* Copy addresses from packed structs to local vars */
    memcpy(&saddr, &ip6h->saddr, sizeof(struct in6_addr));
    memcpy(&daddr, &ip6h->daddr, sizeof(struct in6_addr));
    memcpy(&target, &ns->target, sizeof(struct in6_addr));

    /* 1. Check if the target address is ours */
    if (ipv6_addr_equal(&target, &netdev->addr6_ll)) {
        is_ours = 1;
    } else if (netdev->addr6_global_valid &&
               ipv6_addr_equal(&target, &netdev->addr6_global)) {
        is_ours = 1;
    }

    if (is_ours == 0) {
        ndp_dbg("recv NS for non-local target, ignoring");
        free_skb(skb);
        return;
    }

    ndp_dbg("recv NS for our address, sending NA reply");

    /* 2. Build NA reply: NA header (24) + TLLA option (8) = 32 bytes */
    pktlen = (int)(sizeof(struct ndp_na) + 8);
    total = ETH_HDR_LEN + (int)IPV6_HDR_LEN + pktlen;

    reply = alloc_skb((unsigned int)total);
    if (reply == NULL) {
        print_err("NDP: alloc_skb failed for NA reply\n");
        free_skb(skb);
        return;
    }

    skb_reserve(reply, (unsigned int)total);
    reply->dev = netdev;

    /* 3. Build the NA message */
    skb_push(reply, (unsigned int)pktlen);
    na = (struct ndp_na *)reply->data;
    na->type = ICMPV6_NEIGHBOR_ADVERT;
    na->code = 0;
    na->csum = 0;
    na->r_flag = 0;
    na->s_flag = 1;  /* Solicited: this is a response to an NS */
    na->o_flag = 1;  /* Override: always overwrite the cache entry */
    na->reserved_hi = 0;
    memset(na->reserved, 0, sizeof(na->reserved));
    memcpy(&na->target, &target, sizeof(struct in6_addr));

    /* TLLA option: type=2, length=1 (8 bytes), our MAC */
    opt = na->options;
    opt[0] = NDP_OPT_TLLA;
    opt[1] = 1;
    memcpy(opt + 2, netdev->hwaddr, 6);

    /* 4. Build IPv6 header (hop_limit = 255 per RFC 4861 §11) */
    skb_push(reply, IPV6_HDR_LEN);
    ip6h_reply = (struct ipv6hdr *)reply->data;
    ipv6_hdr_set_vtc_flow(ip6h_reply, IPV6_VERSION, 0, 0);
    ip6h_reply->payload_len = htons((uint16_t)pktlen);
    ip6h_reply->nexthdr = NEXTHDR_ICMPV6;
    ip6h_reply->hop_limit = 255;

    /* Source = target address from the NS (our address) */
    memcpy(&ip6h_reply->saddr, &target, sizeof(struct in6_addr));

    /* Destination = source of the NS (or link-local all-nodes if DAD) */
    if (ipv6_addr_is_unspecified(&saddr)) {
        /* DAD NS: reply to ff02::1 (all-nodes multicast) */
        memset(&reply_daddr, 0, sizeof(struct in6_addr));
        reply_daddr.s6_addr[0] = 0xff;
        reply_daddr.s6_addr[1] = 0x02;
        reply_daddr.s6_addr[15] = 0x01;
    } else {
        memcpy(&reply_daddr, &saddr, sizeof(struct in6_addr));
    }
    memcpy(&ip6h_reply->daddr, &reply_daddr, sizeof(struct in6_addr));

    /* 5. Compute ICMPv6 checksum using local address copies */
    na->csum = icmpv6_checksum(&target, &reply_daddr,
                               (uint8_t *)na, (uint16_t)pktlen);

    /* 6. Compute destination MAC and send */
    if (ipv6_addr_is_multicast(&reply_daddr)) {
        ipv6_multicast_mac(&reply_daddr, dst_mac);
    } else {
        /* Use the source MAC from the incoming NS Ethernet frame */
        memcpy(dst_mac, skb->head + 6, 6);
    }

    free_skb(skb);
    netdev_transmit(reply, dst_mac, ETH_P_IPV6);
    free_skb(reply);
}

/*
 * ndp_incoming - NDP message dispatch entry point (RFC 4861 §11).
 * @skb:   packet buffer (ownership transferred; freed by handlers)
 * @ip6h:  IPv6 header of the NDP message
 * @icmph: ICMPv6 header of the NDP message
 *
 * RFC 4861 §11 security check: hop_limit MUST be 255.  This prevents
 * off-link nodes from injecting forged NDP messages.
 */
void ndp_incoming(struct sk_buff *skb, struct ipv6hdr *ip6h,
                  struct icmpv6_hdr *icmph)
{
    if (ip6h->hop_limit != 255) {
        print_err("NDP: hop_limit != 255 (%d), dropping\n",
                  ip6h->hop_limit);
        free_skb(skb);
        return;
    }

    switch (icmph->type) {
    case ICMPV6_ROUTER_SOLICIT:
        ndp_dbg("recv RS, ignoring (stub)");
        free_skb(skb);
        break;
    case ICMPV6_ROUTER_ADVERT:
        ndp_ra_process(skb, ip6h, (struct ndp_ra *)icmph);
        break;
    case ICMPV6_NEIGHBOR_SOLICIT:
        ndp_ns_process(skb, ip6h, (struct ndp_ns *)icmph);
        break;
    case ICMPV6_NEIGHBOR_ADVERT:
        ndp_na_process(skb, ip6h, (struct ndp_na *)icmph);
        break;
    case ICMPV6_REDIRECT:
        ndp_dbg("recv Redirect, ignoring");
        free_skb(skb);
        break;
    default:
        print_err("NDP: unknown type %d\n", icmph->type);
        free_skb(skb);
        break;
    }
}
