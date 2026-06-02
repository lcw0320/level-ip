#include "syshead.h"
#include "utils.h"
#include "tcp.h"
#include "ip.h"
#include "ipv6.h"
#include "skbuff.h"
#include "timer.h"
#include "route.h"
#include "dst.h"

static void *tcp_retransmission_timeout(void *arg);

static struct sk_buff *tcp_alloc_skb(uint8_t family, int optlen, int size)
{
    int ip_hlen = (family == AF_INET6) ? IPV6_HDR_LEN : IP_HDR_LEN;
    int reserved = ETH_HDR_LEN + ip_hlen + TCP_HDR_LEN + optlen + size;
    struct sk_buff *skb = alloc_skb(reserved);

    skb_reserve(skb, reserved);
    skb->protocol = IP_TCP;
    skb->dlen = size;
    skb->seq = 0;

    return skb;
}

static int tcp_write_syn_options(struct tcphdr *th, struct tcp_options *opts, int optlen)
{
    struct tcp_opt_mss *opt_mss = (struct tcp_opt_mss *) th->data;
    uint32_t i = 0;

    opt_mss->kind = TCP_OPT_MSS;
    opt_mss->len = TCP_OPTLEN_MSS;
    opt_mss->mss = htons(opts->mss);

    i += sizeof(struct tcp_opt_mss);

    if (opts->sack) {
        th->data[i++] = TCP_OPT_NOOP;
        th->data[i++] = TCP_OPT_NOOP;
        th->data[i++] = TCP_OPT_SACK_OK;
        th->data[i++] = TCP_OPTLEN_SACK;
    }

    th->hl = TCP_DOFFSET + (optlen / 4);

    return 0;
}

static int tcp_syn_options(struct sock *sk, struct tcp_options *opts)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    int optlen = 0;

    opts->mss = tsk->rmss;
    optlen += TCP_OPTLEN_MSS;

    if (tsk->sackok) {
        opts->sack = 1;
        optlen += TCP_OPT_NOOP * 2;
        optlen += TCP_OPTLEN_SACK;
    } else {
        opts->sack = 0;
    }
    
    return optlen;
}

static int tcp_write_options(struct tcp_sock *tsk, struct tcphdr *th)
{
    uint8_t *ptr = th->data;
    struct tcp_opt_wso *wso = NULL;
    struct tcp_sack_block *sb = NULL;
    int i = 0;

    if (tsk->wso_allowed) {
        *ptr++ = TCP_OPT_NOOP;
        wso = (struct tcp_opt_wso *)ptr;
        wso->kind = TCP_OPT_WSO;
        wso->len = TCP_OPTLEN_WSO;
        wso->wso = tsk->rcv_scale;
        ptr += sizeof(struct tcp_opt_wso);
    }

    if (tsk->sackok && tsk->sacklen > 0 && tsk->sacks[0].left != 0) {
        *ptr++ = TCP_OPT_NOOP;
        *ptr++ = TCP_OPT_NOOP;
        *ptr++ = TCP_OPT_SACK;
        *ptr++ = 2 + tsk->sacklen * 8;

        sb = (struct tcp_sack_block *)ptr;

        for (i = 0; i < tsk->sacklen; i++) {
            sb->left = htonl(tsk->sacks[i].left);
            sb->right = htonl(tsk->sacks[i].right);
            sb++;
            ptr += sizeof(struct tcp_sack_block);
        }

        memset(tsk->sacks, 0, sizeof(tsk->sacks));
        tsk->sacklen = 0;
    }

    return 0;
}

