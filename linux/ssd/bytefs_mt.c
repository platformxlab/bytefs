#include "bytefs_mt.h"
#include "ftl.h"

#ifndef __ASSOCIATIVE_CACHE_MAP_

// multiplicative hashing function
static uint32_t bytefs_mt_hash(uint32_t key, uint32_t size) {
  // a large prime number
  const unsigned long a = 990590075063L;
  // word_size - log2(TOBSSD_BMT_SIZE)
  const unsigned long b = 32 - 10;
  key ^= key >> b; 
  return ((a * key) >> b) % size;
}

void bmt_insert(struct ssd *ssd, uint64_t lpn, uint32_t buf_idx) {
  struct buffer_mte intermediate;
  uint32_t hkey = bytefs_mt_hash(lpn, TOBSSD_BMT_SIZE);
  uint32_t i = hkey;
  uint32_t psl = 0;
  // printk(KERN_ERR "BMT IST lpn: %lld HKEY: %d\n", lpn, hkey);
  bytefs_assert(ssd->buffer_mt_size < TOBSSD_BMT_SIZE);
  bytefs_assert(lpn != INVALID_LPN);
  bytefs_assert(buf_idx != INVALID_INDEX_32);
  while (ssd->buffer_mt[i].lpn != INVALID_LPN) {
    if (ssd->buffer_mt[i].lpn == lpn) {
      // printk("IST ITER %d: %lld\n", i, ssd->buffer_mt[i].lpn);
      ssd->buffer_mt[i].buf_idx = buf_idx;
      // printk(KERN_ERR "BMT update IDX: %d lpn: %lld BUF: %d TOT: %d\n", 
      //     i, lpn, ssd->buffer_mt[i].buf_idx, ssd->buffer_mt_size);
      return;
    } else if (psl > ssd->buffer_mt[i].psl) {
      // swap
      memcpy(&intermediate, &ssd->buffer_mt[i], sizeof(struct buffer_mte));
      ssd->buffer_mt[i].lpn = lpn;
      ssd->buffer_mt[i].buf_idx = buf_idx;
      ssd->buffer_mt[i].psl = psl;
      lpn = intermediate.lpn;
      buf_idx = intermediate.buf_idx;
      psl = intermediate.psl;
    }
    psl++;
    i = (i + 1) % TOBSSD_BMT_SIZE;
  }
  ssd->buffer_mt[i].lpn = lpn;
  ssd->buffer_mt[i].buf_idx = buf_idx;
  ssd->buffer_mt[i].psl = psl;
  ssd->buffer_mt_size++;
  bytefs_assert(ssd->buffer_mt_size <= TOBSSD_BUFFER_SIZE / ssd->sp.pgsz);
  // printk(KERN_ERR "BMT insert IDX: %d lpn: %lld BUF: %d TOT: %d\n", 
  //     i, lpn, ssd->buffer_mt[i].buf_idx, ssd->buffer_mt_size);
}

struct buffer_mte *bmt_get(struct ssd *ssd, uint64_t lpn) {
  uint32_t hkey = bytefs_mt_hash(lpn, TOBSSD_BMT_SIZE);
  uint32_t i = hkey;
  // printk(KERN_ERR "BMT GET lpn: %lld HKEY: %d\n", lpn, hkey);
  bytefs_assert(lpn != INVALID_LPN);
  while (ssd->buffer_mt[i].lpn != INVALID_LPN) {
    // printk("GET ITER %d: %lld\n", i, ssd->buffer_mt[i].lpn);
    if (ssd->buffer_mt[i].lpn == lpn) {
      // printk(KERN_ERR "BMT found IDX: %d lpn: %lld BUF: %d TOT: %d\n", 
      //     i, lpn, ssd->buffer_mt[i].buf_idx, ssd->buffer_mt_size);
      return &ssd->buffer_mt[i];
    }
    i = (i + 1) % TOBSSD_BMT_SIZE;
  }
  // printk(KERN_ERR "BMT not found IDX: %d lpn: %lld\n", i, lpn);
  return 0;
}

int bmt_remove(struct ssd *ssd, uint64_t lpn) {
  uint32_t hkey = bytefs_mt_hash(lpn, TOBSSD_BMT_SIZE);
  uint32_t i = hkey;
  uint32_t perv_i = 0;
  // printk(KERN_ERR "BMT HKEY: %d\n", hkey);
  bytefs_assert(lpn != INVALID_LPN);
  while (ssd->buffer_mt[i].lpn != INVALID_LPN) {
    // printk("RM ITER %d: %lld\n", i, ssd->buffer_mt[i].lpn);
    if (ssd->buffer_mt[i].lpn == lpn) {
      // printk(KERN_ERR "BMT rm found IDX: %d LPN: %lld BUF: %d TOT: %d\n", 
      //     i, lpn, ssd->buffer_mt[i].buf_idx, ssd->buffer_mt_size);
      perv_i = i;
      i = (i + 1) % TOBSSD_BMT_SIZE;
      while (ssd->buffer_mt[i].lpn != INVALID_LPN && ssd->buffer_mt[i].psl > 0) {
        ssd->buffer_mt[perv_i].lpn = ssd->buffer_mt[i].lpn;
        ssd->buffer_mt[perv_i].buf_idx = ssd->buffer_mt[i].buf_idx;
        ssd->buffer_mt[perv_i].psl = ssd->buffer_mt[i].psl - 1;
        perv_i = i;
        i = (i + 1) % TOBSSD_BMT_SIZE;
      }
      ssd->buffer_mt[perv_i].lpn = INVALID_LPN;
      ssd->buffer_mt[perv_i].buf_idx = INVALID_INDEX_32;
      ssd->buffer_mt[perv_i].psl = 0;
      ssd->buffer_mt_size--;
      return 0;
    }
    i = (i + 1) % TOBSSD_BMT_SIZE;
  }
  // printk(KERN_ERR "BMT rm not found IDX: %d lpn: %lld TOT: %d\n", 
  //     i, lpn, ssd->buffer_mt_size);
  bytefs_assert_msg(false, "BMT remove LPN: %lld not found", lpn);
  return -1;
}

#endif