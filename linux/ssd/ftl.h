#ifndef __BYTEFS_FTL_H__
#define __BYTEFS_FTL_H__

// #include "ssd_nvme.h"
// #include <linux/printk.h>
#include <linux/printk.h>
#include <stdbool.h>
// #include <string.h>
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/gfp.h>

// #include <time.h>
// #include <pthread.h>
// #include <thread.h>


#include <linux/kthread.h>  // for threads
#include <linux/sched.h>
#include <linux/time.h>   // for using jiffies 
#include <linux/timer.h>
#include <linux/timekeeping.h>
#include <linux/delay.h>
// #include <linux/bool.h>

#include <linux/kfifo.h>
#include <linux/circ_buf.h>
#include <linux/blk_types.h> // for bio struct 
#include <linux/bio.h>
#include <linux/spinlock.h> // include locking for bio private debugging 


#include <linux/hrtimer.h>

#include "timing_model.h"
#include "simple_nvme.h"
#include "bytefs_heap.h"
#include "ftl_mapping.h"
#include "bytefs_gc.h"
#include "ring.h"

#include "buffer.h"
 
// #include <linux/kthread.h>  // for threads
// #include <linux/time.h>   // for using jiffies
// #include <linux/timer.h>

// #include "backend.c"

// // define of ssd parameters
#define CH_COUNT            (16)
#define WAY_COUNT           (1)


// #define PL_COUNT 1
#define BLOCK_COUNT         (8 * 4 * 128)
#define PG_COUNT            (128)
#define PG_SIZE             (4 * 1024)
#define PG_MASK             (PG_SIZE - 1)


//change to 40 GB
#define CACHE_SIZE          (0x80000000UL)
#define TOTAL_SIZE          (1UL * CH_COUNT * WAY_COUNT * BLOCK_COUNT * PG_COUNT * PG_SIZE)
#define ALL_TOTAL_SIZE      (CACHE_SIZE + TOTAL_SIZE)

#define NUM_POLLERS         (1)
#define MAX_REQ             (65536)
#define MAX_EVENT_HEAP      (1024)

#define SEC_SIZE              512               // size of a sector
#define NUM_SEC_PER_PAGE      (PG_SIZE / SEC_SIZE) // number of sectors per page

#ifndef __ASSOCIATIVE_CACHE_MAP_
// #define __ASSOCIATIVE_CACHE_MAP_
#endif

#ifdef __ASSOCIATIVE_CACHE_MAP_

#define CACHE_WAY_NUM                   (4)
#define CACHE_SET_NUM                   (1024)
#define TOBSSD_BUFFER_COUNT             (CACHE_SET_NUM * CACHE_WAY_NUM) 
#define TOBSSD_BUFFER_SIZE              (TOBSSD_BUFFER_COUNT * PG_SIZE) // 32 pages
#define TOBSSD_BUFFER_ENTRY_NUM         (TOBSSD_BUFFER_SIZE / PG_SIZE) // 32 entries

struct cache_tagstore {
    uint32_t lpn[CACHE_WAY_NUM]; //served as tag
    uint8_t flag; // flag for lru
};

#define CACHE_TAGSTORE_SIZE (sizeof(struct cache_tagstore)*CACHE_SET_NUM)

#else

// defines about buffer
// #define TOBSSD_BUFFER_SIZE              (256 * 1024 * 1024) // 256MB
// #define TOBSSD_BUFFER_ENTRY_NUM         (TOBSSD_BUFFER_SIZE / PG_SIZE) // 128k entries
#define TOBSSD_BUFFER_COUNT             (65536UL) // 256M
#define TOBSSD_BUFFER_SIZE              (TOBSSD_BUFFER_COUNT * PG_SIZE) // 32 pages
#define TOBSSD_BUFFER_ENTRY_NUM         (TOBSSD_BUFFER_SIZE / PG_SIZE) // 32 entries
#define BUFFER_MANAGER_ENTRY_SIZE       (8)  // 8B for each mapping entry DRAM entry -> Logical entry at page level
#define BUFFER_MANAGER_ENTRIES_NUM      (TOBSSD_BUFFER_ENTRY_NUM / BUFFER_MANAGER_ENTRY_SIZE) // mapping table size
#define TOBSSD_BMT_LOAD_FACTOR_INV      (10)
#define TOBSSD_BMT_SIZE                 (TOBSSD_BUFFER_ENTRY_NUM * TOBSSD_BMT_LOAD_FACTOR_INV)

#endif

#define BYTEFS_LOG_REGION_GRANDULARITY  (64)

#define INVALID_INDEX_32                (0xFFFFFFFFU)

