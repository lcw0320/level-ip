#include "syshead.h"
#include "skbuff.h"
#include "list.h"

struct sk_buff *alloc_skb(unsigned int size)
{
    struct sk_buff *skb = malloc(sizeof(struct sk_buff));

    memset(skb, 0, sizeof(struct sk_buff));
    skb->data = malloc(size);
    memset(skb->data, 0, size);

    skb->refcnt = 0;
    skb->head = skb->data;
    skb->end = skb->data + size;

    list_init(&skb->list);

    return skb;
}

void free_skb(struct sk_buff *skb)
{
    if (skb->refcnt < 1) {
        free(skb->head);
        free(skb);
    }
}

/* 拷贝出独立的 skb：链表节点不能同时属于两条队列 */
struct sk_buff *skb_copy(struct sk_buff *skb)
{
    unsigned int size = skb->end - skb->head;
    struct sk_buff *nskb = alloc_skb(size);

    memcpy(nskb->head, skb->head, size);
    nskb->data = nskb->head + (skb->data - skb->head);

    if (skb->payload != NULL) {
        nskb->payload = nskb->head + (skb->payload - skb->head);
    }

    nskb->protocol = skb->protocol;
    nskb->len = skb->len;
    nskb->dlen = skb->dlen;
    nskb->seq = skb->seq;
    nskb->end_seq = skb->end_seq;
    nskb->dev = skb->dev;
    nskb->rt = skb->rt;
    nskb->sacked = skb->sacked;
    nskb->tcpcsum = skb->tcpcsum;

    return nskb;
}

void *skb_reserve(struct sk_buff *skb, unsigned int len)
{
    skb->data += len;

    return skb->data;
}

uint8_t *skb_push(struct sk_buff *skb, unsigned int len)
{
    skb->data -= len;
    skb->len += len;

    return skb->data;
}

uint8_t *skb_head(struct sk_buff *skb)
{
    return skb->head;
}

void skb_reset_header(struct sk_buff *skb)
{
    skb->data = skb->end - skb->dlen;
    skb->len = skb->dlen;
}
