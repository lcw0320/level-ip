#include "syshead.h"
#include "inet.h"
#include "inet6.h"
#include "socket.h"
#include "sock.h"
#include "tcp.h"
#include "ipv6.h"

extern struct net_ops tcp_ops;
extern struct net_ops udp_ops;

static int INET6_OPS = 2;

/*
 * inet6_stream_connect - IPv6 stream connect entry point
 *
 * Validates AF_INET6, then delegates to the family-agnostic
 * inet_stream_connect() which calls sk->ops->connect (tcp_v6_connect).
 */
static int inet6_stream_connect(struct socket *sock, const struct sockaddr *addr,
                                int addr_len, int flags)
{
    if (addr != NULL && addr->sa_family != AF_INET6 && addr->sa_family != AF_UNSPEC) {
        return -EINVAL;
    }

    return inet_stream_connect(sock, addr, addr_len, flags);
}

/*
 * inet6_bind - IPv6 bind entry point
 *
 * Validates AF_INET6, then delegates to tcp_v6_bind for TCP sockets.
 */
static int inet6_bind(struct socket *sock, const struct sockaddr *addr,
                      int addr_len)
{
    struct sock *sk = sock->sk;

    if (addr_len < (int)sizeof(struct sockaddr_in6)) {
        return -EINVAL;
    }

    if (addr->sa_family != AF_INET6) {
        return -EAFNOSUPPORT;
    }

    /* For TCP, call tcp_v6_bind directly */
    if (sk->protocol == IPPROTO_TCP) {
        return tcp_v6_bind(sk, addr, (socklen_t)addr_len);
    }

    return 0;
}

/* --- IPv6 sock_ops tables --- */

static struct sock_ops inet6_stream_ops = {
    .connect = inet6_stream_connect,
    .bind = inet6_bind,
    .listen = inet_stream_listen,
    .accept = inet_stream_accept,
    .write = inet_write,
    .read = inet_read,
    .close = inet_close,
    .free = inet_free,
    .abort = inet_abort,
    .getpeername = inet_getpeername,
    .getsockname = inet_getsockname,
};

static struct sock_ops inet6_dgram_ops = {
    .write = inet_write,
    .read = inet_read,
    .close = inet_close,
    .free = inet_free,
    .abort = inet_abort,
    .getpeername = inet_getpeername,
    .getsockname = inet_getsockname,
    .bind = inet6_bind,
    .sendto = inet_sendto,
    .recvfrom = inet_recvfrom,
};

/*
 * tcp6_ops - IPv6-specific net_ops for TCP
 *
 * Same as tcp_ops but with tcp_v6_connect and tcp_v6_bind instead
 * of the IPv4-specific versions.
 */
static struct net_ops tcp6_ops = {
    .alloc_sock = &tcp_alloc_sock,
    .init = &tcp_v4_init_sock,
    .connect = &tcp_v6_connect,
    .bind = &tcp_v6_bind,
    .listen = &tcp_v4_listen,
    .accept = &tcp_v4_accept,
    .disconnect = &tcp_disconnect,
    .write = &tcp_write,
    .read = &tcp_read,
    .recv_notify = &tcp_recv_notify,
    .close = &tcp_close,
    .abort = &tcp_abort,
};

static struct sock_type inet6_ops[] = {
    {
        .sock_ops = &inet6_stream_ops,
        .net_ops = &tcp6_ops,
        .type = SOCK_STREAM,
        .protocol = IPPROTO_TCP,
    },
    {
        .sock_ops = &inet6_dgram_ops,
        .net_ops = &udp_ops,
        .type = SOCK_DGRAM,
        .protocol = IPPROTO_UDP,
    },
};

/*
 * inet6_create - Create an IPv6 socket (04 §3.6.1)
 *
 * Looks up the matching sock_type for the requested socket type,
 * allocates a sock via net_ops, sets addr_family = AF_INET6,
 * and initializes the socket data.
 */
int inet6_create(struct socket *sock, int protocol)
{
    struct sock *sk = NULL;
    struct sock_type *skt = NULL;
    int i = 0;

    for (i = 0; i < INET6_OPS; i++) {
        if (inet6_ops[i].type & sock->type) {
            skt = &inet6_ops[i];
            break;
        }
    }

    if (skt == NULL) {
        print_err("Could not find IPv6 socktype for socket\n");
        return 1;
    }

    if (protocol == 0) {
        protocol = skt->protocol;
    }

    sock->ops = skt->sock_ops;

    sk = sk_alloc(skt->net_ops, skt->protocol);
    sk->protocol = skt->protocol;
    sk->addr_family = AF_INET6;

    sock_init_data(sock, sk);

    return 0;
}

struct net_family inet6 = {
    .create = inet6_create,
};

/* --- IPv6 socket lookup --- */

struct sock *inet6_lookup(struct sk_buff *skb,
                          const struct in6_addr *saddr,
                          const struct in6_addr *daddr,
                          uint16_t sport, uint16_t dport)
{
    struct socket *sock = socket_lookup(AF_INET6, 0, 0, saddr, daddr, sport, dport);
    if (sock == NULL) return NULL;

    return sock->sk;
}
