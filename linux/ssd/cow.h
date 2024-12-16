#ifndef _COW_H
#define _COW_H
#include <linux/rbtree.h>
#include <linux/spinlock.h>
#include <linux/llist.h>
#include <linux/semaphore.h>

// configuration
#define PG_CACHE_METHOD_OLD  0
#define PG_CACHE_METHOD_NEW  1
#define PG_CACHE_METHOD      PG_CACHE_METHOD_NEW     // 0 : block only 1 : block + byte

// debug level
#define COW_NO_DEBUG            0
#define COW_BASIC_DEBUG         (1<<0)
#define COW_DEBUG_TYPE          COW_BASIC_DEBUG

// define our debug printk : this does nothing when we're at particular debug level
#if (COW_DEBUG_TYPE != COW_NO_DEBUG)
#define COW_bytefs_debug(loglevel, fmt, args...) \
    printk(loglevel fmt, ##args)
#else
#define COW_bytefs_debug(loglevel, fmt, args...)
#endif

// allocation functions
#define bytefs_alloc_page(alloc_flags) kmalloc(PG_SIZE, alloc_flags);
#define bytefs_alloc_dup_page(alloc_flags) kmalloc(PG_SIZE, alloc_flags);

// rbh flags
#define RBH_FLAGS_INIT       0
#define RBH_FLAGS_DIRTY      (1<<0)
#define RBH_FLAGS_INVALID    (1<<1)
#define RBH_FLAGS_CAN_FREE   (1<<2)

#define RBH_IS_DIRTY(rbh)   (rbh->flags & RBH_FLAGS_DIRTY)
#define RBH_IS_INVALID(rbh) (rbh->flags & RBH_FLAGS_INVALID)
#define RBH_CAN_FREE(rbh)   (rbh->flags & RBH_FLAGS_CAN_FREE)

#define RBH_SET_DIRTY(rbh)                          \
    do {                                            \
        rbh->flags |= RBH_FLAGS_DIRTY;              \
    } while (0)
#define RBH_SET_CLEAN(rbh)                          \
    do {                                            \
        rbh->flags &= ~RBH_FLAGS_DIRTY;             \
    } while (0)
#define RBH_SET_INVALID(rbh)                        \
    do {                                            \
        rbh->flags |= RBH_FLAGS_INVALID;            \
    } while (0)
#define RBH_SET_VALID(rbh)                          \
    do {                                            \
        rbh->flags &= ~RBH_FLAGS_INVALID;           \
    } while (0)
#define RBH_SET_CAN_FREE(rbh)                       \
    do {                                            \
        rbh->flags |= RBH_FLAGS_CAN_FREE;           \
    } while (0)

#define LRU_TRANSFER_TIMES  1

// global variables
#define ALPHA   50
#define LOC_MAX 128  // 128 is the max number

// per file struct  a rb tree and a page count
typedef struct bytefs_file_mapping {
    struct rb_root root;            // a rb tree's root of current file's page cache.
    struct rw_semaphore rbt_sem;    // rbt semaphore
    spinlock_t rbt_lock;            // rbt spinlock
    int64_t page_count;             // number of pages in this file's page cache.
    
    spinlock_t lock;                // spinlock for file  
    struct file* file;              // we need lock from it

    spinlock_t fmap_evict_lock;
    atomic64_t remaining_refs;

    // struct list_head active_list; // active list of pages  // not used
    // struct list_head inactive_list;  // inactive list of pages (LRU, hit times <= LRU_TRANSFER_TIMES)  // not used 
    // struct list_head li;  //used to link to file's list of file mapping
    uint64_t active_size; // active list size (page granularity)
    uint64_t inactive_size; // inactive list size (page granularity)
} fmapping_t;

// this is to use private data field of file struct.
// After verification, this private data is available for host of the file to use (meaning the one who wrote file system).
typedef struct bytefs_filp_private {
    fmapping_t* fmap; // COW related data about file
    // reserved, can add arbitrary fields.
} filp_private_t;

// wrapper to rb_node, associated with a page in our page cache
typedef struct bytefs_rb_node_head {
    fmapping_t* fmap;           // COW related data about file
    uint64_t lpa;               // logical page address, key
    void *page;                 // a possibly dirty page (up-to-date)
    void *dup_page;             // a duplicate CLEAN page
    uint64_t flags;             // page flags
    // links / refs
    struct rb_node rb_node;     // associated rb tree node
    struct llist_node li;       // corresponding item in active / inactive list.
    // locks
    struct rw_semaphore in_use;
    // statisitcs
    // uint64_t dirty_ratio;    // number of dirty cacheline 
    uint64_t locality;          // locality = alpha * locality + (1 - alpha) * dirty_ratio
    uint32_t hit_times;         // page cache hit times
} rbh_t;


// typedef struct bytefs_cow_gc {
//     struct list_head active_list; // active list of pages
//     struct list_head inactive_list;  // inactive list of pages (LRU, hit times <= LRU_TRANSFER_TIMES)
//     uint64_t active_size; // active list size (page granularity)
//     uint64_t inactive_size; // inactive list size (page granularity)
//     spinlock_t lock; // lock for gc
// } cow_gc_t;


static inline void bytefs_wr_private(filp_private_t* fp, fmapping_t* fmap) {
    fp->fmap = fmap;
}

int64_t bytefs_cache_init(void); 
// void cow_unit_test(void);

int64_t bytefs_open_init(struct file* filp);

int64_t bytefs_pgcache_rd(struct file* filp, uint64_t st, uint64_t ed, char __user *buf);

int64_t bytefs_pgcache_wr(struct file* filp, uint64_t st, uint64_t ed, char __user *buf);

int64_t bytefs_page_cache_flush(struct file* filp, uint64_t st, uint64_t ed);

void evict_file_cache(struct file* filp);

#endif

