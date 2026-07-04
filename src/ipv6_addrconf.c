#include "syshead.h"
#include "ipv6_addrconf.h"
#include "ipv6.h"
#include "route.h"
#include "netdev.h"
#include "ndp.h"
#include "timer.h"
#include "utils.h"

extern struct netdev *netdev;

/*
 * DAD context — holds per-DAD-operation state.
 * Protected by dad_lock; only one DAD at a time in this simplified model.
 */
struct dad_ctx {
    struct in6_addr target;
    uint8_t *state_ptr;  /* points to addr6_ll_state or addr6_global_state */
    struct netdev *dev;
};

static pthread_mutex_t dad_lock = PTHREAD_MUTEX_INITIALIZER;
static struct dad_ctx dad_ctx;
static int dad_done = 0;

/* Forward declarations for timer callbacks */
static void *dad_timeout(void *arg);
static void *addr6_preferred_expired(void *arg);
static void *addr6_valid_expired(void *arg);
static void addr6_timer_arg_free(void *arg);

/*
 * EUI-64 algorithm (RFC 4291 Appendix A):
 *
 *   MAC:  aa:bb:cc:dd:ee:ff  (6 bytes)
 *   Step 1: Flip bit 7  -> aa^0x02:bb:cc:dd:ee:ff
 *   Step 2: Insert FF:FE -> aa^02:bb:cc:FF:FE:dd:ee:ff  (8 bytes)
 *   Step 3: Prepend fe80::/64 prefix
 */
int ipv6_generate_linklocal(const uint8_t *hw_addr, struct in6_addr *addr)
{
    uint8_t iid[8] = {0};

    if (hw_addr == NULL || addr == NULL) {
        return -1;
    }

    /* Build EUI-64 Interface Identifier */
    iid[0] = hw_addr[0] ^ 0x02;  /* flip universal/local bit */
    iid[1] = hw_addr[1];
    iid[2] = hw_addr[2];
    iid[3] = 0xff;
    iid[4] = 0xfe;
    iid[5] = hw_addr[3];
    iid[6] = hw_addr[4];
    iid[7] = hw_addr[5];

    /* fe80::/64 prefix + IID */
    memset(addr, 0, sizeof(struct in6_addr));
    addr->s6_addr[0]  = 0xfe;
    addr->s6_addr[1]  = 0x80;
    /* bytes 2..7 are zero (already cleared) */
    memcpy(&addr->s6_addr[8], iid, 8);

    return 0;
}

/*
 * ipv6_addrconf_dad_failed - Handle DAD failure (RFC 4862 §5.4.3).
 *
 * Called from ndp_na_process() when a NA with source :: is received,
 * meaning another node already claims the tentative address.
 * Marks the address as INVALID.
 */
void ipv6_addrconf_dad_failed(struct in6_addr *target)
{
    char addr_str[IPV6_ADDR_STRLEN] = {0};

    pthread_mutex_lock(&dad_lock);

    if (dad_done) {
        pthread_mutex_unlock(&dad_lock);
        return;
    }

    dad_done = 1;

    if (*dad_ctx.state_ptr == ADDR6_TENTATIVE) {
        *dad_ctx.state_ptr = ADDR6_INVALID;
        ipv6_addr_to_str(target, addr_str, sizeof(addr_str));
        addrconf_dbg("DAD failed for %s, address marked INVALID", addr_str);
    }

    pthread_mutex_unlock(&dad_lock);
}

/*
 * ipv6_dad_start - Start Duplicate Address Detection (RFC 4862 §5.4.2).
 *
 * Sends a DAD NS (source = ::, target = the tentative address) to the
 * solicited-node multicast address.  If no NA reply arrives within
 * DAD_TIMEOUT_MS, the address transitions from TENTATIVE to PREFERRED.
 */
void ipv6_dad_start(struct in6_addr *addr, struct netdev *dev)
{
    struct in6_addr unspecified;
    char addr_str[IPV6_ADDR_STRLEN] = {0};
    int ret = 0;

    memset(&unspecified, 0, sizeof(struct in6_addr));

    pthread_mutex_lock(&dad_lock);

    dad_done = 0;
    memcpy(&dad_ctx.target, addr, sizeof(struct in6_addr));
    dad_ctx.dev = dev;

    if (ipv6_addr_equal(addr, &dev->addr6_ll)) {
        dad_ctx.state_ptr = &dev->addr6_ll_state;
    } else {
        dad_ctx.state_ptr = &dev->addr6_global_state;
    }

    pthread_mutex_unlock(&dad_lock);

    ipv6_addr_to_str(addr, addr_str, sizeof(addr_str));
    addrconf_dbg("starting DAD for %s", addr_str);

    /* Send DAD NS: source = :: (unspecified), target = addr */
    ret = ndp_send_ns(addr, &unspecified, dev);
    if (ret != 0) {
        print_err("addrconf: DAD NS send failed for %s\n", addr_str);
        return;
    }

    /* Set timeout: if no NA within DAD_TIMEOUT_MS, DAD passes */
    timer_oneshot(DAD_TIMEOUT_MS, dad_timeout, NULL);
}

