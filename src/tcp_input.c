#include "syshead.h"
#include "tcp_passive_conn.h"
#include "tcp.h"
#include "tcp_data.h"
#include "skbuff.h"
#include "sock.h"

static int tcp_parse_opts(struct tcp_sock *tsk, struct tcphdr *th)
{
    uint8_t *ptr = th->data;
    uint8_t optlen = tcp_hlen(th) - 20;
    // struct tcp_opt_mss *opt_mss = NULL;
    struct tcp_opt_wso *opt_wso = NULL;
    uint8_t sack_seen = 0;
    uint8_t tsopt_seen = 0;
    
    while (optlen > 0 && optlen < 30) {
        switch (*ptr) {
        case TCP_OPT_MSS:
            // opt_mss = (struct tcp_opt_mss *)ptr;
            // uint16_t mss = ntohs(opt_mss->mss);

            // if (mss > 536 && mss <= 1460) {
            //     tsk->smss = mss;
            // }

            ptr += sizeof(struct tcp_opt_mss);
            optlen -= 4;
            break;
        case TCP_OPT_NOOP:
            ptr += 1;
            optlen--;
            break;
        case TCP_OPT_SACK_OK:
            ptr += TCP_OPTLEN_SACK;
            // sack_seen = 1;
            optlen -= TCP_OPTLEN_SACK;
            break;
        case TCP_OPT_TS:
            ptr += TCP_OPTLEN_TS;
            // tsopt_seen = 1;
            optlen -= TCP_OPTLEN_TS;
            break;
        case TCP_OPT_WSO:
            opt_wso = (struct tcp_opt_wso *)ptr;
            uint8_t wso = opt_wso->wso;
            
            if (wso <= 14) {
                tsk->snd_scale = wso;
            } else {
                tsk->snd_scale = 14;
            }

            ptr += sizeof(struct tcp_opt_wso);
            optlen -= TCP_OPTLEN_WSO;
            break;
        default:
            print_err("Unrecognized TCPOPT\n");
            optlen--;
            break;
        }
    }

    if (!tsopt_seen) {
        tsk->tsopt = 0;
    }

    if (sack_seen && tsk->sackok) {
        // There's room for 4 sack blocks without TS OPT
        if (tsk->tsopt) tsk->sacks_allowed = 3;
        else tsk->sacks_allowed = 4;
    } else {
        tsk->sackok = 0;
    }

    return 0;
}

/*
 * RFC 5681 §3.1：根据被 ACK 的字节数增长 cwnd。
 * cwnd < ssthresh 时走慢启动（每 ACK 增 min(N, SMSS)）；
 * 否则走拥塞避免（每累计 cwnd 字节增一个 SMSS）。
 * 注意：fast recovery 期间（in_recovery == 1）不在此处增长 cwnd，
 *       cwnd 由 inflate/deflate 流程单独管理。
 */
static void tcp_cong_avoid(struct tcp_sock *tsk, uint32_t acked)
{
    if (acked == 0) {
        return;
    }

    if (tsk->in_recovery) {
        return;
    }

    if (tsk->cwnd < tsk->ssthresh) {
        tsk->cwnd += min(acked, tsk->smss);
    } else {
        tsk->bytes_acked += acked;
        if (tsk->bytes_acked >= tsk->cwnd) {
            tsk->bytes_acked -= tsk->cwnd;
            tsk->cwnd += tsk->smss;
        }
    }
}

/*
 * Acks all segments from retransmissionn queue that are "older"
 * than current unacknowledged sequence
 */
static int tcp_clean_rto_queue(struct sock *sk, uint32_t una)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct sk_buff *skb;
    uint32_t acked = 0;
    int rc = 0;

    while ((skb = skb_peek(&sk->write_queue)) != NULL) {
        if (skb->seq > 0 && skb->end_seq <= una) {
            /* skb fully acknowledged */
            acked = skb->dlen;
            skb_dequeue(&sk->write_queue);
            wait_wakeup(&sk->write_wait);
            skb->refcnt--;
            free_skb(skb);

            if (tsk->inflight >= acked) {
                tsk->inflight -= acked;
            } else {
                tsk->inflight = 0;
            }

            tcp_cong_avoid(tsk, acked);
        } else {
            break;
        }
    };

    if (skb == NULL || tsk->inflight == 0) {
        /* No unacknowledged skbs, stop rto timer */
        tcp_stop_rto_timer(tsk);
    }

    return rc;
}

