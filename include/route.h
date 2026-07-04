#ifndef _ROUTE_H
#define _ROUTE_H

#include "list.h"
#include "syshead.h"

#define RT_LOOPBACK 0x01
#define RT_GATEWAY  0x02
#define RT_HOST     0x04
#define RT_REJECT   0x08
#define RT_UP       0x10

struct rtentry {
    struct list_head list;
    uint8_t family;
    union {
        uint32_t v4;
        struct in6_addr v6;
    } dst;
    union {
        uint32_t v4;
        struct in6_addr v6;
    } gateway;
    uint32_t netmask;
    uint8_t prefix_len;
    uint8_t flags;
    uint32_t metric;
    uint16_t pmtu;
    struct netdev *dev;
};

void route_init();
struct rtentry *route_lookup(uint32_t daddr);
struct rtentry *route6_lookup(const struct in6_addr *daddr);
void route6_add(const struct in6_addr *dst, const struct in6_addr *gateway,
                uint8_t prefix_len, uint8_t flags, uint32_t metric,
                struct netdev *dev);
void route6_add_default(const struct in6_addr *gateway, uint8_t flags,
                        uint32_t metric, struct netdev *dev);
int route6_update_pmtu(const struct in6_addr *daddr, uint16_t mtu);
void free_routes();

#endif