static int tcp_transmit_skb(struct sock *sk, struct sk_buff *skb, uint32_t seq)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *thdr = tcp_hdr_for_sk(sk, skb);

    /* No options were previously set */
    if (thdr->hl == 0) thdr->hl = TCP_DOFFSET;

    thdr->sport = sk->sport;
    thdr->dport = sk->dport;
    thdr->seq = seq;
    thdr->ack_seq = tcb->rcv_nxt;
    thdr->rsvd = 0;
    thdr->win = tcb->real_rcv_wnd;
    if (!thdr->syn) {
        thdr->win = thdr->win >> tsk->rcv_scale;
    }
    thdr->csum = 0;
    thdr->urp = 0;

    if (thdr->hl > 5) {
        tcp_write_options(tsk, thdr);
    }

    if (sk->addr_family == AF_INET6) {
        /* ── IPv6 path ──────────────────────────────────────
         * tcp_alloc_skb reserved ETH + IPV6_HDR + TCP headroom.
         * We resolve the source addr, compute checksum, then
         * push the full packet (IPv6 hdr + TCP) in one step. */
        struct rtentry *rt = route6_lookup(&sk->daddr.v6);
        struct ipv6hdr *ip6h = NULL;
        int tcp_len = thdr->hl * 4;

        if (rt == NULL) {
            print_err("tcp_transmit_skb: IPv6 route lookup failed\n");
            free_skb(skb);
            return -1;
        }
        skb->dev = rt->dev;
        skb->rt = rt;

        /* Resolve source address if unspecified */
        if (ipv6_addr_is_unspecified(&sk->saddr.v6)) {
            if (skb->dev->addr6_global_valid) {
                memcpy(&sk->saddr.v6, &skb->dev->addr6_global,
                       sizeof(struct in6_addr));
            } else {
                memcpy(&sk->saddr.v6, &skb->dev->addr6_ll,
                       sizeof(struct in6_addr));
            }
        }

        /* TCP checksum with the real source address */
        thdr->csum = tcp_v6_checksum(skb, &sk->saddr.v6, &sk->daddr.v6);

        tcp_out_dbg(thdr, sk, skb);

        /* Convert TCP header fields to network byte order */
        thdr->sport = htons(thdr->sport);
        thdr->dport = htons(thdr->dport);
        thdr->seq = htonl(thdr->seq);
        thdr->ack_seq = htonl(thdr->ack_seq);
        thdr->win = htons(thdr->win);
        thdr->csum = htons(thdr->csum);
        thdr->urp = htons(thdr->urp);

        /* Push IPv6 header + TCP header in one step */
        skb_push(skb, IPV6_HDR_LEN + tcp_len);
        ip6h = (struct ipv6hdr *)skb->data;
        ipv6_hdr_set_vtc_flow(ip6h, IPV6_VERSION, 0, 0);
        ip6h->payload_len = htons((uint16_t)tcp_len);
        ip6h->nexthdr = NEXTHDR_TCP;
        ip6h->hop_limit = IPV6_DEFAULT_HOPLIMIT;
        memcpy(&ip6h->saddr, &sk->saddr.v6, sizeof(struct in6_addr));
        memcpy(&ip6h->daddr, &sk->daddr.v6, sizeof(struct in6_addr));

        ipv6_dbg("out", ip6h);

        return dst6_neigh_output(skb);
    }

    /* ── IPv4 path ────────────────────────────────────────── */
    skb_push(skb, thdr->hl * 4);

    tcp_out_dbg(thdr, sk, skb);

    thdr->sport = htons(thdr->sport);
    thdr->dport = htons(thdr->dport);
    thdr->seq = htonl(thdr->seq);
    thdr->ack_seq = htonl(thdr->ack_seq);
    thdr->win = htons(thdr->win);
    thdr->csum = htons(thdr->csum);
    thdr->urp = htons(thdr->urp);

    thdr->csum = tcp_v4_checksum(skb, htonl(sk->saddr.v4), htonl(sk->daddr.v4));

    return ip_output(sk, skb);
}

/* 发送窗口反压：FlightSize 超过 min(cwnd, rwnd) 就释放锁睡眠，
 * 由 ACK 路径释放窗口后唤醒，确保只有真正发出去的 skb 才进入 write_queue。 */