/*
 * RFC 5681 §2 严格定义：判断收到的 ACK 是否为重复 ACK。
 * 五个条件全部满足才算：
 *   1) ack_seq == snd_una（确认号没推进）
 *   2) skb->dlen == 0（无数据）
 *   3) 无 SYN/FIN 标志
 *   4) th->win == last_ack_win（窗口未更新）
 *   5) inflight > 0（还有数据在飞）
 */
static int tcp_is_dupack(struct tcp_sock *tsk, struct tcphdr *th, struct sk_buff *skb)
{
    if (th->ack_seq != tsk->tcb.snd_una) {
        return 0;
    }

    if (skb->dlen != 0) {
        return 0;
    }

    if (th->syn || th->fin) {
        return 0;
    }

    if (th->win != tsk->last_ack_win) {
        return 0;
    }

    if (tsk->inflight == 0) {
        return 0;
    }

    return 1;
}

/*
 * RFC 5681 §3.2 step 2-5：进入 fast retransmit / fast recovery。
 *   ssthresh = max(FlightSize/2, 2*SMSS)
 *   重传 SND.UNA 起的丢失段
 *   cwnd = ssthresh + 3*SMSS  （inflate 3 个已离开网络的段）
 *   重置 RTO 定时器，给 fast recovery 留出完整 RTO 时间窗
 */
static void tcp_enter_fast_recovery(struct tcp_sock *tsk)
{
    tsk->ssthresh = max(tsk->inflight / 2, (uint32_t)(2 * tsk->smss));

    tcp_fast_retransmit(tsk);

    tsk->cwnd = tsk->ssthresh + 3 * tsk->smss;
    tsk->bytes_acked = 0;
    tsk->in_recovery = 1;

    tcp_rearm_rto_timer(tsk);
}

/*
 * RFC 5681 §3.2 step 6：收到推进 SND.UNA 的新 ACK，退出 fast recovery，
 * cwnd 放气回到 ssthresh。
 */
static void tcp_exit_fast_recovery(struct tcp_sock *tsk)
{
    tsk->cwnd = tsk->ssthresh;
    tsk->in_recovery = 0;
    tsk->dupacks = 0;
    tsk->bytes_acked = 0;
}

/*
 * RFC 5681 §3.2：收到 dupACK 时按 dupacks 计数分别处理。
 *   1、2 个 dupACK -> Limited Transmit（步骤 1）：临时允许多发 1 段新数据
 *   第 3 个 dupACK -> 进入 fast recovery（步骤 2-3），并尝试发 1 段新数据（步骤 5）
 *   第 4 个及以后  -> cwnd inflate 1*SMSS（步骤 4），并尝试发 1 段新数据（步骤 5）
 */
static void tcp_handle_dupack(struct sock *sk)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    int pending = 0;
    uint32_t extra = 0;

    tsk->dupacks++;

    if (tsk->dupacks < 3) {
        /* Limited Transmit：第 N 个 dupACK 临时放行 N*SMSS */
        extra = (uint32_t)tsk->dupacks * tsk->smss;
    } else if (tsk->dupacks == 3) {
        tcp_enter_fast_recovery(tsk);
        extra = 0;
    } else {
        tsk->cwnd += tsk->smss;
        extra = 0;
    }

    pending = skb_queue_len(&sk->write_queue);
    if (pending > 0) {
        tcp_send_next(sk, pending, extra);
    }
}

static inline int __tcp_drop(struct sock *sk, struct sk_buff *skb)
{
    free_skb(skb);
    return 0;
}

static int tcp_verify_segment(struct tcp_sock *tsk, struct tcphdr *th, struct sk_buff *skb)
{
    struct tcb *tcb = &tsk->tcb;

    if (skb->dlen > 0 && tcb->rcv_wnd == 0) return 0;

    if (th->seq < tcb->rcv_nxt ||
        th->seq > (tcb->rcv_nxt + tcb->rcv_wnd)) {
        tcpsock_dbg("Received invalid segment", (&tsk->sk));
        return 0;
    }

    return 1;
}

