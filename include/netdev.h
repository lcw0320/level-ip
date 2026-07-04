#ifndef NETDEV_H
#define NETDEV_H
#include "syshead.h"
#include "ethernet.h"
#include "skbuff.h"
#include "utils.h"

#define BUFLEN 1600
#define MAX_ADDR_LEN 32

#define netdev_dbg(fmt, args...)                \
    do {                                        \
        print_debug("NETDEV: "fmt, ##args);     \
    } while (0)

struct eth_hdr;
struct timer;

struct netdev {
    uint32_t addr;
    struct in6_addr addr6_ll;
    struct in6_addr addr6_global;
    uint8_t prefix_len;
    int addr6_global_valid;
    uint8_t addr6_ll_state;          /* ADDR6_TENTATIVE/PREFERRED/DEPRECATED/INVALID */
    uint8_t addr6_global_state;      /* ADDR6_TENTATIVE/PREFERRED/DEPRECATED/INVALID */
    uint32_t addr6_valid_lifetime;   /* seconds, 0 = infinite */
    uint32_t addr6_preferred_lifetime; /* seconds, 0 = infinite */
    struct timer *addr6_pref_timer;  /* preferred -> deprecated timer */
    struct timer *addr6_valid_timer; /* valid -> invalid timer */
    uint8_t addr_len;
    uint8_t hwaddr[6];
    uint32_t mtu;
    uint32_t mtu6;
};

void netdev_init();
int netdev_transmit(struct sk_buff *skb, uint8_t *dst, uint16_t ethertype);
void *netdev_rx_loop();
void free_netdev();
struct netdev *netdev_get(uint32_t sip);
#endif
