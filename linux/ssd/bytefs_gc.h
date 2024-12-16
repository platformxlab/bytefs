#ifndef __BYTEFS_GC_H__
#define __BYTEFS_GC_H__

#include "ftl.h"
#include "ftl_mapping.h"

struct ppa;

struct gc_free_list {
  int num_blocks;
  struct nand_block *blocks_start;
  struct nand_block *blocks_end;
};

struct gc_candidate_list {
  int num_blocks;
  struct nand_block *blocks_start;
  struct nand_block *blocks_end;
};

int bytefs_gc_init(struct ssd *ssd);
void bytefs_gc_reset(struct ssd *ssd);
void bytefs_gc_free(struct ssd *ssd);

void bytefs_try_add_gc_candidate_ppa(struct ssd *ssd, struct ppa *ppa);
void bytefs_try_add_gc_candidate_blk(struct ssd *ssd, struct nand_block *blk);

struct nand_block *bytefs_get_next_free_blk(struct ssd *ssd, int *start_idx);
inline int bytefs_should_start_gc(struct ssd *ssd);
inline int bytefs_should_stop_gc(struct ssd *ssd);
void bytefs_gc(struct ssd *ssd);

#endif
