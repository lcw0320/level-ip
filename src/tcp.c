#include "syshead.h"
#include "inet.h"
#include "tcp.h"
#include "ip.h"
#include "sock.h"
#include "utils.h"
#include "timer.h"
#include "wait.h"
#include "ipv6.h"
#include "inet6.h"
#include "netdev.h"
#include "route.h"

#ifdef DEBUG_TCP
const char *tcp_dbg_states[] = {
    "TCP_LISTEN", "TCP_SYNSENT", "TCP_SYN_RECEIVED", "TCP_ESTABLISHED", "TCP_FIN_WAIT_1",
    "TCP_FIN_WAIT_2", "TCP_CLOSE", "TCP_CLOSE_WAIT", "TCP_CLOSING", "TCP_LAST_ACK", "TCP_TIME_WAIT",
};
#endif

static pthread_rwlock_t tcplock = PTHREAD_RWLOCK_INITIALIZER;

struct net_ops tcp_ops = {
    .alloc_sock = &tcp_alloc_sock,
    .init = &tcp_v4_init_sock,
    .connect = &tcp_v4_connect,
    .bind = &tcp_v4_bind,
    .listen = &tcp_v4_listen,
    .accept = &tcp_v4_accept,
    .disconnect = &tcp_disconnect,
    .write = &tcp_write,
    .read = &tcp_read,
    .recv_notify = &tcp_recv_notify,
    .close = &tcp_close,
    .abort = &tcp_abort,
};

void tcp_init()
{
    
}

static void tcp_init_segment(struct tcphdr *th, int tcp_dlen, struct sk_buff *skb)
{
    th->sport = ntohs(th->sport);
    th->dport = ntohs(th->dport);
    th->seq = ntohl(th->seq);
    th->ack_seq = ntohl(th->ack_seq);
    th->win = ntohs(th->win);
    th->csum = ntohs(th->csum);
    th->urp = ntohs(th->urp);

    skb->seq = th->seq;
    skb->dlen = tcp_dlen;
    skb->len = skb->dlen + th->syn + th->fin;
    skb->end_seq = skb->seq + skb->dlen;
    skb->payload = th->data;
}

static void tcp_clear_queues(struct tcp_sock *tsk) {
    skb_queue_free(&tsk->ofo_queue);
}

void tcp_in(struct sk_buff *skb)
{
    struct sock *sk;
    struct iphdr *iph;
    struct tcphdr *th;
    int tcp_dlen = 0;

    iph = ip_hdr(skb);
    th = (struct tcphdr*) iph->data;

    tcp_dlen = ip_len(iph) - tcp_hlen(th);
    tcp_init_segment(th, tcp_dlen, skb);

    sk = inet_lookup(skb, iph->saddr, iph->daddr, th->sport, th->dport);

    if (sk == NULL) {
        print_err("No TCP socket for sport %d dport %d\n",
                  th->sport, th->dport);
        free_skb(skb);
        return;
    }
    socket_wr_acquire(sk->sock);

    tcp_in_dbg(th, sk, skb);
    /* if (tcp_checksum(iph, th) != 0) { */
    /*     goto discard; */
    /* } */
    tcp_input_state(sk, th, skb, &iph->saddr, sk->addr_family);

    socket_release(sk->sock);
}

/*
 * tcp_in_v6 - TCP IPv6 receive entry point (04 §3.4.12)
 * @skb:     packet buffer (ownership transferred)
 * @payload: first byte of the TCP segment (after IPv6 + ext headers)
 */
void tcp_in_v6(struct sk_buff *skb, uint8_t *payload)
{
    struct ipv6hdr *ip6h = NULL;
    struct tcphdr *th = NULL;
    struct sock *sk = NULL;
    struct in6_addr saddr;
    struct in6_addr daddr;
    int tcp_dlen = 0;

    ip6h = ipv6_hdr(skb);
    th = (struct tcphdr *)payload;

    /* Copy addresses to local vars to avoid packed-member alignment issues */
    memcpy(&saddr, ip6h->saddr, sizeof(struct in6_addr));
    memcpy(&daddr, ip6h->daddr, sizeof(struct in6_addr));

    /* TCP data length = IPv6 payload_len - ext headers consumed - TCP header */
    tcp_dlen = ntohs(ip6h->payload_len) - (uint32_t)(payload - (uint8_t *)(ip6h + 1)) - tcp_hlen(th);
    tcp_init_segment(th, tcp_dlen, skb);

    sk = inet6_lookup(skb, &saddr, &daddr, th->sport, th->dport);

    if (sk == NULL) {
        print_err("No TCP socket for sport %d dport %d (IPv6)\n",
                  th->sport, th->dport);
        free_skb(skb);
        return;
    }

    socket_wr_acquire(sk->sock);

    tcp_in_dbg(th, sk, skb);

    tcp_input_state(sk, th, skb, &saddr, sk->addr_family);

    socket_release(sk->sock);
}

