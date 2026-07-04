#ifndef IPV6_ADDRCONF_H
#define IPV6_ADDRCONF_H

#include "syshead.h"

/*
 * IPv6 address states (RFC 4862 §5.4)
 *
 * TENTATIVE  → DAD in progress, address not yet usable
 * PREFERRED  → address is valid and may be used for new connections
 * DEPRECATED → address is valid but should not be used for new connections
 * INVALID    → address has expired and must not be used
 */
#define ADDR6_TENTATIVE     0
#define ADDR6_PREFERRED     1
#define ADDR6_DEPRECATED    2
#define ADDR6_INVALID       3

/* DAD timeout: 1 second (RFC 4862 §5.4.2, RetransTimer) */
#define DAD_TIMEOUT_MS      1000

#ifdef DEBUG_ADDRCONF
#define addrconf_dbg(msg, ...) \
    do { \
        print_debug("addrconf " msg, ##__VA_ARGS__); \
    } while (0)
#else
#define addrconf_dbg(msg, ...)
#endif

struct netdev;

/*
 * Generate a link-local address (fe80::/64) from a 6-byte MAC
 * address using the EUI-64 algorithm (RFC 4291 Appendix A).
 *
 * Returns 0 on success, -1 on error.
 */
int ipv6_generate_linklocal(const uint8_t *hw_addr, struct in6_addr *addr);

/*
 * Initialize IPv6 stateless address autoconfiguration:
 * generate link-local address, perform DAD, and send RS.
 */
void ipv6_addrconf_init(void);

/*
 * Start Duplicate Address Detection (DAD) for the given address.
 * Sends a DAD NS (source = ::, target = addr) to the solicited-node
 * multicast address.  If no NA is received within DAD_TIMEOUT_MS,
 * the address transitions from TENTATIVE to PREFERRED.
 *
 * @addr: the tentative address to verify
 * @dev:  the network device owning this address
 */
void ipv6_dad_start(struct in6_addr *addr, struct netdev *dev);

/*
 * Called from NDP when a NA with source :: is received during DAD,
 * indicating another node already claims this address.
 * Marks the address as INVALID.
 */
void ipv6_addrconf_dad_failed(struct in6_addr *addr);

/*
 * Process address lifetime from a received Router Advertisement
 * Prefix Information option (RFC 4862 §5.5.3).
 *
 * Sets up timers for:
 *   preferred_lifetime expiry → PREFERRED → DEPRECATED
 *   valid_lifetime expiry     → DEPRECATED → INVALID
 *
 * @prefix:          the advertised prefix
 * @prefix_len:      prefix length in bits
 * @valid_lt:        valid lifetime in seconds (host byte order)
 * @preferred_lt:    preferred lifetime in seconds (host byte order)
 * @dev:             the network device
 */
void ipv6_addrconf_update_lifetime(const struct in6_addr *prefix,
                                   uint8_t prefix_len,
                                   uint32_t valid_lt,
                                   uint32_t preferred_lt,
                                   struct netdev *dev);

#endif /* IPV6_ADDRCONF_H */
