#ifndef _UDP_DATA_H
#define _UDP_DATA_H

#include "udp.h"

int udp_data_dequeue(struct udp_sock *tsk, void *user_buf, int len, struct sockaddr *addr, socklen_t *addr_len);
int udp_data_queue(struct udp_sock *tsk, struct udphdr *th, struct sk_buff *skb);
int udp_data_close(struct udp_sock *tsk, struct udphdr *th, struct sk_buff *skb);
#endif