int tcp_udp_checksum(uint32_t saddr, uint32_t daddr, uint8_t proto,
                     uint8_t *data, uint16_t len)
{
    uint32_t sum = 0;

    sum += saddr;
    sum += daddr;
    sum += htons(proto);
    sum += htons(len);
    
    return checksum(data, len, sum);
}

int tcp_v4_checksum(struct sk_buff *skb, uint32_t saddr, uint32_t daddr)
{
    return tcp_udp_checksum(saddr, daddr, IP_TCP, skb->data, skb->len);
}

/*
 * tcp_v6_checksum - TCP IPv6 pseudo-header checksum (RFC 8200 §8.1)
 * @saddr: source IPv6 address
 * @daddr: destination IPv6 address
 * @skb:   packet buffer (skb->data = TCP header + payload, skb->len = TCP total length)
 *
 * Pseudo-header format is identical to ICMPv6:
 *   Source Address       : 128 bits
 *   Destination Address  : 128 bits
 *   Upper-Layer Length   : 32 bits
 *   Zero (3 bytes) + NH  : 8 bits  (TCP = 6)
 */
int tcp_v6_tcp_partion_checksum(struct sk_buff *skb)
{
    uint32_t sum = 0;
    uint32_t len = 0;
    uint8_t pseudo[8] = {0};

    len = skb->len;
    pseudo[0] = (uint8_t)(len >> 24);
    pseudo[1] = (uint8_t)(len >> 16);
    pseudo[2] = (uint8_t)(len >> 8);
    pseudo[3] = (uint8_t)(len & 0xFF);
    /* pseudo[36..38] = 0 */
    pseudo[7] = IP_TCP;

    sum += sum_every_16bits(pseudo, 8);
    sum += sum_every_16bits(skb->data, skb->len);

    return sum;
}

struct sock *tcp_alloc_sock()
{
    struct tcp_sock *tsk = malloc(sizeof(struct tcp_sock));

    memset(tsk, 0, sizeof(struct tcp_sock));
    tsk->sk.state = TCP_CLOSE;
    tsk->sackok = 1;
    
    tsk->rmss = 1460;
    // Default to 536 as per spec
    tsk->smss = 536;
    /* RFC 5681 §3.1：IW 应按 SMSS 大小分三档（2/3/4 倍）。
     * 这里先简化为固定 4*SMSS，待 MSS 协商完善后再细化。
     * TODO: implement IW table per RFC 5681 §3.1 */
    tsk->cwnd = 4 * tsk->smss;
    tsk->ssthresh = 0xFFFFFFFFu;
    tsk->inflight = 0;
    tsk->bytes_acked = 0;
    tsk->dupacks = 0;
    tsk->in_recovery = 0;
    tsk->last_ack_win = 0;

    skb_queue_init(&tsk->ofo_queue);
    
    return (struct sock *)tsk;
}

int tcp_v4_init_sock(struct sock *sk)
{
    tcp_init_sock(sk);
    return 0;
}

int tcp_init_sock(struct sock *sk)
{
    return 0;
}

void __tcp_set_state(struct sock *sk, uint32_t state)
{
    sk->state = state;
}

static uint16_t generate_port()
{
    /* TODO: Generate a proper port */
    static int port = 40000;

    pthread_rwlock_wrlock(&tcplock);
    int copy =  ++port + (timer_get_tick() % 10000);
    pthread_rwlock_unlock(&tcplock);

    return copy;
}

int generate_iss()
{
    /* TODO: Generate a proper ISS */
    return (int)time(NULL) * rand();
}

int tcp_v4_connect(struct sock *sk, const struct sockaddr *addr, socklen_t addrlen, int flags)
{
    uint16_t dport = ((struct sockaddr_in *)addr)->sin_port;
    uint32_t daddr = ((struct sockaddr_in *)addr)->sin_addr.s_addr;

    sk->dport = ntohs(dport);
    sk->sport = generate_port();
    sk->daddr.v4 = ntohl(daddr);
    /* TODO: Do not hardcode lvl-ip local interface */
    sk->saddr.v4 = parse_ipv4_string("10.0.0.4");

    return tcp_connect(sk);
}