// sanity check
#if PG_SIZE % 1024 != 0
#error "Page size is not 1k aligned"
#endif
#if TOTAL_SIZE % 1024 != 0
#error "Page size is not 1k aligned"
#endif

/* CPUs binded to ftl threads */
#define SSD_THREAD_CPU  2
#define SSD_POLLING_CPU 3

/* Filesystem pa region */
#define BYTEFS_PA_START (32ULL<<30)
#define BYTEFS_PA_END   (64ULL<<30)

/* byte issue related fields*/
// to stop enabling the following features, make these feature zero.
#define BYTE_ISSUE_64_ALIGN              0
#define BYTE_BLOCK_MIX					 0

#define LOG_SIZE                         64

/* DRAM backend structure */
struct SsdDramBackend {
    void    *phy_loc;  /* Emulated physical location */
    void    *virt_loc; /* Virtual address (in host's DRAM) used to emulate DRAM in 2B-SSD */
    unsigned long size; /* in bytes */
};



/* nand request type */
#define NAND_READ   0
#define NAND_WRITE  1
#define NAND_ERASE  2


/* io type */
#define USER_IO     0
#define GC_IO       1
#define INTERNAL_TRANSFER 2

/* page status */
#define PG_FREE     0
#define PG_INVALID  1
#define PG_VALID    2


/* Page mapping defines */
// @TODO check it inited using this value
#define UNMAPPED_PPA    (0xFFFFFFFFFFFFFFFFUL)
#define INVALID_LPN     (0xFFFFFFFFFFFFFFFFUL)


/* page index combination */
#define BLK_BITS    (16)
#define PG_BITS     (16)
// #define SEC_BITS    (8)
// #define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr -> real_ppa */
struct ppa {
    // union {
    struct {
        uint64_t blk : BLK_BITS;
        uint64_t pg  : PG_BITS;
        // uint64_t sec : SEC_BITS;
        // uint64_t pl  : PL_BITS;
        uint64_t lun : LUN_BITS;
        uint64_t ch  : CH_BITS;
        uint64_t rsv : 1;
    } g;
    uint64_t realppa;
    // };
};


/**
 * struct page - page structure
 * @pg_num: physical page number of the page -> real_ppa
 * @status: status of the page
 */
struct nand_page {
    // struct ppa;
    int pg_num;
    int status;
    uint32_t csum;
};

/**
 * struct block - block structure
 * @pg: pages in this block )
 * @npgs: physical block number of the block
 * @vpc: valid page count of the block
 * @ipc: invalid page count of the block
 * @erase_cnt: times of the block being erased
 * @wp : write pointer -> speicificly which page are we going to write to
 */
struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
    
    // GC specific
    int ch_idx;
    int way_idx;
    int blk_idx;
    int is_candidate;
    struct nand_block *next_blk;
};

/* plane is one so we just not include it at this version */
// struct nand_plane {
//     struct nand_block *blk;
//     int nblks;
// };

/**
 * struct lun - lun structure
 * @blk: blocks in this lun
 * @nblks: block count of the lun
 * @next_lun_avail_time: time for all request in this lun finishes
 * @busy: is lun working now? (not really used)
 */
struct nand_lun {
    struct nand_block *blk;
    int nblks;
    uint64_t next_lun_avail_time;
    bool busy;
    // uint64_t gc_endtime;
};


/**
 * struct channel - channel structure
 * @lun: luns in this channel
 * @nluns: lun count of the channel
 * @next_ch_avail_time: time for all request in this channel finishes
 * @busy: is channel working now? (not really used)
 */
struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    // uint64_t gc_endtime;
};

struct ssdparams {

    int pgsz;          /* page size in bytes */

    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_lun;  /* # of blocks per plane */
    // int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    // double gc_thres_pcent;
    // double gc_thres_pcent_high;
    // bool enable_gc_delay;

    /* below are all calculated values */
    // int pgs_per_lun;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */


    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */

    int num_poller;
};


/* wp: record next write page */
struct write_pointer {
    // struct line *curline;
    int ch;
    int next_ch;
    int lun;
    int blk;
    struct nand_block *blk_ptr;
    int pg;
};


struct nand_cmd {
    int type;
    int cmd;
    uint64_t stime; /* Coperd: request arrival time */
};


typedef struct buffer_mapping_entry {
    uint32_t buffer_index;
    uint64_t lpn;
    bool dirty;
    struct buffer_mapping_entry* pre;
    struct buffer_mapping_entry* next;
} bmap_entry;

typedef struct buffer_mapping_table {
    bmap_entry entries[TOBSSD_BUFFER_ENTRY_NUM];
} tobssd_buffer_map;

struct buffer_free_list {
    bmap_entry* head;
    bmap_entry* tail;
};

