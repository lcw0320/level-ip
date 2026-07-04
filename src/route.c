#include "syshead.h"
#include "route.h"
#include "dst.h"
#include "netdev.h"
#include "list.h"
#include "ip.h"

static LIST_HEAD(routes);

extern struct netdev *netdev;
extern struct netdev *loop;

extern char *tapaddr;
extern char *taproute;

static struct rtentry *route_alloc(uint32_t dst, uint32_t gateway, uint32_t netmask,
                                   uint8_t flags, uint32_t metric, struct netdev *dev)
{
    struct rtentry *rt = malloc(sizeof(struct rtentry));
    list_init(&rt->list);

    rt->family = AF_INET;
    rt->dst.v4 = dst;
    rt->gateway.v4 = gateway;
    rt->netmask = netmask;
    rt->prefix_len = 0;
    rt->flags = flags;
    rt->metric = metric;
    rt->pmtu = 0;
    rt->dev = dev;
    return rt;
}

void route_add(uint32_t dst, uint32_t gateway, uint32_t netmask, uint8_t flags,
               uint32_t metric, struct netdev *dev)
{
    struct rtentry *rt = route_alloc(dst, gateway, netmask, flags, metric, dev);

    list_add_tail(&rt->list, &routes);
}

static struct rtentry *route6_alloc(const struct in6_addr *dst,
                                    const struct in6_addr *gateway,
                                    uint8_t prefix_len, uint8_t flags,
                                    uint32_t metric, struct netdev *dev)
{
    struct rtentry *rt = malloc(sizeof(struct rtentry));
    list_init(&rt->list);

    rt->family = AF_INET6;
    memset(&rt->dst, 0, sizeof(rt->dst));
    memset(&rt->gateway, 0, sizeof(rt->gateway));
    if (dst != NULL) {
        memcpy(&rt->dst.v6, dst, sizeof(struct in6_addr));
    }
    if (gateway != NULL) {
        memcpy(&rt->gateway.v6, gateway, sizeof(struct in6_addr));
    }
    rt->netmask = 0;
    rt->prefix_len = prefix_len;
    rt->flags = flags;
    rt->metric = metric;
    rt->pmtu = 0;
    rt->dev = dev;
    return rt;
}

void route6_add(const struct in6_addr *dst, const struct in6_addr *gateway,
                uint8_t prefix_len, uint8_t flags, uint32_t metric,
                struct netdev *dev)
{
    struct rtentry *rt = route6_alloc(dst, gateway, prefix_len, flags, metric, dev);

    list_add_tail(&rt->list, &routes);
}

/*
 * Add an IPv6 default route (::/0) — convenience wrapper around route6_add.
 * Called from ndp_ra_process() when a Router Advertisement is received.
 */
void route6_add_default(const struct in6_addr *gateway, uint8_t flags,
                        uint32_t metric, struct netdev *dev)
{
    struct in6_addr dst_zero = {{{0}}};

    route6_add(&dst_zero, gateway, 0, flags, metric, dev);
}

void route_init()
{
    route_add(loop->addr, 0, 0xff000000, RT_LOOPBACK, 0, loop);
    route_add(netdev->addr, 0, 0xffffff00, RT_HOST, 0, netdev);
    route_add(0, ip_parse(tapaddr), 0, RT_GATEWAY, 0, netdev);
}

struct rtentry *route_lookup(uint32_t daddr)
{
    struct list_head *item = NULL;
    struct rtentry *rt = NULL;

    list_for_each(item, &routes) {
        rt = list_entry(item, struct rtentry, list);
        if (rt->family != AF_INET) {
            continue;
        }
        if ((daddr & rt->netmask) == (rt->dst.v4 & rt->netmask)) {
            break;
        }
        /* If no matches, we default to default gw (last item) */
    }

    return rt;
}

/*
 * IPv6 longest-prefix match.
 * Walks the entire route list and returns the AF_INET6 entry with the
 * longest prefix_len that still matches daddr.  If no prefix matches,
 * returns the default route (prefix_len == 0) if one exists.
 */
struct rtentry *route6_lookup(const struct in6_addr *daddr)
{
    struct list_head *item = NULL;
    struct rtentry *rt = NULL;
    struct rtentry *best = NULL;
    uint8_t best_len = 0;
    int i = 0;
    int bytes = 0;
    int bits = 0;
    uint8_t mask = 0;
    int match = 0;

    list_for_each(item, &routes) {
        rt = list_entry(item, struct rtentry, list);
        if (rt->family != AF_INET6) {
            continue;
        }

        /* Compare prefix_len bits of daddr against rt->dst.v6 */
        bytes = rt->prefix_len / 8;
        bits = rt->prefix_len % 8;
        match = 1;

        for (i = 0; i < bytes; i++) {
            if (daddr->s6_addr[i] != rt->dst.v6.s6_addr[i]) {
                match = 0;
                break;
            }
        }

        if (match && bits > 0 && bytes < 16) {
            mask = (uint8_t)(0xFF << (8 - bits));
            if ((daddr->s6_addr[bytes] & mask) !=
                (rt->dst.v6.s6_addr[bytes] & mask)) {
                match = 0;
            }
        }

        if (match) {
            if (best == NULL || rt->prefix_len > best_len) {
                best = rt;
                best_len = rt->prefix_len;
            }
        }
    }

    return best;
}

/*
 * route6_update_pmtu - Update path MTU for an IPv6 route (RFC 8201).
 * @daddr: destination address from the triggering packet
 * @mtu:   new MTU value from ICMPv6 Packet Too Big (host byte order)
 *
 * Finds the matching IPv6 route and updates its pmtu field.
 * The MTU must not be lower than 1280 (IPv6 minimum MTU, RFC 8200 §5).
 * Returns 0 on success, -1 if no matching route found.
 */
int route6_update_pmtu(const struct in6_addr *daddr, uint16_t mtu)
{
    struct rtentry *rt = NULL;
    uint16_t old_pmtu = 0;

    if (mtu < 1280) {
        print_err("route6: PMTU %u below IPv6 minimum 1280, clamping\n", mtu);
        mtu = 1280;
    }

    rt = route6_lookup(daddr);
    if (rt == NULL) {
        print_err("route6: no route for PMTU update\n");
        return -1;
    }

    old_pmtu = rt->pmtu;
    rt->pmtu = mtu;

    print_debug("pmtu: updated to %u (was %u)\n", mtu, old_pmtu);

    return 0;
}

void free_routes()
{
    struct list_head *item = NULL;
    struct list_head *tmp = NULL;
    struct rtentry *rt = NULL;

    list_for_each_safe(item, tmp, &routes) {
        rt = list_entry(item, struct rtentry, list);
        list_del(item);

        free(rt);
    }
}
