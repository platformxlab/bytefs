#include <linux/slab.h>
#include <linux/printk.h>
#include "bytefs_gc.h"
#include "bytefs_heap.h"
#include "bytefs_utils.h"
#include "ftl.h"

static void bytefs_push_back_free_list(struct ssd *ssd, struct nand_block *nand_block) {
  struct gc_free_list *free_list = &ssd->free_blks[nand_block->ch_idx];
  bytefs_assert(nand_block->vpc == 0);
  bytefs_assert(nand_block->ipc == 0);
  nand_block->next_blk = NULL;
  if (!free_list->blocks_end) {
    bytefs_assert(!free_list->blocks_start);
    free_list->blocks_start = nand_block;
  } else {
    free_list->blocks_end->next_blk = nand_block;
  }
  free_list->blocks_end = nand_block;
  free_list->num_blocks++;
  ssd->total_free_blks++;
}

static inline struct nand_block *bytefs_get_front_free_list(struct ssd *ssd, int ch_idx) {
  return ssd->free_blks[ch_idx].blocks_start;
}

static void bytefs_rm_front_free_list(struct ssd *ssd, int ch_idx) {
  struct gc_free_list *free_list = &ssd->free_blks[ch_idx];
  bytefs_assert(free_list->blocks_start);
  bytefs_assert(free_list->num_blocks);
  if (free_list->blocks_start == free_list->blocks_end) {
    bytefs_assert(free_list->num_blocks == 1);
    free_list->blocks_start = NULL;
    free_list->blocks_end = NULL;
  } else {
    free_list->blocks_start = free_list->blocks_start->next_blk;
  }
  free_list->num_blocks--;
  ssd->total_free_blks--;
}

static void bytefs_push_back_candidate_list(struct ssd *ssd, struct nand_block *nand_block) {
  int ch_idx = nand_block->ch_idx;
  // int way_idx = nand_block->way_idx;
  // int blk_idx = nand_block->blk_idx;
  struct gc_candidate_list *candidate_list = &ssd->gc_candidate_blks[ch_idx];
  bytefs_expect(nand_block->vpc <= PG_COUNT);
  bytefs_assert(!nand_block->is_candidate);
  nand_block->is_candidate = 1;
  nand_block->next_blk = NULL;
  if (!candidate_list->blocks_end) {
    bytefs_assert(!candidate_list->blocks_start);
    candidate_list->blocks_start = nand_block;
  } else {
    candidate_list->blocks_end->next_blk = nand_block;
  }
  candidate_list->blocks_end = nand_block;
  candidate_list->num_blocks++;
}

static inline struct nand_block *bytefs_get_front_candidate_list(struct ssd *ssd, int ch_idx) {
  return ssd->gc_candidate_blks[ch_idx].blocks_start;
}

static void bytefs_rm_front_candidate_list(struct ssd *ssd, int ch_idx) {
  struct gc_candidate_list *candidate_list = &ssd->gc_candidate_blks[ch_idx];
  bytefs_assert(candidate_list->blocks_start);
  bytefs_assert(candidate_list->num_blocks);
  if (candidate_list->blocks_start == candidate_list->blocks_end) {
    candidate_list->blocks_start = NULL;
    candidate_list->blocks_end = NULL;
  } else {
    candidate_list->blocks_start = candidate_list->blocks_start->next_blk;
  }
  candidate_list->num_blocks--;
}

int bytefs_gc_init(struct ssd *ssd) {
  int ch_idx;
  ssd->free_blks = kzalloc(ssd->sp.nchs * sizeof(struct gc_free_list), GFP_KERNEL);
  ssd->gc_candidate_blks = kzalloc(ssd->sp.nchs * sizeof(struct gc_candidate_list), GFP_KERNEL);
  ssd->gc_heaps = kzalloc(ssd->sp.nchs * sizeof(struct bytefs_heap), GFP_KERNEL);
  ssd->gc_buffer = kzalloc(ssd->sp.pgsz, GFP_KERNEL);
  bytefs_expect(ssd->free_blks);
  bytefs_expect(ssd->gc_candidate_blks);
  bytefs_expect(ssd->gc_heaps);
  bytefs_expect(ssd->gc_buffer);
  if (!ssd->free_blks || !ssd->gc_candidate_blks || !ssd->gc_heaps || !ssd->gc_buffer) {
    bytefs_err("GC init failed");
    return -1;
  }
  for (ch_idx = 0; ch_idx < ssd->sp.nchs; ch_idx++) {
    heap_create(&ssd->gc_heaps[ch_idx], ssd->sp.blks_per_ch);
  }
  ssd->free_blk_low_threshold = ssd->sp.blks_per_ch * ssd->sp.nchs * 10 / 100;
  ssd->free_blk_high_threshold = ssd->sp.blks_per_ch * ssd->sp.nchs * 25 / 100;

  bytefs_gc_reset(ssd);
  return 0;
}

