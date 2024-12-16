#ifndef __PMFS_BUFFER_H__
#define __PMFS_BUFFER_H__

#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include "ftl.h"

#define LRU_TRANSFER_TIMES     1

#define BH_RBH_FLAG_INIT       0


// debug level
#define BH_NO_DEBUG            0
#define BH_BASIC_DEBUG         (1<<0)
#define BH_DEBUG_TYPE          BH_BASIC_DEBUG

// define our debug printk : this does nothing when we're at particular debug level
#if (BH_DEBUG_TYPE != BH_NO_DEBUG)
#define BH_bytefs_debug(loglevel, fmt, args...) \
    printk(loglevel fmt, ##args)
#else
#define BH_bytefs_debug(loglevel, fmt, args...)
#endif
typedef struct bf_bytefs_rb_node_head {
    struct rb_node node; // associated rb tree node
    uint64_t lpa;             // logical page address, key
    void *page;               // a possibly dirty page (up-to-date)
    uint64_t flags;             // page flags
    uint32_t hit_times;
    struct list_head li; // corresponding item in active / inactive list.
} buf_rbh_t;


typedef struct buf_cache {
    struct list_head active_list;
    struct list_head inactive_list; 
    uint64_t active_size;
    uint64_t inactive_size;
    int64_t page_count; // number of pages in this file's page cache.
    struct rb_root   root; // key : lpa val : buf
    spinlock_t lock;
} bcache_t;

void test_buffer_rdwr(void);

// buffer cache interface
extern int64_t buf_rd(uint64_t st, uint64_t size, char* buf);
extern int64_t buf_wr(uint64_t st, uint64_t size, char* buf);

#endif /* __PMFS_BUFFER_H__ */