static void tcp_wait_snd_wnd(struct sock *sk, uint32_t bytes)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    uint32_t snd_wnd = 0;

    snd_wnd = min(tsk->cwnd, tcb->snd_wnd);
    while (tsk->inflight + bytes > snd_wnd) {
        pthread_mutex_lock(&sk->write_wait.lock);
        socket_release(sk->sock);
        wait_sleep(&sk->write_wait);
        pthread_mutex_unlock(&sk->write_wait.lock);
        socket_wr_acquire(sk->sock);
        snd_wnd = min(tsk->cwnd, tcb->snd_wnd);
    }
}

static int tcp_queue_transmit_skb(struct sock *sk, struct sk_buff *skb)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *th = tcp_hdr_for_sk(sk, skb);
    int was_empty = 0;
    int rc = 0;

    /* RFC 5681：FlightSize 不能超过 min(cwnd, rwnd)，等窗口打开再发 */
    tcp_wait_snd_wnd(sk, skb->dlen);

    was_empty = skb_queue_empty(&sk->write_queue);

    skb->seq = tcb->snd_nxt;
    tcb->snd_nxt += skb->dlen;
    skb->end_seq = tcb->snd_nxt;
    if (th->fin) {
        tcb->snd_nxt++;
    }
    tsk->inflight += skb->dlen;

    /* 先入队再发送，保证 write_queue 始终按 seq 升序排列；
     * 否则发送在前、入队在后，期间释放锁会让 ACK 处理看到错位的队列。 */
    skb_queue_tail(&sk->write_queue, skb);

    /* RTO 与 inflight 绑定：原本队列空（inflight 必为 0）才需要 arm */
    if (was_empty) {
        tcp_rearm_rto_timer(tsk);
    }

    rc = tcp_transmit_skb(sk, skb, skb->seq);

    return rc;
}

int tcp_send_synack(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT && sk->state != TCP_SYN_RECEIVED) {
        print_err("TCP synack: Socket was not in correct state (SYN_SENT) (TCP_SYN_RECEIVED)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb * tcb = &tcp_sk(sk)->tcb;
    int rc = 0;
    // todo: set correct hl, now only send window scale
    int hl = 6;

    skb = tcp_alloc_skb(sk->addr_family, (hl - 5) << 2, 0);
    th = tcp_hdr_for_sk(sk, skb);

    th->syn = 1;
    th->ack = 1;
    th->hl = 6;

    rc = tcp_queue_transmit_skb(sk, skb);
    tcb->snd_nxt++;

    return rc;
}

/* Routine for timer-invoked delayed acknowledgment */
void *tcp_send_delack(void *arg)
{
    struct sock *sk = (struct sock *) arg;
    socket_wr_acquire(sk->sock);

    struct tcp_sock *tsk = tcp_sk(sk);
    tsk->delacks = 0;
    tcp_release_delack_timer(tsk);
    tcp_send_ack(sk);

    socket_release(sk->sock);

    return NULL;
}

/* todo： now it is useless */
int tcp_send_next(struct sock *sk, int amount, uint32_t extra)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    struct tcphdr *th;
    struct sk_buff *next;
    struct list_head *item, *tmp;
    uint32_t snd_wnd = 0;
    int i = 0;

    /* Limited Transmit / Fast Recovery 期间允许临时多发 extra 字节 */
    snd_wnd = min(tsk->cwnd + extra, tcb->snd_wnd);

    list_for_each_safe(item, tmp, &sk->write_queue.head) {
        if (++i > amount) {
            break;
        }
        next = list_entry(item, struct sk_buff, list);

        if (next == NULL) {
            return -1;
        }

        /* 跳过已发送过的 skb（seq 不为 0 表示已分配过序号） */
        if (next->seq != 0) {
            continue;
        }

        /* 受 cwnd 和 rwnd 限制，发不下就停 */
        if (tsk->inflight + next->dlen > snd_wnd) {
            break;
        }

        skb_reset_header(next);
        tcp_transmit_skb(sk, next, tcb->snd_nxt);

        next->seq = tcb->snd_nxt;
        tcb->snd_nxt += next->dlen;
        next->end_seq = tcb->snd_nxt;
        tsk->inflight += next->dlen;

        th = tcp_hdr(next);
        if (th->fin) {
            tcb->snd_nxt++;
        }
    }

    return 0;
}