void bytefs_gc_reset(struct ssd *ssd) {
  int ch_idx, way_idx, blk_idx;
  struct nand_block *blk;
  ssd->total_free_blks = 0;
  for (ch_idx = 0; ch_idx < ssd->sp.nchs; ch_idx++) {
    // Per channel free list
    ssd->free_blks[ch_idx].blocks_start = NULL;
    ssd->free_blks[ch_idx].blocks_end = NULL;
    ssd->free_blks[ch_idx].num_blocks = 0;
    for (way_idx = 0; way_idx < WAY_COUNT; way_idx++) {
      for (blk_idx = 0; blk_idx < BLOCK_COUNT; blk_idx++) {
        blk = &ssd->ch[ch_idx].lun[way_idx].blk[blk_idx];
        blk->ch_idx = ch_idx;
        blk->way_idx = way_idx;
        blk->blk_idx = blk_idx;
        blk->is_candidate = 0;
        bytefs_push_back_free_list(ssd, blk);
      }
    }
    // Per channel GC candidate list
    ssd->gc_candidate_blks[ch_idx].blocks_start = NULL;
    ssd->gc_candidate_blks[ch_idx].blocks_end = NULL;
    ssd->gc_candidate_blks[ch_idx].num_blocks = 0;
    // Per channel GC candidate heap
    heap_clear(&ssd->gc_heaps[ch_idx]);
  }
}

void bytefs_gc_free(struct ssd *ssd) {
  // int ch_idx;
  // for (ch_idx = 0; ch_idx < ssd->sp.nchs; ch_idx++) {
  //   heap_create(&ssd->gc_heaps[ch_idx], ssd->sp.blks_per_ch);
  // }
  kfree(ssd->free_blks);
  kfree(ssd->gc_heaps);
}

static struct ppa bytefs_get_ppa_from_nand_blk(struct nand_block *blk) {
  struct ppa ppa;
  ppa.realppa = 0;
  ppa.g.ch = blk->ch_idx;
  ppa.g.lun = blk->way_idx;
  ppa.g.blk = blk->blk_idx;
  return ppa;
}

void bytefs_try_add_gc_candidate_ppa(struct ssd *ssd, struct ppa *ppa) {
  // criteria should align with the criteria in bytefs_should_stop_gc
  bytefs_try_add_gc_candidate_blk(ssd, &ssd->ch[ppa->g.ch].lun[ppa->g.lun].blk[ppa->g.blk]);
}

void bytefs_try_add_gc_candidate_blk(struct ssd *ssd, struct nand_block *blk) {
  // criteria should align with the criteria in bytefs_should_stop_gc
  if (blk->is_candidate) return;
  bytefs_push_back_candidate_list(ssd, blk);
  
  // if (blk->ipc >= blk->npgs * 80 / 100) {
  //   bytefs_push_back_candidate_list(ssd, blk);
  // }
}

static void bytefs_add_everyting_to_candidate_list(struct ssd *ssd, int ch_idx) {
  int i, j;
  struct nand_block *blk;
  for (i = 0; i < ssd->sp.luns_per_ch; i++) {
    for (j = 0; j < ssd->sp.blks_per_lun; j++) {
      blk = &ssd->ch[ch_idx].lun[i].blk[j];
      if (!blk->is_candidate && blk->vpc < ssd->sp.pgs_per_blk)
        bytefs_push_back_candidate_list(ssd, blk);
    }
  }
}

inline int bytefs_should_start_gc(struct ssd *ssd) {
  // bytefs_log("TT Free/Threshold/TT: %8d/%8d/%8d",
  //     ssd->total_free_blks,
  //     ssd->free_blk_low_threshold,
  //     ssd->sp.blks_per_ch * ssd->sp.nchs);
  return ssd->total_free_blks <= ssd->free_blk_low_threshold;
}

inline int bytefs_should_stop_gc(struct ssd *ssd) {
  return ssd->total_free_blks >= ssd->free_blk_high_threshold;
}

void bytefs_generate_gc_heaps(struct ssd *ssd) {
  int ch_idx;
  struct nand_block *blk;
  int max_heap_len = ssd->free_blk_high_threshold - ssd->free_blk_low_threshold;
  for (ch_idx = 0; ch_idx < ssd->sp.nchs; ch_idx++) {
    heap_clear(&ssd->gc_heaps[ch_idx]);
    bytefs_add_everyting_to_candidate_list(ssd, ch_idx);
    blk = bytefs_get_front_candidate_list(ssd, ch_idx);
    while (blk && ssd->gc_heaps[ch_idx].count < max_heap_len) {
      bytefs_rm_front_candidate_list(ssd, ch_idx);
      heap_insert(&ssd->gc_heaps[ch_idx], blk->vpc, blk);
      blk = bytefs_get_front_candidate_list(ssd, ch_idx);
    }
  }
}

struct nand_block *bytefs_get_next_free_blk(struct ssd *ssd, int *start_idx) {
  struct nand_block *blk;
  int ch_idx = *start_idx;
  const int end_idx = ch_idx;
  bytefs_assert(ch_idx >= 0 && ch_idx < ssd->sp.nchs);
  do {
    blk = bytefs_get_front_free_list(ssd, ch_idx);
    if (blk) {
      bytefs_rm_front_free_list(ssd, ch_idx);
      *start_idx = (ch_idx + 1) % ssd->sp.nchs;
      return blk;
    }
    ch_idx = (ch_idx + 1) % ssd->sp.nchs;
  } while (ch_idx != end_idx);
  bytefs_assert("No free block left on device");
  return NULL;
}

