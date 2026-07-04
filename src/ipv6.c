#include "ipv6.h"

/*
 * parse_hex_group - parse up to 4 hex digits into a uint16_t
 *
 * Returns 0 on success, -1 on error.
 */
static int parse_hex_group(const char *s, int len, uint16_t *out)
{
    char tmp[5] = {0};
    unsigned int val = 0;
    int i = 0;

    if (len <= 0 || len > 4) {
        return -1;
    }

    for (i = 0; i < len; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return -1;
        }
    }

    memcpy(tmp, s, (size_t)len);
    tmp[len] = '\0';

    if (sscanf(tmp, "%x", &val) != 1) {
        return -1;
    }

    *out = (uint16_t)val;
    return 0;
}

/*
 * ipv6_parse_addr - parse IPv6 address string into struct in6_addr
 *
 * Handles :: compression (RFC 4291 §2.2).
 * Returns 0 on success, -1 on error.
 */
int ipv6_parse_addr(const char *str, struct in6_addr *addr)
{
    uint16_t groups[8] = {0};
    int ngroups = 0;
    int dcolon_pos = -1;
    int total = 0;
    const char *p = NULL;
    const char *start = NULL;
    int i = 0;

    if (str == NULL || addr == NULL) {
        return -1;
    }

    /* Locate :: (at most one allowed) */
    p = strstr(str, "::");
    if (p != NULL) {
        dcolon_pos = (int)(p - str);
    }

    /* Parse groups before :: */
    start = str;
    p = str;
    while (*p != '\0') {
        if (*p == ':') {
            if (dcolon_pos >= 0 && (int)(p - str) == dcolon_pos) {
                break;
            }
            if (parse_hex_group(start, (int)(p - start),
                                &groups[ngroups]) < 0) {
                return -1;
            }
            ngroups++;
            if (ngroups > 8) {
                return -1;
            }
            start = p + 1;
        }
        p++;
    }

    /* Handle trailing group (no :: involved) */
    if (dcolon_pos < 0 && *p == '\0' && p > start) {
        if (parse_hex_group(start, (int)(p - start),
                            &groups[ngroups]) < 0) {
            return -1;
        }
        ngroups++;
    }

    if (dcolon_pos < 0) {
        /* No :: — must have exactly 8 groups */
        if (ngroups != 8) {
            return -1;
        }
    } else {
        /* Parse groups after :: */
        int right_start = dcolon_pos + 2;
        int right_n = 0;
        uint16_t right_groups[8] = {0};

        if (str[right_start] != '\0') {
            start = str + right_start;
            p = start;
            while (*p != '\0') {
                if (*p == ':') {
                    if (parse_hex_group(start, (int)(p - start),
                                        &right_groups[right_n]) < 0) {
                        return -1;
                    }
                    right_n++;
                    if (right_n > 8) {
                        return -1;
                    }
                    start = p + 1;
                }
                p++;
            }
            if (p > start) {
                if (parse_hex_group(start, (int)(p - start),
                                    &right_groups[right_n]) < 0) {
                    return -1;
                }
                right_n++;
            }
        }

        total = ngroups + right_n;
        if (total > 8) {
            return -1;
        }

        /* Shift right-side groups to their final positions */
        for (i = right_n - 1; i >= 0; i--) {
            groups[8 - right_n + i] = right_groups[i];
        }

        /* Zero-fill the gap (groups[ngroups .. 8-right_n-1] already 0) */
    }

    /* Store result in network byte order */
    for (i = 0; i < 8; i++) {
        addr->s6_addr[i * 2] = (uint8_t)(groups[i] >> 8);
        addr->s6_addr[i * 2 + 1] = (uint8_t)(groups[i] & 0xff);
    }

    return 0;
}

/*
 * ipv6_addr_to_str - convert struct in6_addr to string
 *
 * Uses snprintf with %02x%02x: per segment pair.
 * Caller must provide a buffer of at least IPV6_ADDR_STRLEN bytes.
 * Returns buf on success, NULL on error.
 */
const char *ipv6_addr_to_str(const struct in6_addr *addr, char *buf,
                             size_t buflen)
{
    int written = 0;

    if (addr == NULL || buf == NULL || buflen < IPV6_ADDR_STRLEN) {
        return NULL;
    }

    written = snprintf(buf, buflen,
                       "%02x%02x:%02x%02x:%02x%02x:%02x%02x:"
                       "%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                       addr->s6_addr[0], addr->s6_addr[1],
                       addr->s6_addr[2], addr->s6_addr[3],
                       addr->s6_addr[4], addr->s6_addr[5],
                       addr->s6_addr[6], addr->s6_addr[7],
                       addr->s6_addr[8], addr->s6_addr[9],
                       addr->s6_addr[10], addr->s6_addr[11],
                       addr->s6_addr[12], addr->s6_addr[13],
                       addr->s6_addr[14], addr->s6_addr[15]);

    if (written < 0 || (size_t)written >= buflen) {
        return NULL;
    }

    return buf;
}