/* TCP RST received */
static void tcp_reset(struct sock *sk)
{
    sk->poll_events = (POLLOUT | POLLWRNORM | POLLERR | POLLHUP);
    switch (sk->state) {
    case TCP_SYN_SENT:
        sk->err = -ECONNREFUSED;
        break;
    case TCP_CLOSE_WAIT:
        sk->err = -EPIPE;
        break;
    case TCP_CLOSE:
        return;
    default:
        sk->err = -ECONNRESET;
        break;
    }

    tcp_done(sk);
}

static inline int tcp_discard(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    free_skb(skb);
    return 0;
}

static inline struct tcp_sock * fork_socket(pid_t pid, struct sk_buff *skb, struct tcphdr *th, uint32_t saddr, uint16_t sport, uint32_t daddr)
{
    int fd = -1;
    struct socket *sk = NULL;
    struct tcp_sock *tsk = NULL;
    struct tcb *tcb = NULL;

    fd = _socket(pid, AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sk = get_socket(pid, fd);
    tcpsock_dbg("tcp listen recv syn, create a new socket", sk->sk);

    // init tcb
    tcp_set_state(sk->sk, TCP_SYN_RECEIVED);
    tsk = tcp_sk(sk->sk);

    tcp_parse_opts(tsk, th);
    if (tsk->snd_scale) {
        tsk->wso_allowed = 1;
        tsk->rcv_scale = tsk->snd_scale;
    }

    tcb = &tsk->tcb;
    tcb->iss = generate_iss();
    tcb->rcv_nxt = th->seq + 1;
    tcp_select_initial_window(&tcb->rcv_wnd);
    tcb->real_rcv_wnd = tcb->rcv_wnd;
    tcb->irs = th->seq; // Q: what is irs
    tcb->snd_una = tcb->iss;
    tcb->snd_nxt = tcb->iss;
    tcb->snd_wnd = th->win;
    tcb->snd_wl1 = 0;
    tcb->snd_wl2 = 0;

    sk->sk->saddr = saddr;
    sk->sk->sport = sport;
    sk->sk->dport = th->sport;
    sk->sk->daddr = daddr;

    return tsk;
}

static int tcp_listen(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th, uint32_t saddr)
{
    int ret = 0;

    if (th->syn) {      
        // create new socket from now
        struct tcp_sock *fork_tsk = fork_socket(tsk->sk.sock->pid, skb, th, tsk->sk.saddr, tsk->sk.sport, saddr);
        
        fork_tsk->ptsk = tsk;

        // send syc-ack
        ret = tcp_send_synack(&fork_tsk->sk);

        fork_tsk->tcb.snd_nxt++;
    }

    free_skb(skb);
    return ret;
}

static int tcp_synsent(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    struct tcb *tcb = &tsk->tcb;
    struct sock *sk = &tsk->sk;

    tcpsock_dbg("state is synsent", sk);
    
    if (th->ack) {
        if (th->ack_seq <= tcb->iss || th->ack_seq > tcb->snd_nxt) {
            tcpsock_dbg("ACK is unacceptable", sk);
            
            if (th->rst) goto discard;
            goto reset_and_discard;
        }

        if (th->ack_seq < tcb->snd_una || th->ack_seq > tcb->snd_nxt) {
            tcpsock_dbg("ACK is unacceptable", sk);
            goto reset_and_discard;
        }
    }

    /* ACK is acceptable */
    
    if (th->rst) {
        tcp_reset(&tsk->sk);
        goto discard;
    }

    /* third check the security and precedence -> ignored */

    /* fourth check the SYN bit */
    if (!th->syn) {
        goto discard;
    }

    tcb->rcv_nxt = th->seq + 1;
    tcb->irs = th->seq;
    if (th->ack) {
        tcb->snd_una = th->ack_seq;
        /* Any packets in RTO queue that are acknowledged here should be removed */
        tcp_clean_rto_queue(sk, tcb->snd_una);
    }

    if (tcb->snd_una > tcb->iss) {
        tcp_set_state(sk, TCP_ESTABLISHED);
        tcb->snd_una = tcb->snd_nxt;
        tsk->backoff = 0;
        /* RFC 6298: Sender SHOULD set RTO <- 1 second */
        tsk->rto = 1000;
        tcp_send_ack(&tsk->sk);
        tcp_rearm_user_timeout(&tsk->sk);
        tcp_parse_opts(tsk, th);
        sock_connected(sk);
    } else {
        tcp_set_state(sk, TCP_SYN_RECEIVED);
        tcb->snd_una = tcb->iss;
        tcp_send_synack(&tsk->sk);
    }
    
discard:
    tcp_drop(sk, skb);
    return 0;
reset_and_discard:
    //TODO reset
    tcp_drop(sk, skb);
    return 0;
}

static int tcp_closed(struct tcp_sock *tsk, struct sk_buff *skb, struct tcphdr *th)
{
    /*
      All data in the incoming segment is discarded.  An incoming
      segment containing a RST is discarded.  An incoming segment not
      containing a RST causes a RST to be sent in response.  The
      acknowledgment and sequence field values are selected to make the
      reset sequence acceptable to the TCP that sent the offending
      segment.

      If the ACK bit is off, sequence number zero is used,

        <SEQ=0><ACK=SEG.SEQ+SEG.LEN><CTL=RST,ACK>

      If the ACK bit is on,

        <SEQ=SEG.ACK><CTL=RST>

      Return.
    */

    int rc = -1;

    tcpsock_dbg("state is closed", (&tsk->sk));

    if (th->rst) {
        tcp_discard(tsk, skb, th);
        rc = 0;
        goto out;
    }

    if (th->ack) {
 
    } else {
        
    
    }
    
    rc = tcp_send_reset(tsk);
    free_skb(skb);

out:
    return rc;
}

static int add_tsk_to_parent_establied_conn_list(struct tcp_sock *tsk)
{
    struct conn_info* conn_info = calloc_conn();
    conn_info->sk = tsk->sk.sock;
    conn_queue_tail(&tsk->ptsk->tcp_passive_conn_queue.establied_conn_queue, conn_info);
    wait_wakeup(&tsk->ptsk->tcp_passive_conn_queue.recv_wait);

    return 0;
}

static void update_snd_win(struct tcp_sock *tsk, uint16_t win)
{
    tsk->tcb.snd_wnd = win << tsk->snd_scale;
}

/*
 * Follows RFC793 "Segment Arrives" section closely
 */ 
int tcp_input_state(struct sock *sk, struct tcphdr *th, struct sk_buff *skb, uint32_t saddr)
{
    struct tcp_sock *tsk = tcp_sk(sk);
    struct tcb *tcb = &tsk->tcb;

    tcpsock_dbg("input state", sk);

    switch (sk->state) {
    case TCP_CLOSE:
        return tcp_closed(tsk, skb, th);
    case TCP_LISTEN:
        return tcp_listen(tsk, skb, th, saddr);
    case TCP_SYN_SENT:
        return tcp_synsent(tsk, skb, th);
    }

    /* "Otherwise" section in RFC793 */

    /* first check sequence number */
    if (!tcp_verify_segment(tsk, th, skb)) {
        /* RFC793: If an incoming segment is not acceptable, an acknowledgment
         * should be sent in reply (unless the RST bit is set, if so drop
         *  the segment and return): */
        if (!th->rst) {
           tcp_send_ack(sk); 
        }
        return_tcp_drop(sk, skb);
    }
    
    /* second check the RST bit */
    if (th->rst) {
        free_skb(skb);
        tcp_enter_time_wait(sk);
        tsk->sk.ops->recv_notify(&tsk->sk);
        return 0;
    }
    
    /* third check security and precedence */
    // Not implemented

    /* fourth check the SYN bit */
    if (th->syn) {
        /* RFC 5961 Section 4.2 */
        tcp_send_challenge_ack(sk, skb);
        return_tcp_drop(sk, skb);
    }
    
    /* fifth check the ACK field */
    if (!th->ack) {
        return_tcp_drop(sk, skb);
    }

    // ACK bit is on
    switch (sk->state) {
    case TCP_SYN_RECEIVED:
        if (tcb->snd_una < th->ack_seq && th->ack_seq <= tcb->snd_nxt) {
            tcp_set_state(sk, TCP_ESTABLISHED);
            add_tsk_to_parent_establied_conn_list(tsk);
        } else {
            return_tcp_drop(sk, skb);
        }
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
        /* RFC 5681 §3.2：dupACK 严格判断后走 fast retransmit / fast recovery 分支，
         * 处理完直接返回，不再沿用通用 ACK 路径（避免重复 send_next 与 RTO rearm）。 */
        if (tcp_is_dupack(tsk, th, skb)) {
            tcp_handle_dupack(sk);
            tsk->last_ack_win = th->win;
            free_skb(skb);
            return 0;
        }

        if (tcb->snd_una < th->ack_seq && th->ack_seq <= tcb->snd_nxt) {
            tcb->snd_una = th->ack_seq;
            /* Any segments on the retransmission queue which are thereby
               entirely acknowledged are removed. */
            tcp_rtt(tsk);
            tcp_clean_rto_queue(sk, tcb->snd_una);
            /* RFC 5681 §3.2 step 6：第一个真正推进 SND.UNA 的新 ACK 触发 deflate */
            if (tsk->in_recovery) {
                tcp_exit_fast_recovery(tsk);
            }
        }

        if (th->ack_seq < tcb->snd_una) {
            // If the ACK is a duplicate, it can be ignored
            return_tcp_drop(sk, skb);
        }

        if (th->ack_seq > tcb->snd_nxt) {
            // If the ACK acks something not yet sent, then send an ACK, drop segment
            // and return
            // TODO: Dropping the seg here, why would I respond with an ACK? Linux
            // does not respond either
            //tcp_send_ack(&tsk->sk);
            return_tcp_drop(sk, skb);
        }

        if (tcb->snd_una < th->ack_seq && th->ack_seq <= tcb->snd_nxt) {
            // TODO: should deal with zero window
            update_snd_win(tsk, th->win);
        }

        tsk->last_ack_win = th->win;
        break;
    }

    /* If the write queue is empty, it means our FIN was acked */
    if (skb_queue_empty(&sk->write_queue)) {
        switch (sk->state) {
        case TCP_FIN_WAIT_1:
            tcp_set_state(sk, TCP_FIN_WAIT_2);
        case TCP_FIN_WAIT_2:
            break;
        case TCP_CLOSING:
            /* In addition to the processing for the ESTABLISHED state, if
             * the ACK acknowledges our FIN then enter the TIME-WAIT state,
               otherwise ignore the segment. */
            tcp_set_state(sk, TCP_TIME_WAIT);
            break;
        case TCP_LAST_ACK:
            /* The only thing that can arrive in this state is an acknowledgment of our FIN.  
             * If our FIN is now acknowledged, delete the TCB, enter the CLOSED state, and return. */
            free_skb(skb);
            return tcp_done(sk);
        case TCP_TIME_WAIT:
            /* TODO: The only thing that can arrive in this state is a
               retransmission of the remote FIN.  Acknowledge it, and restart
               the 2 MSL timeout. */
            if (tcb->rcv_nxt == th->seq) {
                tcpsock_dbg("Remote FIN retransmitted?", sk);
//                tcb->rcv_nxt += 1;
                tsk->flags |= TCP_FIN;
                tcp_send_ack(sk);
            }
            break;
        }
    }
    
    /* sixth, check the URG bit */
    if (th->urg) {

    }

    int expected = skb->seq == tcb->rcv_nxt;

    /* seventh, process the segment txt */
    switch (sk->state) {
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if (th->psh || skb->dlen > 0) {
            tcp_data_queue(tsk, th, skb);
        }
                
        break;
    case TCP_CLOSE_WAIT:
    case TCP_CLOSING:
    case TCP_LAST_ACK:
    case TCP_TIME_WAIT:
        /* This should not occur, since a FIN has been received from the
           remote side.  Ignore the segment text. */
        break;
    }

    /* eighth, check the FIN bit */
    if (th->fin && expected) {
        tcpsock_dbg("Received in-sequence FIN", sk);

        switch (sk->state) {
        case TCP_CLOSE:
        case TCP_LISTEN:
        case TCP_SYN_SENT:
            // Do not process, since SEG.SEQ cannot be validated
            goto drop_and_unlock;
        }

        tcb->rcv_nxt += 1;
        tsk->flags |= TCP_FIN;
        sk->poll_events |= (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND);
        
        tcp_send_ack(sk);
        tsk->sk.ops->recv_notify(&tsk->sk);

        switch (sk->state) {
        case TCP_SYN_RECEIVED:
        case TCP_ESTABLISHED:
            tcp_set_state(sk, TCP_CLOSE_WAIT);
            break;
        case TCP_FIN_WAIT_1:
            /* If our FIN has been ACKed (perhaps in this segment), then
               enter TIME-WAIT, start the time-wait timer, turn off the other
               timers; otherwise enter the CLOSING state. */
            if (skb_queue_empty(&sk->write_queue)) {
                tcp_enter_time_wait(sk);
            } else {
                tcp_set_state(sk, TCP_CLOSING);
            }

            break;
        case TCP_FIN_WAIT_2:
            /* Enter the TIME-WAIT state.  Start the time-wait timer, turn
               off the other timers. */
            tcp_enter_time_wait(sk);
            break;
        case TCP_CLOSE_WAIT:
        case TCP_CLOSING:
        case TCP_LAST_ACK:
            /* Remain in the state */
            break;
        case TCP_TIME_WAIT:
            /* TODO: Remain in the TIME-WAIT state.  Restart the 2 MSL time-wait
               timeout. */
            break;
        }
    }

    /* Congestion control and delacks */
    switch (sk->state) {
    case TCP_ESTABLISHED:
    case TCP_FIN_WAIT_1:
    case TCP_FIN_WAIT_2:
        if (expected) {
            tcp_stop_delack_timer(tsk);

            int pending = skb_queue_len(&sk->write_queue);
            /* RFC1122:  A TCP SHOULD implement a delayed ACK, but an ACK should not
             * be excessively delayed; in particular, the delay MUST be less than
             * 0.5 seconds, and in a stream of full-sized segments there SHOULD
             * be an ACK for at least every second segment. */
            if (pending > 0) {
                /* cwnd/rwnd 已在 tcp_send_next 内部判断，发多少由它决定 */
                tcp_send_next(sk, pending, 0);
                tcp_rearm_rto_timer(tsk);
            } else if (th->psh || (skb->dlen > 1000 && ++tsk->delacks > 1)) {
                tsk->delacks = 0;
                tcp_send_ack(sk);
            } else if (skb->dlen > 0) {
                tsk->delack = timer_add(200, &tcp_send_delack, &tsk->sk);
            }
        }
    }

    free_skb(skb);

unlock:
    return 0;
drop_and_unlock:
    tcp_drop(sk, skb);
    goto unlock;
}

int tcp_receive(struct tcp_sock *tsk, void *buf, int len)
{
    int rlen = 0;
    int curlen = 0;
    struct sock *sk = &tsk->sk;
    struct socket *sock = sk->sock;

    memset(buf, 0, len);

    while (rlen < len) {
        curlen = tcp_data_dequeue(tsk, buf + rlen, len - rlen);

        rlen += curlen;

        if (tsk->flags & TCP_PSH) {

            tsk->flags &= ~TCP_PSH;
            break;
        }

        if (tsk->flags & TCP_FIN || rlen == len) break;

        if (sock->flags & O_NONBLOCK) {
            if (rlen == 0) {
                rlen = -EAGAIN;
            } 
            
            break;
        } else {
            pthread_mutex_lock(&tsk->sk.recv_wait.lock);
            socket_release(sock);
            wait_sleep(&tsk->sk.recv_wait);
            pthread_mutex_unlock(&tsk->sk.recv_wait.lock);
            socket_wr_acquire(sock);
        }
    }

    if (rlen >= 0) tcp_rearm_user_timeout(sk);
    
    return rlen;
}