static struct nand_block *bytefs_gc_find_next_gc_nand_blk(struct ssd *ssd, int *start_idx) {
  struct nand_block *blk;
  int ch_idx = *start_idx;
  const int end_idx = ch_idx;
  bytefs_assert(ch_idx >= 0 && ch_idx < ssd->sp.nchs);
  do {
    if (!heap_is_empty(&ssd->gc_heaps[ch_idx])) {
      *start_idx = (ch_idx + 1) % ssd->sp.nchs;
      blk = (struct nand_block *) heap_get_min(&ssd->gc_heaps[ch_idx]);
      heap_pop_min(&ssd->gc_heaps[ch_idx]);
      return blk;
    }
    ch_idx = (ch_idx + 1) % ssd->sp.nchs;
  } while (ch_idx != end_idx);
  bytefs_err("No GC candidate available");
  return NULL;
}

static void put_nand_blk_back_to_candidates(struct ssd *ssd) {
  int ch_idx;
  struct nand_block *blk;
  for (ch_idx = 0; ch_idx < ssd->sp.nchs; ch_idx++) {
    while (!heap_is_empty(&ssd->gc_heaps[ch_idx])) {
      blk = heap_get_min(&ssd->gc_heaps[ch_idx]);
      heap_pop_min(&ssd->gc_heaps[ch_idx]);
      blk->is_candidate = 0;
      bytefs_push_back_candidate_list(ssd, blk);
    }
  }
}

void bytefs_gc(struct ssd *ssd) {
  struct nand_block *gc_blk;
  struct ppa free_blk_ppa, gc_blk_ppa;
  uint64_t gc_page_lpn;
  uint64_t current_time;
  struct nand_cmd cmd;
  int ch_idx = 0, gc_blk_offset = 0;

  // iterate over GC blocks and gather valid pages
      
  bytefs_generate_gc_heaps(ssd);
  while (1) {
    current_time = get_time_ns();
    // get new block that is ready for GC
    gc_blk = bytefs_gc_find_next_gc_nand_blk(ssd, &ch_idx);
    if (gc_blk == NULL) {
      // GC ends because cannot find more candidates
      bytefs_warn("GC force end, no candidate available");
      return;
    }
    gc_blk_ppa = bytefs_get_ppa_from_nand_blk(gc_blk);
    if (gc_blk->vpc != 0) {
      for (gc_blk_offset = 0; gc_blk_offset < ssd->sp.pgs_per_blk; gc_blk_offset++) {
        // get ppa of current GC block
        gc_blk_ppa.g.pg = gc_blk_offset;
        // bytefs_log("GC blk ch: %5d way: %5d blk: %5d pg: %5d",
        //     gc_blk_ppa.g.ch, gc_blk_ppa.g.lun, gc_blk_ppa.g.blk, gc_blk_ppa.g.pg);
        ppa2pgidx(ssd, &gc_blk_ppa);
        // data migration if this ppa is valid
        gc_page_lpn = get_rmap_ent(ssd, &gc_blk_ppa);
        if (gc_page_lpn != INVALID_LPN) {
          // interact with nand flash
          cmd.type = USER_IO;
          cmd.cmd = NAND_READ;
          cmd.stime = current_time;
          ssd_advance_status(ssd, &gc_blk_ppa, &cmd);
          backend_rw(ssd->bd, gc_blk_ppa.realppa, ssd->gc_buffer, 0);
          // validate the copied entries
          // bytefs_log("Free blk ch: %5d way: %5d blk: %5d pg: %5d",
          //     free_blk_ppa.g.ch, free_blk_ppa.g.lun, free_blk_ppa.g.blk, free_blk_ppa.g.pg);
          free_blk_ppa = get_new_page(ssd);
          set_maptbl_ent(ssd, gc_page_lpn, &free_blk_ppa);
          set_rmap_ent(ssd, gc_page_lpn, &free_blk_ppa);
          mark_page_valid(ssd, &free_blk_ppa);
          // interact with nand flash
          cmd.type = USER_IO;
          cmd.cmd = NAND_WRITE;
          cmd.stime = current_time;
          ssd_advance_status(ssd, &free_blk_ppa, &cmd);
          backend_rw(ssd->bd, free_blk_ppa.realppa, ssd->gc_buffer, 1);
          ssd_advance_write_pointer(ssd);
        }
      }
    }
    mark_block_free(ssd, &gc_blk_ppa);
    bytefs_assert(gc_blk->vpc == 0);
    bytefs_assert(gc_blk->ipc == 0);
    bytefs_push_back_free_list(ssd, gc_blk);

    // if GC should end, try end it
    if (bytefs_should_stop_gc(ssd)) {
      // put back candidates for possible future GC
      put_nand_blk_back_to_candidates(ssd);
      return;
    }
  }
}
