#ifndef __FTL_CACHE_H__
#define __FTL_CACHE_H__

#include "ftl.h"
#include "bytefs_heap.h"
#include "bytefs_mt.h"

#ifndef __ASSOCIATIVE_CACHE_MAP_
// #define __ASSOCIATIVE_CACHE_MAP_
#endif

#ifdef __ASSOCIATIVE_CACHE_MAP_

// DRAM LAYOUT
#define DRAM_START                  (0UL)

#define TOBSSD_BUFFER_START         (DRAM_START)
#define TOBSSD_BUFFER_END           (TOBSSD_BUFFER_START + TOBSSD_BUFFER_SIZE)


#define TOBSSD_CACHE_STORE          (TOBSSD_BUFFER_END)
#define TOBSSD_CACHE_STORE_END      (TOBSSD_CACHE_STORE + CACHE_TAGSTORE_SIZE)

#define DRAM_END                    (TOBSSD_CACHE_STORE_END)
#define DRAM_LAST_ADDR              (DRAM_END - 1)


#else

// DRAM LAYOUT
#define DRAM_START                  (0UL)

#define TOBSSD_BUFFER_START         (DRAM_START)
#define TOBSSD_BUFFER_END           (TOBSSD_BUFFER_START + TOBSSD_BUFFER_SIZE)

#define TOBSSD_BUFFER_MT_START      (TOBSSD_BUFFER_END)
#define TOBSSD_BUFFER_MT_END        (TOBSSD_BUFFER_MT_START + TOBSSD_BMT_SIZE * sizeof(struct buffer_mte))

#define TOBSSD_BUFFER_TABLE_START   (TOBSSD_BUFFER_MT_END)
#define TOBSSD_BUFFER_TABLE_END     (TOBSSD_BUFFER_TABLE_START + sizeof(tobssd_buffer_map))

#define DRAM_END                    (TOBSSD_BUFFER_TABLE_END)
#define DRAM_LAST_ADDR              (DRAM_END - 1)

#endif


#endif
