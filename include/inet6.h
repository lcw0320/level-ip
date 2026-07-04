#ifndef INET6_H
#define INET6_H

#include "syshead.h"
#include "sock.h"
#include "skbuff.h"

/*
 * inet6_lookup - Find an IPv6 socket by four-tuple
 * @skb:   packet buffer (unused, kept for API symmetry with inet_lookup)
 * @saddr: remote source IPv6 address
 * @daddr: local destination IPv6 address
 * @sport: remote source port (host byte order)
 * @dport: local destination port (host byte order)
 *
 * Returns the matching sock pointer, or NULL if not found.
 */
struct sock *inet6_lookup(struct sk_buff *skb,
                          const struct in6_addr *saddr,
                          const struct in6_addr *daddr,
                          uint16_t sport, uint16_t dport);

#endif /* INET6_H */
