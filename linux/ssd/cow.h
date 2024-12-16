#ifndef _COW_H
#define _COW_H
#include <linux/rbtree.h>
#include <linux/spinlock.h>

// configuration
#define PG_CACHE_METHOD_OLD  0
#define PG_CACHE_METHOD_NEW  1
#define PG_CACHE_METHOD      PG_CACHE_METHOD_OLD      // 0 : block only 1 : block + byte

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


// per file struct  a rb tree and a page count
typedef struct bytefs_file_mapping {
    struct rb_root root; // a rb tree's root of current file's page cache.
    int64_t page_count; // number of pages in this file's page cache.
    //spinlock for file
    spinlock_t lock;

} fmapping_t;

// this is to use private data field of file struct.
// After verification, this private data is available for host of the file to use (meaning the one who wrote file system).
typedef struct bytefs_filp_private {
    fmapping_t* fmap; // COW related data about file
    // reserved, can add arbitrary fields.
} filp_private_t;

// wrapper to rb_node, associated with a page in our page cache
typedef struct bytefs_rb_node_head {
    uint64_t lpa;             // logical page address, key
    void *page;               // a possibly dirty page (up-to-date)
    void *dup_page;           // a duplicate CLEAN page
    uint64_t flags;             // page flags
    struct rb_node rb_node; // associated rb tree node
} rbh_t;



static inline void bytefs_wr_private(filp_private_t* fp, fmapping_t* fmap) {
    fp->fmap = fmap;
}


int64_t bytefs_pgcache_rd(struct file* filp, uint64_t st, uint64_t ed, char __user *buf);

int64_t bytefs_pgcache_wr(struct file* filp, uint64_t st, uint64_t ed, char __user *buf);

int64_t bytefs_page_cache_flush(struct file* filp, uint64_t st, uint64_t ed);


#endif

