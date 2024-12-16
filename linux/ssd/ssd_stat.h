#ifndef __SSD_STAT__
#define __SSD_STAT__

#include "ftl.h"
#include <linux/types.h>
#include <linux/atomic.h>


struct ssd_stat {
    // total issue counter
    atomic_long_t block_rissue_count;
    atomic_long_t block_wissue_count;
    atomic_long_t byte_rissue_count;
    atomic_long_t byte_wissue_count;
    // total traffic
    atomic_long_t block_rissue_traffic;
    atomic_long_t block_wissue_traffic;
    atomic_long_t byte_rissue_traffic;
    atomic_long_t byte_wissue_traffic;
    // meta data
    atomic_long_t inode_traffic;
    atomic_long_t superblock_traffic;
    atomic_long_t bitmap_traffic;
    atomic_long_t journal_traffic;
    atomic_long_t dp_traffic;
    // traffic
    atomic_long_t block_metadata_issue_traffic_r;
    atomic_long_t block_metadata_issue_traffic_w;
    atomic_long_t block_data_traffic_r;
    atomic_long_t block_data_traffic_w;
    atomic_long_t byte_metadata_issue_traffic_r;
    atomic_long_t byte_metadata_issue_traffic_w;
    atomic_long_t byte_data_traffic_r;
    atomic_long_t byte_data_traffic_w;
    // page based stats
    atomic_long_t buffer_rd_hit;
    atomic_long_t buffer_wr_hit;
    atomic_long_t buffer_rd_miss;
    atomic_long_t buffer_wr_miss;
    atomic_long_t buffer_promote;
    atomic_long_t buffer_evict;
    // log based stats
    atomic_long_t log_wr_op;
    atomic_long_t log_rd_op;
    atomic_long_t log_rd_log_page_partial_hit;
    atomic_long_t log_rd_log_page_hit;
    atomic_long_t log_direct_rd_page;
    atomic_long_t log_coalescing_rd_page;
    atomic_long_t log_wr_page;
    atomic_long_t log_append;
    atomic_long_t log_flushes;
    atomic_long_t byte_issue_nand_wr_modified_distribution[PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY];
    atomic_long_t byte_issue_nand_rd_modified_distribution[PG_SIZE / BYTEFS_LOG_REGION_GRANDULARITY];
    // latency
    atomic_long_t total_r_lat;
    atomic_long_t total_w_lat;
    atomic_long_t prog_lat;
    // page cache
    atomic_long_t page_cache_rd_hit;
    atomic_long_t page_cache_wr_hit;
    atomic_long_t page_cache_rd_miss;
    atomic_long_t page_cache_wr_miss;
};



extern struct ssd_stat ssd_stat;

extern int check_stat_state(void);
extern int turn_on_stat(void);
extern int reset_ssd_stat(void);
extern int print_stat(void* ptr);

// function wrapper for module use
extern int ssd_stat_add(int name, unsigned long value);

#define SSD_STAT_ADD(name, value) {     \
    if (check_stat_state()) {           \
        ssd_stat.name.counter += value;     \
    }                                   \
}

#define SSD_STAT_SUB(name, value) {     \
    if (check_stat_state()) {           \
        ssd_stat.name.counter -= value;     \
    }                                   \
}

#define SSD_STAT_SET(name, value) {     \
    if (check_stat_state()) {           \
        ssd_stat.name.counter = value;      \
    }                                   \
}

#define SSD_STAT_ATOMIC_ADD(name, value) {  \
    if (check_stat_state()) {               \
        atomic_long_add(value, &ssd_stat.name); \
    }                                       \
}

#define SSD_STAT_ATOMIC_SUB(name, value) {  \
    if (check_stat_state()) {               \
        atomic_long_sub(value, &ssd_stat.name); \
    }                                       \
}

#define SSD_STAT_ATOMIC_INC(name) {         \
    if (check_stat_state()) {               \
        atomic_long_inc(&ssd_stat.name);        \
    }                                       \
}

#define SSD_STAT_ATOMIC_DEC(name) {         \
    if (check_stat_state()) {               \
        atomic_long_dec(&ssd_stat.name);        \
    }                                       \
}

#define SSD_STAT_ATOMIC_SET(name, value) {  \
    if (check_stat_state()) {               \
        atomic_long_set(value, &ssd_stat.name); \
    }                                       \
}





#endif