static int tcp_options_len(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    uint8_t optlen = 0;

    if (tsk->wso_allowed) {
        optlen += 1 + TCP_OPTLEN_WSO;  /* 1 NOOP + 3 WSO */
    }

    if (tsk->sackok && tsk->sacklen > 0 && tsk->sacks[0].left != 0) {
        optlen += 2 + 2 + tsk->sacklen * 8;  /* 2 NOOP + kind + len + blocks */
    }

    while (optlen % 4 > 0) {
        optlen++;
    }

    return optlen;
}

int tcp_send_ack(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;
    
    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb *tcb = &tcp_sk(sk)->tcb;
    int rc = 0;
    int optlen = tcp_options_len(sk);

    skb = tcp_alloc_skb(sk->addr_family, optlen, 0);
    
    th = tcp_hdr_for_sk(sk, skb);
    th->ack = 1;
    th->hl = TCP_DOFFSET + (optlen / 4);

    rc = tcp_transmit_skb(sk, skb, tcb->snd_nxt);
    free_skb(skb);

    return rc;
}

static int tcp_send_syn(struct sock *sk)
{
    if (sk->state != TCP_SYN_SENT && sk->state != TCP_CLOSE && sk->state != TCP_LISTEN) {
        print_err("Socket was not in correct state (closed or listen)\n");
        return 1;
    }

    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcp_options opts = { 0 };
    int tcp_options_len = 0;

    tcp_options_len = tcp_syn_options(sk, &opts);
    skb = tcp_alloc_skb(sk->addr_family, tcp_options_len, 0);
    th = tcp_hdr_for_sk(sk, skb);

    tcp_write_syn_options(th, &opts, tcp_options_len);
    sk->state = TCP_SYN_SENT;
    th->syn = 1;

    return tcp_queue_transmit_skb(sk, skb);
}

int tcp_send_fin(struct sock *sk)
{
    if (sk->state == TCP_CLOSE) return 0;

    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(sk->addr_family, 0, 0);
    
    th = tcp_hdr_for_sk(sk, skb);
    th->fin = 1;
    th->ack = 1;

    rc = tcp_queue_transmit_skb(sk, skb);

    return rc;
}

void tcp_select_initial_window(uint32_t *rcv_wnd)
{
    *rcv_wnd = 44477;
}

static void tcp_notify_user(struct sock *sk)
{
    switch (sk->state) {
    case TCP_CLOSE_WAIT:
        wait_wakeup(&sk->sock->sleep);
        break;
    }
}

static void *tcp_connect_rto(void *arg)
{
    struct rto_timer_arg *targ = (struct rto_timer_arg *) arg;
    struct tcp_sock *tsk = targ->tsk;
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;

    socket_wr_acquire(sk->sock);

    /* 拿到锁后再校验：rearm/stop 都会把 epoch 推到下一个值，
     * 此处 epoch 不一致说明这次定时器已被取消或被新一轮覆盖。 */
    if (targ->epoch != tsk->rto_epoch) {
        socket_release(sk->sock);
        free(targ);
        return NULL;
    }

    tcp_release_rto_timer(tsk);

    if (sk->state == TCP_SYN_SENT) {
        if (tsk->backoff > TCP_CONN_RETRIES) {
            tsk->sk.err = -ETIMEDOUT;
            sk->poll_events |= (POLLOUT | POLLERR | POLLHUP);
            tcp_done(sk);
        } else {
            struct sk_buff *skb = write_queue_head(sk);

            if (skb) {
                skb_reset_header(skb);
                tcp_transmit_skb(sk, skb, tcb->snd_una);
            
                tsk->backoff++;
                tcp_rearm_rto_timer(tsk);
            }
         }
    } else {
        print_err("TCP connect RTO triggered even when not in SYNSENT\n");
    }

    socket_release(sk->sock);
    free(targ);

    return NULL;
}