/*
 * dad_timeout - Timer callback: DAD timeout with no NA received.
 *
 * Transitions the address from TENTATIVE to PREFERRED, indicating
 * the address is unique on the link and safe to use.
 */
static void *dad_timeout(void *arg)
{
    char addr_str[IPV6_ADDR_STRLEN] = {0};

    (void)arg;

    pthread_mutex_lock(&dad_lock);

    if (dad_done) {
        pthread_mutex_unlock(&dad_lock);
        return NULL;
    }

    dad_done = 1;

    if (*dad_ctx.state_ptr == ADDR6_TENTATIVE) {
        *dad_ctx.state_ptr = ADDR6_PREFERRED;
        ipv6_addr_to_str(&dad_ctx.target, addr_str, sizeof(addr_str));
        addrconf_dbg("DAD passed for %s, address PREFERRED", addr_str);
    }

    pthread_mutex_unlock(&dad_lock);

    return NULL;
}

/*
 * addr6_preferred_expired - Timer callback: preferred lifetime expired.
 *
 * Transitions the global address from PREFERRED to DEPRECATED.
 * A deprecated address remains valid but should not be used for
 * new connections (RFC 4862 §5.5.4).
 */
static void *addr6_preferred_expired(void *arg)
{
    struct netdev *dev = (struct netdev *)arg;
    char addr_str[IPV6_ADDR_STRLEN] = {0};

    if (dev->addr6_global_state == ADDR6_PREFERRED) {
        dev->addr6_global_state = ADDR6_DEPRECATED;
        dev->addr6_pref_timer = NULL;
        ipv6_addr_to_str(&dev->addr6_global, addr_str, sizeof(addr_str));
        addrconf_dbg("global address %s PREFERRED -> DEPRECATED", addr_str);
    }

    return NULL;
}

/*
 * addr6_valid_expired - Timer callback: valid lifetime expired.
 *
 * Transitions the global address to INVALID and clears the address.
 * An invalid address must not be used for any communication
 * (RFC 4862 §5.5.4).
 */
static void *addr6_valid_expired(void *arg)
{
    struct netdev *dev = (struct netdev *)arg;
    char addr_str[IPV6_ADDR_STRLEN] = {0};

    ipv6_addr_to_str(&dev->addr6_global, addr_str, sizeof(addr_str));
    dev->addr6_global_state = ADDR6_INVALID;
    dev->addr6_global_valid = 0;
    dev->addr6_valid_timer = NULL;
    addrconf_dbg("global address %s -> INVALID, removed", addr_str);

    return NULL;
}

/*
 * Timer arg release callback — frees heap-allocated netdev copy
 * when a lifetime timer is cancelled before firing.
 */
static void addr6_timer_arg_free(void *arg)
{
    free(arg);
}

/*
 * ipv6_addrconf_update_lifetime - Process RA prefix lifetimes (RFC 4862 §5.5.3).
 *
 * Generates a global address from the advertised prefix + EUI-64 IID,
 * starts DAD for it, and schedules preferred/valid lifetime timers.
 *
 * @prefix:       advertised prefix from RA Prefix Information option
 * @prefix_len:   prefix length in bits
 * @valid_lt:     valid lifetime in seconds (host byte order)
 * @preferred_lt: preferred lifetime in seconds (host byte order)
 * @dev:          the network device
 */
void ipv6_addrconf_update_lifetime(const struct in6_addr *prefix,
                                   uint8_t prefix_len,
                                   uint32_t valid_lt,
                                   uint32_t preferred_lt,
                                   struct netdev *dev)
{
    struct in6_addr global;
    struct in6_addr ll_tmp;
    struct netdev *dev_copy = NULL;
    char addr_str[IPV6_ADDR_STRLEN] = {0};
    int ret = 0;

    memset(&global, 0, sizeof(struct in6_addr));
    memset(&ll_tmp, 0, sizeof(struct in6_addr));

