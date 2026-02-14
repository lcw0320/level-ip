#include "syshead.h"
#include "udp.h"
#include "udp_data.h"
#include "skbuff.h"
#include "sock.h"

int udp_receive(struct udp_sock *usk, void *buf, int len, int flags, struct sockaddr *addr, socklen_t *addr_len)
{
    struct socket *sock = usk->sk.sock;
    int rlen = 0;

    for (;;) {
        if ((rlen = udp_data_dequeue(usk, buf, len, addr, addr_len)) > 0) {
            break;
        }
        
        /* wait packet from net stack */
        pthread_mutex_lock(&usk->sk.recv_wait.lock);
        socket_release(sock);
        wait_sleep(&usk->sk.recv_wait);
        pthread_mutex_unlock(&usk->sk.recv_wait.lock);
        socket_wr_acquire(sock);
    }

    return rlen;
}