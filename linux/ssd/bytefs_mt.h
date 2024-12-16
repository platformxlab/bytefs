#ifndef __BYTEFS_MT_H__
#define __BYTEFS_MT_H__

#include "ftl.h"

#define MTE_EXIST   (1 << 0)

struct indirection_mte {
    uint64_t lpa;
    uint64_t log_offset;
    uint32_t psl;
};

struct coalescing_mte {
    uint64_t lpn;
    uint64_t bitmap;
    uint32_t psl;
};

void imt_insert(struct ssd *ssd, uint64_t lpa, uint64_t log_offset);
struct indirection_mte *imt_get(struct ssd *ssd, uint64_t lpa);
int imt_remove(struct ssd *ssd, uint64_t lpa);

void cmt_insert(struct ssd *ssd, uint64_t lpa);
struct coalescing_mte *cmt_get(struct ssd *ssd, uint64_t lpa);
int cmt_remove(struct ssd *ssd, uint64_t lpa);

#endif
