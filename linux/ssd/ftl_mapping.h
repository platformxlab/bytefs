#ifndef __FTL_CACHE_H__
#define __FTL_CACHE_H__

#include "ftl.h"
#include "bytefs_heap.h"
#include "bytefs_mt.h"


// DRAM LAYOUT
#define DRAM_START                  (0UL)

#define BYTEFS_LOG_REGION_START     (DRAM_START)
#define BYTEFS_LOG_REGION_END       (BYTEFS_LOG_REGION_START + BYTEFS_LOG_REGION_SIZE)

#define BYTEFS_INDIRECTION_MT_START (BYTEFS_LOG_REGION_END)
#define BYTEFS_INDIRECTION_MT_END   (BYTEFS_INDIRECTION_MT_START + BYTEFS_MT_SIZE * sizeof(struct indirection_mte))

#define BYTEFS_COALESCING_MT_START  (BYTEFS_INDIRECTION_MT_END)
#define BYTEFS_COALESCING_MT_END    (BYTEFS_COALESCING_MT_START + BYTEFS_MT_SIZE * sizeof(struct coalescing_mte))

#define DRAM_END                    (BYTEFS_COALESCING_MT_END)
#define DRAM_LAST_ADDR              (DRAM_END - 1)

#endif