    if (dev == NULL || prefix == NULL) {
        return;
    }

    if (prefix_len != 64) {
        addrconf_dbg("prefix_len %d != 64, skipping SLAAC", prefix_len);
        return;
    }

    /* Generate EUI-64 IID via a temporary link-local, then extract IID */
    ret = ipv6_generate_linklocal(dev->hwaddr, &ll_tmp);
    if (ret != 0) {
        print_err("addrconf: failed to generate global IID\n");
        return;
    }

    /* Global address = advertised prefix (high 64 bits) + EUI-64 IID (low 64 bits) */
    memcpy(&global.s6_addr[0], &prefix->s6_addr[0], 8);
    memcpy(&global.s6_addr[8], &ll_tmp.s6_addr[8], 8);

    /* Copy the generated global address into the netdev */
    memcpy(&dev->addr6_global, &global, sizeof(struct in6_addr));
    dev->addr6_global_state = ADDR6_TENTATIVE;
    dev->addr6_global_valid = 1;

    ipv6_addr_to_str(&dev->addr6_global, addr_str, sizeof(addr_str));
    addrconf_dbg("generated global address %s (prefix_len=%d)",
                 addr_str, prefix_len);

    /* Start DAD for the new global address */
    ipv6_dad_start(&dev->addr6_global, dev);

    /* Store lifetimes */
    dev->addr6_valid_lifetime = valid_lt;
    dev->addr6_preferred_lifetime = preferred_lt;

    /* Schedule preferred → deprecated timer (skip if infinite) */
    if (preferred_lt > 0 && preferred_lt != 0xffffffff) {
        if (dev->addr6_pref_timer != NULL) {
            timer_cancel(dev->addr6_pref_timer);
        }
        dev_copy = malloc(sizeof(struct netdev));
        if (dev_copy != NULL) {
            memcpy(dev_copy, dev, sizeof(struct netdev));
            dev->addr6_pref_timer = timer_add_with_release(
                preferred_lt * 1000,
                addr6_preferred_expired,
                dev_copy,
                addr6_timer_arg_free);
        }
    }

    /* Schedule valid → invalid timer (skip if infinite) */
    if (valid_lt > 0 && valid_lt != 0xffffffff) {
        if (dev->addr6_valid_timer != NULL) {
            timer_cancel(dev->addr6_valid_timer);
        }
        dev_copy = malloc(sizeof(struct netdev));
        if (dev_copy != NULL) {
            memcpy(dev_copy, dev, sizeof(struct netdev));
            dev->addr6_valid_timer = timer_add_with_release(
                valid_lt * 1000,
                addr6_valid_expired,
                dev_copy,
                addr6_timer_arg_free);
        }
    }
}

void ipv6_addrconf_init(void)
{
    struct in6_addr ll_prefix;
    char addr_str[IPV6_ADDR_STRLEN] = {0};
    int ret = 0;

    if (netdev == NULL) {
        print_err("addrconf: netdev not initialized\n");
        return;
    }

    ret = ipv6_generate_linklocal(netdev->hwaddr, &netdev->addr6_ll);
    if (ret != 0) {
        print_err("addrconf: failed to generate link-local address\n");
        return;
    }

    netdev->addr6_ll_state = ADDR6_TENTATIVE;

    ipv6_addr_to_str(&netdev->addr6_ll, addr_str, sizeof(addr_str));
    print_debug("addrconf: link-local address %s (MAC %02x:%02x:%02x:%02x:%02x:%02x)\n",
                addr_str,
                netdev->hwaddr[0], netdev->hwaddr[1], netdev->hwaddr[2],
                netdev->hwaddr[3], netdev->hwaddr[4], netdev->hwaddr[5]);

    /* Add link-local route: fe80::/10 (RFC 4291 §2.5.6) */
    memset(&ll_prefix, 0, sizeof(struct in6_addr));
    ll_prefix.s6_addr[0] = 0xfe;
    ll_prefix.s6_addr[1] = 0x80;
    route6_add(&ll_prefix, NULL, 10, RT_HOST, 0, netdev);

    /* Perform DAD on the link-local address before using it */
    ipv6_dad_start(&netdev->addr6_ll, netdev);

    /* Send Router Solicitation to discover routers on the link */
    ret = ndp_send_rs(&netdev->addr6_ll, netdev);
    if (ret != 0) {
        print_err("addrconf: failed to send RS (ret=%d)\n", ret);
    }
}