/*
 * tcp_v6_connect - TCP IPv6 connect (04 §3.6)
 * @sk:      socket
 * @addr:    sockaddr_in6 destination
 * @addrlen: address length
 * @flags:   connect flags (unused)
 */
int tcp_v6_connect(struct sock *sk, const struct sockaddr *addr, socklen_t addrlen, int flags)
{
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;
    struct rtentry *rt = NULL;
    struct tcp_sock *tsk = NULL;
    uint16_t pmtu = 0;

    if (addrlen < sizeof(struct sockaddr_in6) || addr->sa_family != AF_INET6) {
        return -EINVAL;
    }

    sk->dport = ntohs(sin6->sin6_port);
    sk->sport = generate_port();
    memcpy(&sk->daddr.v6, &sin6->sin6_addr, sizeof(struct in6_addr));

    /* TODO: proper source address selection (routing-based) */
    memset(&sk->saddr.v6, 0, sizeof(struct in6_addr));

    /* Adjust MSS for IPv6: PMTU - 60 (40 IPv6 + 20 TCP) */
    tsk = tcp_sk(sk);
    rt = route6_lookup(&sk->daddr.v6);
    if (rt != NULL && rt->pmtu > 0) {
        pmtu = rt->pmtu;
    } else if (rt != NULL && rt->dev != NULL) {
        pmtu = (uint16_t)(rt->dev->mtu6 > 0 ? rt->dev->mtu6 : rt->dev->mtu);
    } else {
        pmtu = 1500;
    }

    tsk->smss = pmtu - 60;
    tsk->rmss = pmtu - 60;

    return tcp_connect(sk);
}

/*
 * tcp_v6_bind - TCP IPv6 bind
 */
int tcp_v6_bind(struct sock *sk, const struct sockaddr *addr, socklen_t addr_len)
{
    struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)addr;

    if (addr_len < sizeof(struct sockaddr_in6) || addr->sa_family != AF_INET6) {
        return -EINVAL;
    }

    sk->sport = ntohs(sin6->sin6_port);
    memcpy(&sk->saddr.v6, &sin6->sin6_addr, sizeof(struct in6_addr));

    return 0;
}

int tcp_disconnect(struct sock *sk, int flags)
{
    return 0;
}

int tcp_v4_bind(struct sock *sk, const struct sockaddr *addr, socklen_t addr_len)
{
    uint16_t sport = sockaddr_port(addr);
    uint32_t saddr = sockaddr_addr(addr);

    sk->sport = ntohs(sport);
    sk->saddr.v4 = ntohl(saddr);

    return 0;
}

int tcp_v4_listen(struct sock *sk, int n)
{
    int ret = 0;

    switch (sk->state) {
    case TCP_CLOSE:
        tcp_set_state(sk, TCP_LISTEN);
        break;
    default:
        ret = EOPNOTSUPP;
        goto out;
    }

    // init half conn and conned queue
    struct tcp_sock *tsk = tcp_sk(sk);
    tsk->tcp_passive_conn_queue.max_conn = n;
    conn_queue_init(&tsk->tcp_passive_conn_queue.establied_conn_queue);
    wait_init(&tsk->tcp_passive_conn_queue.recv_wait);

out:
    return ret;
}

static inline int get_established_conn_fd(struct tcp_sock *tsk)
{
    struct conn_head *list = &tsk->tcp_passive_conn_queue.establied_conn_queue;
    struct socket *sock = tsk->sk.sock;

    for (;;) {
        if (conn_queue_len(list) > 0) {
            struct conn_info * conn_info = conn_dequeue(list);
            return conn_info->sk->fd;
        } else {
            if (sock->flags & O_NONBLOCK) {
                return -EAGAIN;
            } else {
                pthread_mutex_lock(&tsk->tcp_passive_conn_queue.recv_wait.lock);
                socket_release(sock);
                wait_sleep(&tsk->tcp_passive_conn_queue.recv_wait);
                pthread_mutex_unlock(&tsk->tcp_passive_conn_queue.recv_wait.lock);
                socket_wr_acquire(sock);
            }
        }
    }

    return -1;
}

int tcp_v4_accept(struct sock *sk, struct sockaddr *__restrict__ addr, socklen_t *__restrict__ addr_len)
{
    struct tcp_sock *tsk = tcp_sk(sk);

    // 1. check sk state
    if (sk->state != TCP_LISTEN) {
        print_err("TCP synack: Socket was not in correct state (TCP_LISTEN)\n");
        return -1;
    }
    // 2. wait, until new connect create
    int fd = get_established_conn_fd(tsk);
    struct socket *sock = get_socket(tsk->sk.sock->pid, fd);

    build_sockaddr_from_host_order(sock->sk->dport, sock->sk->daddr.v4, addr, addr_len);

    return fd;
}

