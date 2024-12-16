/**
 * @file buffer.h
 * @brief metadata buffer
 */
#ifndef _PMFS_BUFFER_H
#define _PMFS_BUFFER_H

#define LRU_TRANSFER_TIMES 1

#define INIT_RD_WR_VARS(lpa, rbh, rbh_next, l_contig, r_contig, bufl_l, bufl_r, bufr_l, bufr_r, buf_l, buf_r) \
do { \
    node = rb_search(&buffer_cache.root, lpa); \
    nxt_node = rb_next(node); \
    rbh = (node == NULL) ? NULL : get_rbh(node); \
    rbh_next = (nxt_node == NULL) ? NULL :get_rbh(nxt_node); \
    if(rbh) {  \
        bufl_l = rbh->lpa; \
        bufl_r = rbh->lpa + rbh->size - 1; \
    } \
    if(rbh_next) { \
        bufr_l = rbh_next->lpa; \
        bufr_r = rbh_next->lpa + rbh_next->size - 1; \
    }   \
    buf_l = lpa;    \
    buf_r = lpa + size - 1; \
    l_contig = rbh ? (bufl_r >= buf_l) : 0; \
    r_contig = rbh_next ? (bufr_l <= buf_r) : 0; \
} while(0)


#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/spinlock.h>

#include "../../ssd/ftl.h"

typedef struct buf_rbh {
    struct rb_node node;
    uint64_t lpa;
    uint8_t* buf;
    uint32_t size;
    uint32_t hit_times;
    struct list_head* li; // corresponding item in active / inactive list.
} buf_rbh_t;

typedef struct buf_cache {
    struct list_head active_list;
    struct list_head inactive_list; 
    uint64_t active_size;
    uint64_t inactive_size;
    struct rb_root   root; // key : lpa val : buf
    spinlock_t lock;
} bcache_t;

void test_buffer_rdwr(void);
int64_t buf_wr(uint64_t lpa, uint64_t size, char* buf);
int64_t buf_rd(uint64_t lpa, uint64_t size, char* buf);


#endif /* _PMFS_BUFFER_H */