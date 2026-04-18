#ifndef _TCP_PASSIVE_CONN_H
#define _TCP_PASSIVE_CONN_H
#include "list.h"
#include "socket.h"

struct conn_info {
    struct list_head list;
    struct socket* sk;   
};

struct conn_head {
    struct list_head head;
    uint32_t qlen;
};

static inline struct conn_info* calloc_conn()
{
    return calloc(1, sizeof(struct conn_info));
}

static inline void free_conn(struct conn_info *conn)
{
    free(conn);
}

static inline uint32_t conn_queue_len(const struct conn_head *list)
{
    return list->qlen;
}

static inline void conn_queue_init(struct conn_head *list)
{
    list_init(&list->head);
    list->qlen = 0;
}

static inline void conn_queue_add(struct conn_head *list, struct conn_info *new, struct conn_info *next)
{
    list_add_tail(&new->list, &next->list);
    list->qlen += 1;
}

static inline void conn_queue_tail(struct conn_head *list, struct conn_info *new)
{
    list_add_tail(&new->list, &list->head);
    list->qlen += 1;
}

static inline struct conn_info *conn_dequeue(struct conn_head *list)
{
    struct conn_info *conn = list_first_entry(&list->head, struct conn_info, list);
    list_del(&conn->list);
    list->qlen -= 1;

    return conn;
}

static inline int conn_queue_empty(const struct conn_head *list)
{
    return conn_queue_len(list) < 1;
}

static inline struct conn_info *conn_peek(struct conn_head *list)
{
    if (conn_queue_empty(list)) return NULL;
        
    return list_first_entry(&list->head, struct conn_info, list);
}

static inline void conn_queue_free(struct conn_head *list)
{
    struct conn_info *conn = NULL;
    
    while ((conn = conn_peek(list)) != NULL) {
        conn_dequeue(list);
        free_conn(conn);
    }
}

#endif