int tcp_write(struct sock *sk, const void *buf, int len)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    int ret = sk->err;

    if (ret != 0) goto out;

    switch (sk->state) {
    case TCP_ESTABLISHED:
    case TCP_CLOSE_WAIT:
        break;
    default:
        ret = -EBADF;
        goto out;
    }

    return tcp_send(tsk, buf, len);    

out: 
    return ret;
}

int tcp_read(struct sock *sk, void *buf, int len)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    int ret = -1;

    switch (sk->state) {
    case TCP_CLOSE:
        ret = -EBADF;
        goto out;
    case TCP_LISTEN:
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
        /* Queue for processing after entering ESTABLISHED state.  If there
           is no room to queue this request, respond with "error:
           insufficient resources". */
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        /* If insufficient incoming segments are queued to satisfy the
           request, queue the request. */
        
        break;
    case TCP_CLOSE_WAIT:
        /* If no text is awaiting delivery, the RECEIVE will get a
           "error:  connection closing" response.  Otherwise, any remaining
           text can be used to satisfy the RECEIVE. */
        if (!skb_queue_empty(&tsk->sk.receive_queue)) break;
        if (tsk->flags & TCP_FIN) {
            tsk->flags &= ~TCP_FIN;
            return 0;
        }

        break;
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
        ret = sk->err;
        goto out;
    default:
        goto out;
    }

    return tcp_receive(tsk, buf, len);    

out: 
    return ret;
}

int tcp_recv_notify(struct sock *sk)
{
    if (!sk) {
        return -1;
    }
    return wait_wakeup(&sk->recv_wait);
}

int tcp_close(struct sock *sk)
{
    switch (sk->state) {
    case TCP_CLOSE:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        /* Respond with "error:  connection closing". */
        sk->err = -EBADF;
        return -1;
    case TCP_LISTEN:
    case TCP_SYN_SENT:
    case TCP_SYN_RECEIVED:
        return tcp_done(sk);
    case TCP_ESTABLISHED:
        /* Queue this until all preceding SENDs have been segmentized, then
           form a FIN segment and send it.  In any case, enter FIN-WAIT-1
           state. */
        tcp_set_state(sk, TCP_FIN_WAIT_1);
        tcp_queue_fin(sk);
        break;
    case TCP_CLOSE_WAIT:
        /* Queue this request until all preceding SENDs have been
           segmentized; then send a FIN segment, enter LAST_ACK state. */
        tcp_queue_fin(sk);
        tcp_set_state(sk, TCP_LAST_ACK);
        break;
    default:
        print_err("Unknown TCP state for close\n");
        return -1;
    }

    return 0;
}

int tcp_abort(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    tcp_send_reset(tsk);
    return tcp_done(sk);
}

static int tcp_free(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);

    tcp_clear_timers(sk);
    tcp_clear_queues(tsk);

    wait_wakeup(&sk->sock->sleep);

    return 0;
}

int tcp_done(struct sock *sk)
{
    tcp_set_state(sk, TCP_CLOSING);
    tcp_free(sk);
    return socket_delete(sk->sock);
}

void tcp_clear_timers(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    tcp_stop_rto_timer(tsk);
    tcp_stop_delack_timer(tsk);

    timer_cancel(tsk->keepalive);
    tsk->keepalive = NULL;
    timer_cancel(tsk->linger);
    tsk->linger = NULL;
}

void tcp_stop_rto_timer(struct tcp_sock *tsk)
{
    if (tsk) {
        timer_cancel(tsk->retransmit);
        tsk->retransmit = NULL;
        tsk->backoff = 0;
        /* 推进代号：即使已经 spawn 出去的回调还没拿到锁，也会被作废 */
        tsk->rto_epoch++;
    }
}

void tcp_release_rto_timer(struct tcp_sock *tsk)
{
    if (tsk) {
        timer_release(tsk->retransmit);
        tsk->retransmit = NULL;
    }
}

void tcp_stop_delack_timer(struct tcp_sock *tsk)
{
    timer_cancel(tsk->delack);
    tsk->delack = NULL;
}

void tcp_release_delack_timer(struct tcp_sock *tsk)
{
    timer_release(tsk->delack);
    tsk->delack = NULL;
}

