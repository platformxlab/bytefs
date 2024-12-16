#ifndef __BYTEFS_MT_H__
#define __BYTEFS_MT_H__



#define MTE_EXIST   (1 << 0)

#include "ftl.h"

#ifndef __ASSOCIATIVE_CACHE_MAP_
// #define __ASSOCIATIVE_CACHE_MAP_
#endif


#ifndef __ASSOCIATIVE_CACHE_MAP_
struct buffer_mte {
    uint64_t lpn;
    uint32_t buf_idx;
    uint32_t psl;
};

void bmt_insert(struct ssd *ssd, uint64_t lpn, uint32_t buf_idx);
struct buffer_mte *bmt_get(struct ssd *ssd, uint64_t lpn);
int bmt_remove(struct ssd *ssd, uint64_t lpn);

#endif

#endif