/* RFC 2018 §5: 超时后接收方可能 renege，清空所有 SACK 标记从 snd_una 重传 */
static void tcp_sack_clear(struct tcp_sock *tsk)
{
    struct sock *sk = &tsk->sk;
    struct sk_buff *s = NULL;
    struct list_head *it = NULL;
    struct list_head *tmp = NULL;

    list_for_each_safe(it, tmp, &sk->write_queue.head) {
        s = list_entry(it, struct sk_buff, list);
        s->sacked = 0;
    }
    tsk->sack_max_right = 0;
}

static void *tcp_retransmission_timeout(void *arg)
{
    struct rto_timer_arg *targ = (struct rto_timer_arg *) arg;
    struct tcp_sock *tsk = targ->tsk;
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;
    struct tcphdr *th = NULL;
    struct sk_buff *skb = NULL;

    socket_wr_acquire(sk->sock);

    /* 拿到锁后再校验 epoch：rearm/stop 都会推进 epoch，
     * 不一致表示这次定时器已被取消或被新一轮覆盖，作废即可。 */
    if (targ->epoch != tsk->rto_epoch) {
        socket_release(sk->sock);
        free(targ);
        return NULL;
    }

    tcp_release_rto_timer(tsk);

    skb = write_queue_head(sk);

    if (!skb) {
        tsk->backoff = 0;
        tcpsock_dbg("TCP RTO queue empty, notifying user", sk);
        tcp_notify_user(sk);
        goto unlock;
    }

    /* todo: should deal with zero window */

    /* RFC 5681 §3.1：RTO 表示丢包，更新 ssthresh、cwnd 回到 1 SMSS 重新慢启动。
     * TODO: 同一段被多次 RTO 重传时 ssthresh 应保持不变，需要按 skb 记录重传次数 */
    tsk->ssthresh = max(tsk->inflight / 2, (uint32_t)(2 * tsk->smss));
    tsk->cwnd = tsk->smss;
    tsk->bytes_acked = 0;

    tcp_sack_clear(tsk);

    th = tcp_hdr_for_sk(sk, skb);
    skb_reset_header(skb);

    tcp_transmit_skb(sk, skb, tcb->snd_una);
    /* RFC 6298: 2.5 Maximum value MAY be placed on RTO, provided it is at least
       60 seconds */
    if (tsk->rto > 60000) {
        tcp_done(sk);

        tsk->sk.err = -ETIMEDOUT;
        sk->poll_events |= (POLLOUT | POLLERR | POLLHUP);

        socket_release(sk->sock);
        free(targ);
        return NULL;
    } else {
        /* RFC 6298: Section 5.5 double RTO time */
        tsk->rto *= 2;
        tsk->backoff++;
        tcp_rearm_rto_timer(tsk);

        if (th->fin) {
            tcp_handle_fin_state(sk);
        }
    }

unlock:
    socket_release(sk->sock);
    free(targ);

    return NULL;
}

void tcp_rearm_rto_timer(struct tcp_sock *tsk)
{
    struct sock *sk = &tsk->sk;
    struct rto_timer_arg *arg = NULL;

    tcp_release_rto_timer(tsk);

    /* 推进代号让上一次（如果还有遗留的 detached 回调）作废 */
    tsk->rto_epoch++;

    arg = malloc(sizeof(*arg));
    if (arg == NULL) {
        return;
    }
    arg->tsk = tsk;
    arg->epoch = tsk->rto_epoch;

    if (sk->state == TCP_SYN_SENT) {
        tsk->retransmit = timer_add_with_release(TCP_SYN_BACKOFF << tsk->backoff,
                                                 &tcp_connect_rto, arg, free);
    } else {
        tsk->retransmit = timer_add_with_release(tsk->rto,
                                                 &tcp_retransmission_timeout, arg, free);
    }
}