void tcp_handle_fin_state(struct sock *sk)
{
    switch (sk->state) {
    case TCP_CLOSE_WAIT:
        tcp_set_state(sk, TCP_LAST_ACK);
        break;
    case TCP_ESTABLISHED:
        tcp_set_state(sk, TCP_FIN_WAIT_1);
        break;
    }
}

static void *tcp_linger(void *arg)
{
    struct sock *sk = (struct sock *) arg;
    socket_wr_acquire(sk->sock);
    struct tcp_sock *tsk = tcp_sk(sk);
    tcpsock_dbg("TCP time-wait timeout, freeing TCB", sk);

    timer_cancel(tsk->linger);
    tsk->linger = NULL;

    tcp_done(sk);
    socket_release(sk->sock);

    return NULL;
}

static void *tcp_user_timeout(void *arg)
{
    struct sock *sk = (struct sock *) arg;
    socket_wr_acquire(sk->sock);
    struct tcp_sock *tsk = tcp_sk(sk);
    tcpsock_dbg("TCP user timeout, freeing TCB and aborting conn", sk);

    timer_cancel(tsk->linger);
    tsk->linger = NULL;

    tcp_abort(sk);
    socket_release(sk->sock);
    
    return NULL;
}

void tcp_enter_time_wait(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);

    tcp_set_state(sk, TCP_TIME_WAIT);

    tcp_clear_timers(sk);
    /* RFC793 arbitrarily defines MSL to be 2 minutes */
    tsk->linger = timer_add(TCP_2MSL, &tcp_linger, sk);
}

void tcp_rearm_user_timeout(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);

    if (sk->state == TCP_TIME_WAIT) return;

    timer_cancel(tsk->linger);
    /* RFC793 set user timeout */
    tsk->linger = timer_add(TCP_USER_TIMEOUT, &tcp_user_timeout, sk);
}

void tcp_rtt(struct tcp_sock *tsk)
{
    if (tsk->backoff > 0 || !tsk->retransmit) {
        // Karn's Algorithm: Don't measure retransmissions
        return;
    }

    int r = timer_get_tick() - (tsk->retransmit->expires - tsk->rto);
    if (r < 0) return;

    if (!tsk->srtt) {
        /* RFC6298 2.2 first measurement is made */
        tsk->srtt = r;
        tsk->rttvar = r / 2;
    } else {
        /* RFC6298 2.3 a subsequent measurement is made */
        double beta = 0.25;
        double alpha = 0.125;
        tsk->rttvar = (1 - beta) * tsk->rttvar + beta * abs(tsk->srtt - r);
        tsk->srtt = (1 - alpha) * tsk->srtt + alpha * r;
    }

    int k = 4 * tsk->rttvar;

    /* RFC6298 says RTO should be at least 1 second. Linux uses 200ms */
    if (k < 200) k = 200;

    tsk->rto = tsk->srtt + k;
}

/* RFC 2018 §4：第一个 block 必须是触发本次 ACK 的最新段，
 * 后续 blocks 按 ofo_queue 顺序填入（跳过与第一个 block 重叠的段）。*/
int tcp_calculate_sacks(struct tcp_sock *tsk, uint32_t trigger_seq, uint32_t trigger_end_seq)
{
    struct tcp_sack_block *sb = NULL;
    struct sk_buff *next = NULL;
    struct list_head *item = NULL;
    struct list_head *tmp = NULL;

    memset(tsk->sacks, 0, sizeof(tsk->sacks));
    tsk->sacklen = 0;

    tsk->sacks[0].left = trigger_seq;
    tsk->sacks[0].right = trigger_end_seq;
    tsk->sacklen = 1;

    sb = &tsk->sacks[1];

    list_for_each_safe(item, tmp, &tsk->ofo_queue.head) {
        next = list_entry(item, struct sk_buff, list);

        if (next->seq >= trigger_seq && next->end_seq <= trigger_end_seq) {
            continue;
        }

        if (tsk->sacklen >= tsk->sacks_allowed) {
            break;
        }

        if (sb->left == 0) {
            sb->left = next->seq;
            sb->right = next->end_seq;
            tsk->sacklen++;
        } else if (sb->right == next->seq) {
            sb->right = next->end_seq;
        } else {
            if (tsk->sacklen >= tsk->sacks_allowed) {
                break;
            }
            sb = &tsk->sacks[tsk->sacklen];
            sb->left = next->seq;
            sb->right = next->end_seq;
            tsk->sacklen++;
        }
    }

    return 0;
}
