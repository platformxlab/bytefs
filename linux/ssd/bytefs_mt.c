#include "bytefs_mt.h"
#include "ftl.h"

// multiplicative hashing function
static uint32_t bytefs_mt_hash(uint64_t key, uint64_t size) {
  // a large prime number
  const unsigned long a = 990590075063L;
  // word_size - log2(BYTEFS_MT_SIZE)
  const unsigned long b = 32 - 10;
  key ^= key >> b; 
  return ((a * key) >> b) % size;
}

// IMT
void imt_insert(struct ssd *ssd, uint64_t lpa, uint64_t log_offset) {
  struct indirection_mte intermediate;
  uint32_t hkey = bytefs_mt_hash(lpa, BYTEFS_MT_SIZE);
  uint32_t i = hkey;
  uint32_t psl = 0;
  // bytefs_log("IMT insert on LPA %lld", lpa);
  bytefs_assert(ssd->imt_size < BYTEFS_MT_SIZE);
  bytefs_assert(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
  bytefs_assert(log_offset != INVALID_LOG_OFFSET);
  while (ssd->indirection_mt[i].lpa != INVALID_LPN) {
    if (ssd->indirection_mt[i].lpa == lpa) {
      // entry found, modify
      ssd->indirection_mt[i].log_offset = log_offset;
      bytefs_assert(ssd->indirection_mt[i].psl == psl);
      return;
    } else if (psl > ssd->indirection_mt[i].psl) {
      // swap
      memcpy(&intermediate, &ssd->indirection_mt[i], sizeof(struct indirection_mte));
      ssd->indirection_mt[i].lpa = lpa;
      ssd->indirection_mt[i].log_offset = log_offset;
      ssd->indirection_mt[i].psl = psl;
      lpa = intermediate.lpa;
      log_offset = intermediate.log_offset;
      psl = intermediate.psl;
    }
    psl++;
    i = (i + 1) % BYTEFS_MT_SIZE;
  }
  // new entry insertion
  ssd->indirection_mt[i].lpa = lpa;
  ssd->indirection_mt[i].log_offset = log_offset;
  ssd->indirection_mt[i].psl = psl;
  ssd->imt_size++;
}

struct indirection_mte *imt_get(struct ssd *ssd, uint64_t lpa) {
  uint32_t hkey = bytefs_mt_hash(lpa, BYTEFS_MT_SIZE);
  uint32_t i = hkey;
  // bytefs_log("IMT get on LPA %lld", lpa);
  bytefs_assert(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
  while (ssd->indirection_mt[i].lpa != INVALID_LPN) {
    if (ssd->indirection_mt[i].lpa == lpa) {
      return &ssd->indirection_mt[i];
    }
    i = (i + 1) % BYTEFS_MT_SIZE;
  }
  return 0;
}

int imt_remove(struct ssd *ssd, uint64_t lpa) {
  uint32_t hkey = bytefs_mt_hash(lpa, BYTEFS_MT_SIZE);
  uint32_t i = hkey;
  uint32_t perv_i;
  // bytefs_log("IMT remove on LPA %lld", lpa);
  bytefs_assert(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
  while (ssd->indirection_mt[i].lpa != INVALID_LPN) {
    if (ssd->indirection_mt[i].lpa == lpa) {
      // entry found, chain remove
      perv_i = i;
      i = (i + 1) % BYTEFS_MT_SIZE;
      while (ssd->indirection_mt[i].lpa != INVALID_LPN && ssd->indirection_mt[i].psl > 0) {
        ssd->indirection_mt[perv_i].lpa = ssd->indirection_mt[i].lpa;
        ssd->indirection_mt[perv_i].log_offset = ssd->indirection_mt[i].log_offset;
        ssd->indirection_mt[perv_i].psl = ssd->indirection_mt[i].psl - 1;
        perv_i = i;
        i = (i + 1) % BYTEFS_MT_SIZE;
      }
      // clear out the entry found last
      ssd->indirection_mt[perv_i].lpa = INVALID_LPN;
      ssd->indirection_mt[perv_i].log_offset = INVALID_LOG_OFFSET;
      ssd->indirection_mt[perv_i].psl = 0;
      ssd->imt_size--;
      return 0;
    }
    i = (i + 1) % BYTEFS_MT_SIZE;
  }
  bytefs_assert_msg(false, "IMT remove LPN: %lld not found", lpa);
  return -1;
}

// CMT
void cmt_insert(struct ssd *ssd, uint64_t lpa) {
  struct coalescing_mte intermediate;
  uint64_t offset = lpa & PG_MASK;
  uint64_t lpn = lpa - offset;
  uint64_t bitmask = 1UL << (offset / BYTEFS_LOG_REGION_GRANDULARITY);
  uint32_t hkey = bytefs_mt_hash(lpn, BYTEFS_MT_SIZE);
  uint32_t i = hkey;
  uint32_t psl = 0;
  // bytefs_log("CMT insert on LPA %lld base %lld bitmask %016llX", lpa, lpn, bitmask);
  bytefs_assert(ssd->cmt_size < BYTEFS_MT_SIZE);
  bytefs_assert(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
  while (ssd->coalescing_mt[i].lpn != INVALID_LPN) {
    if (ssd->coalescing_mt[i].lpn == lpn) {
      // entry found, modify
      ssd->coalescing_mt[i].bitmap |= bitmask;
      bytefs_assert(ssd->coalescing_mt[i].psl == psl);
      // bytefs_log("CMT insert %lld modify bitmask -> %016llX", lpn, ssd->coalescing_mt[i].bitmap);
      return;
    } else if (psl > ssd->coalescing_mt[i].psl) {
      // swap
      memcpy(&intermediate, &ssd->coalescing_mt[i], sizeof(struct coalescing_mte));
      ssd->coalescing_mt[i].lpn = lpn;
      ssd->coalescing_mt[i].bitmap = bitmask;
      ssd->coalescing_mt[i].psl = psl;
      lpn = intermediate.lpn;
      bitmask = intermediate.bitmap;
      psl = intermediate.psl;
    }
    psl++;
    i = (i + 1) % BYTEFS_MT_SIZE;
  }
  // new entry insertion
  // bytefs_log("CMT insert create");
  ssd->coalescing_mt[i].lpn = lpn;
  ssd->coalescing_mt[i].bitmap = bitmask;
  ssd->coalescing_mt[i].psl = psl;
  ssd->cmt_size++;
  bytefs_assert(ssd->cmt_size <= BYTEFS_LOG_REGION_MAX_ENTRY_NUM);
}

struct coalescing_mte *cmt_get(struct ssd *ssd, uint64_t lpa) {
  uint64_t offset = lpa & PG_MASK;
  uint64_t lpn = lpa - offset;
  uint32_t hkey = bytefs_mt_hash(lpn, BYTEFS_MT_SIZE);
  uint32_t i = hkey;
  // bytefs_log("CMT get on LPA %lld base %lld", lpa, lpn);
  while (ssd->coalescing_mt[i].lpn != INVALID_LPN) {
    if (ssd->coalescing_mt[i].lpn == lpn) {
      return &ssd->coalescing_mt[i];
    }
    i = (i + 1) % BYTEFS_MT_SIZE;
  }
  return 0;
}

int cmt_remove(struct ssd *ssd, uint64_t lpa) {
  uint64_t offset = lpa & PG_MASK;
  uint64_t lpn = lpa - offset;
  uint64_t bitmask = 1UL << (offset / BYTEFS_LOG_REGION_GRANDULARITY);
  uint32_t hkey = bytefs_mt_hash(lpn, BYTEFS_MT_SIZE);
  uint32_t i = hkey;
  uint32_t perv_i;
  // bytefs_log("CMT remove on LPA %lld base %lld bitmask %016llX", lpa, lpn, bitmask);
  bytefs_assert(lpa % BYTEFS_LOG_REGION_GRANDULARITY == 0);
  while (ssd->coalescing_mt[i].lpn != INVALID_LPN) {
    if (ssd->coalescing_mt[i].lpn == lpn) {
      // chain remove
      perv_i = i;
      i = (i + 1) % BYTEFS_MT_SIZE;
      while (ssd->coalescing_mt[i].lpn != INVALID_LPN && ssd->coalescing_mt[i].psl > 0) {
        ssd->coalescing_mt[perv_i].lpn = ssd->coalescing_mt[i].lpn;
        ssd->coalescing_mt[perv_i].bitmap = ssd->coalescing_mt[i].bitmap;
        ssd->coalescing_mt[perv_i].psl = ssd->coalescing_mt[i].psl - 1;
        perv_i = i;
        i = (i + 1) % BYTEFS_MT_SIZE;
      }
      // clear out the entry found last
      ssd->coalescing_mt[perv_i].lpn = INVALID_LPN;
      ssd->coalescing_mt[perv_i].bitmap = 0;
      ssd->coalescing_mt[perv_i].psl = 0;
      ssd->cmt_size--;
      return 0;
    }
    i = (i + 1) % BYTEFS_MT_SIZE;
  }
  bytefs_assert_msg(false, "CMT remove LPN: %lld not found", lpa);
  return -1;
}