int tcp_connect(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;
    int rc = 0;
    
    tsk->tcp_header_len = sizeof(struct tcphdr);
    tcb->iss = generate_iss();
    tcb->snd_wnd = 0;
    tcb->snd_wl1 = 0;

    tcb->snd_una = tcb->iss;
    tcb->snd_up = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->rcv_nxt = 0;

    tcp_select_initial_window(&tsk->tcb.rcv_wnd);
    tsk->tcb.real_rcv_wnd = tsk->tcb.rcv_wnd;
    tcb->real_rcv_wnd = tcb->rcv_wnd;

    rc = tcp_send_syn(sk);
    tcb->snd_nxt++;
    
    return rc;
}

int tcp_send(struct tcp_sock *tsk, const void *buf, int len)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    int slen = len;
    int mss = tsk->smss;
    int dlen = 0;

    while (slen > 0) {
        dlen = slen > mss ? mss : slen;
        slen -= dlen;

        skb = tcp_alloc_skb(tsk->sk.addr_family, 0, dlen);
        skb_push(skb, dlen);
        memcpy(skb->data, buf, dlen);
        
        buf += dlen;

        th = tcp_hdr_for_sk(&tsk->sk, skb);
        th->ack = 1;

        if (slen == 0) {
            th->psh = 1;
        }

        if (tcp_queue_transmit_skb(&tsk->sk, skb) == -1) {
            perror("Error on TCP skb queueing");
        }
    }

    tcp_rearm_user_timeout(&tsk->sk);
    
    return len;
}

int tcp_send_reset(struct tcp_sock *tsk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    struct tcb *tcb;
    int rc = 0;

    skb = tcp_alloc_skb(tsk->sk.addr_family, 0, 0);
    th = tcp_hdr_for_sk(&tsk->sk, skb);
    tcb = &tsk->tcb;

    th->rst = 1;
    tcb->snd_una = tcb->snd_nxt;

    rc = tcp_transmit_skb(&tsk->sk, skb, tcb->snd_nxt);
    free_skb(skb);

    return rc;
}

int tcp_send_challenge_ack(struct sock *sk, struct sk_buff *skb)
{
    // TODO: implement me
    return 0;
}

int tcp_queue_fin(struct sock *sk)
{
    struct sk_buff *skb;
    struct tcphdr *th;
    int rc = 0;

    skb = tcp_alloc_skb(sk->addr_family, 0, 0);
    th = tcp_hdr_for_sk(sk, skb);

    th->fin = 1;
    th->ack = 1;

    tcpsock_dbg("Queueing fin", sk);

    rc = tcp_queue_transmit_skb(sk, skb);

    return rc;
}

static struct sk_buff *tcp_retransmit_candidate(struct tcp_sock *tsk)
{
    struct sock *sk = &tsk->sk;
    struct sk_buff *skb = NULL;
    struct list_head *item = NULL;
    struct list_head *tmp = NULL;

    if (tsk->sack_max_right > 0) {
        list_for_each_safe(item, tmp, &sk->write_queue.head) {
            skb = list_entry(item, struct sk_buff, list);
            if (!skb->sacked && skb->end_seq <= tsk->sack_max_right) {
                return skb;
            }
        }
    }

    return write_queue_head(sk);
}

/* RFC 5681 §3.2 step 3：重传 write_queue 中第一个未被 SACK 确认的丢失段。
 * 若存在 sack_max_right，跳过已 sacked 的段只重传空洞；否则退化为重传队首。 */
int tcp_fast_retransmit(struct tcp_sock *tsk)
{
    struct sk_buff *candidate = NULL;
    int rc = 0;

    candidate = tcp_retransmit_candidate(tsk);
    if (candidate == NULL) {
        return 0;
    }

    skb_reset_header(candidate);
    rc = tcp_transmit_skb(&tsk->sk, candidate, candidate->seq);

    return rc;
}
