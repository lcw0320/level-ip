#include "ipv6.h"
#include "utils.h"

/*
 * ipv6_parse_exthdrs - Walk the IPv6 extension header chain (RFC 8200 §4).
 * @skb:      packet buffer (unused, kept for API symmetry with callers)
 * @nexthdr:  Next Header value from the IPv6 fixed header
 * @start:    pointer to the first byte after the 40-byte fixed header
 * @plen:     payload length (from ipv6hdr->payload_len, host byte order)
 * @result:   output structure filled on success
 *
 * Returns 0 on success with result->final_nexthdr set to the upper-layer
 * protocol (TCP/UDP/ICMPv6) or NEXTHDR_NONE.  Returns -1 on any error
 * (malformed chain, length overflow, unknown header type, or too many hops).
 *
 * The caller can locate the upper-layer payload at (start + result->consumed).
 */
int ipv6_parse_exthdrs(struct sk_buff *skb, uint8_t nexthdr,
                       uint8_t *start, uint16_t plen,
                       struct ipv6_exthdr_result *result)
{
    uint8_t *ptr = NULL;
    uint16_t consumed = 0;
    int max_iterations = 0;

    (void)skb;

    ptr = start;
    consumed = 0;
    max_iterations = 0;

    memset(result, 0, sizeof(*result));

    /*
     * Walk the extension header chain (RFC 8200 §4).
     * Cap at 8 iterations to prevent malicious infinite loops.
     */
    while (max_iterations < 8) {
        max_iterations++;

        switch (nexthdr) {
        case NEXTHDR_TCP:
        case NEXTHDR_UDP:
        case NEXTHDR_ICMPV6:
        case NEXTHDR_NONE:
            /* Reached an upper-layer protocol or "no next header"; stop. */
            result->final_nexthdr = nexthdr;
            result->consumed = consumed;
            return 0;

        case NEXTHDR_HOP:
        case NEXTHDR_ROUTING:
        case NEXTHDR_DEST: {
            /*
             * Hop-by-Hop / Routing / Destination Options
             * Generic format: nexthdr(1) + hdrlen(1) + data[]
             * hdrlen is in 8-octet units, not including the first 8 octets.
             */
            struct ipv6_opt_hdr *opt = NULL;
            uint16_t optlen = 0;

            if (consumed + 2 > plen) {
                print_err("IPv6: ext hdr overflow at hop %d\n",
                          max_iterations);
                return -1;
            }

            opt = (struct ipv6_opt_hdr *)ptr;
            optlen = (uint16_t)(opt->hdrlen + 1) * 8;

            if (consumed + optlen > plen) {
                print_err("IPv6: ext hdr optlen overflow at hop %d\n",
                          max_iterations);
                return -1;
            }

            nexthdr = opt->nexthdr;
            ptr += optlen;
            consumed += optlen;
            break;
        }

        case NEXTHDR_FRAGMENT: {
            /*
             * Fragment Header - fixed 8 bytes (RFC 8200 §4.5)
             * nexthdr(1) + reserved(1) + frag_off(2) + identification(4)
             */
            struct ipv6_frag_hdr *frag = NULL;

            if (consumed + 8 > plen) {
                print_err("IPv6: frag hdr overflow at hop %d\n",
                          max_iterations);
                return -1;
            }

            frag = (struct ipv6_frag_hdr *)ptr;
            result->has_frag = 1;
            result->frag_off = (ntohs(frag->frag_off) & IPV6_FRAG_OFFSET_MASK)
                               >> 3;
            result->frag_id = ntohl(frag->identification);

            nexthdr = frag->nexthdr;
            ptr += 8;
            consumed += 8;
            break;
        }

        default:
            /*
             * Unknown extension header type.
             * RFC 8200 §4 requires ICMPv6 Parameter Problem; simplified
             * implementation returns -1 and lets the caller drop the packet.
             */
            print_err("IPv6: unknown extension header %d\n", nexthdr);
            return -1;
        }
    }

    print_err("IPv6: too many extension headers (>8)\n");
    return -1;
}