struct buffer_pin_list {
    bmap_entry* head;
    bmap_entry* tail;
};

// struct log_info {
//     uint64_t actual_lpa;     /*  starting "SSD address"  */
//     void *data_start;        /* buf */
//     unsigned long data_size; /* write data size */
// };

// #define BYTEFS_LOG_EXIST    (1 << 0)

// struct bytefs_log_entry {
//     uint64_t lpa;
//     uint8_t flag;
//     uint8_t content[LOG_DSIZE_GRANULARITY];
// };


struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    // struct line_mgmt lm;

    /* lockless ring for communication with NVMe IO thread */
    struct Ring **to_ftl;
    struct Ring **to_poller;
    // bool *dataplane_started_ptr;

    //backend
    struct SsdDramBackend* bd;

    //thread
    struct ftl_thread_info *thread_args;
    struct task_struct *thread_id;
    struct task_struct *polling_thread_id;

    // buffer mapping table
    struct buffer_mte *buffer_mt;
    int buffer_mt_size;

    // 2bssd buffer region and mapping table
    void *tobssd_buffer_ptr;
#ifdef __ASSOCIATIVE_CACHE_MAP_
    struct cache_tagstore *tobssd_tagstore_ptr;
#else
    tobssd_buffer_map *tobssd_buffer_table_ptr;
    struct buffer_free_list tobssd_buffer_free_list;
    struct buffer_pin_list tobssd_buffer_pin_list;
#endif
    // GC
    int total_free_blks;
    struct gc_free_list *free_blks;
    struct gc_candidate_list *gc_candidate_blks;
    struct bytefs_heap *gc_heaps;
    void *gc_buffer;
    int free_blk_low_threshold;
    int free_blk_high_threshold;
};




/** Backend.c */

extern struct SsdDramBackend *dram_backend;

int init_dram_space(void*phy_loc, void* virt_loc, unsigned int nbytes);

/* Memory Backend (mbe) for emulated SSD */

int init_dram_backend(struct SsdDramBackend **mbe, size_t nbytes, phys_addr_t phy_loc);


void free_dram_backend(struct SsdDramBackend *b);

// read or write to dram location
int backend_rw(struct SsdDramBackend *b, unsigned long ppa, void* data, bool is_write);

// read or write to dram location
int cache_rw(struct SsdDramBackend *b, unsigned long off, void* data, bool is_write, unsigned long size);

void *cache_mapped(struct SsdDramBackend *b, unsigned long off);


/** ftl.c */
extern struct ssd gdev;
extern uint64_t start, cur;

extern int ssd_init(void);
extern int ssd_reset(void);
/* SSD API not using BIO */
extern int nvme_issue(int is_write, uint64_t lba, uint64_t len, void *buf);
extern int byte_issue(int is_write, uint64_t lpa, uint64_t size, void *buf);

/* SSD API related to bio : details see function headers in ftl.c */
extern int nvme_issue_wait(int is_write, uint64_t lba, uint64_t len, void *buf, struct bio* bio);
extern int nvme_issue_nowait(int is_write, uint64_t lba, uint64_t len, void *buf, struct bio* bio, unsigned long* if_end_bio);
extern int nvme_issue_sector_wait(int is_write, int64_t bi_sec, int64_t num_sec, void* buf, struct bio* bio);
extern int nvme_issue_delete( int64_t bi_sec, int64_t num_sec, struct bio* bio);


void ppa2pgidx(struct ssd *ssd, struct ppa *ppa);
void pgidx2ppa(struct ssd *ssd, struct ppa *ppa);

inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn);
inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa);
inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa);
inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa);

struct ppa get_new_page(struct ssd *ssd);
void mark_page_invalid(struct ssd *ssd, struct ppa *ppa);
void mark_page_valid(struct ssd *ssd, struct ppa *ppa);
void mark_block_free(struct ssd *ssd, struct ppa *ppa);

void ssd_advance_write_pointer(struct ssd *ssd);
uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd);

/**
 * Check lba within disk.
*/
static inline bool lba_is_legal(uint64_t lba) {
    return (lba * PG_SIZE < TOTAL_SIZE);
}

static inline uint64_t timespec2uint(struct timespec64 *t) {
    return (uint64_t)t->tv_sec * 1000000000UL + t->tv_nsec;
}

static inline uint64_t get_time_ns(void) {
    struct timespec64 ts;
    ktime_get_ts64(&ts);
    return timespec2uint(&ts);
}

// /* BYTEFS assert() */
// #ifdef BYTEFS_DEBUG_FTL
// #define bytefs_assert(expression) assert(expression)
// #else
// #define bytefs_assert(expression)
// #endif

int ftl_thread(void *arg);
int request_poller_thread(void *arg);

#endif
