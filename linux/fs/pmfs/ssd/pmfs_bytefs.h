#ifndef _PMFS_BYTEFS_H_
#define _PMFS_BYTEFS_H_

#include "../../../ssd/ftl.h"
#include "../../../ssd/cow.h"

// conifgurations -- start

// enable page cache or not
#define PG_CACHE_ENABLE      0
#define BUFFER_CACHE_ENABLE  0

// #define ENABLE_JOURNAL 

// both in unit of physical address : PFN * (4<<10)

// in total 32 G, less than disk size (40G)
#define PA_START (BYTEFS_PA_START) // start from 4G
#define PA_END   (BYTEFS_PA_END) // end at 36G

// to stop enabling the following features, comment these define out

// #define PMFS_BYTE_MEASURE_ENABLE

// configuration -- end

#define PMFS_DUMMY_BUFFER_LEN (4096 * 2)
extern void *pmfs_dummy_buffer;
#define PMFS_INIT_DUMMY_BUFFER()                                                    \
do {                                                                                \
  if (!pmfs_dummy_buffer)                                                           \
    pmfs_dummy_buffer = vmalloc(PMFS_DUMMY_BUFFER_LEN);                             \
} while (0)

/**
 * Check if lpa is within range of [PA_START,PA_END) 
 * return lpa w.r.t. the start of disk. 
*/
static inline int64_t pmfs_debug_lpa_in_range(uint64_t lpa){
  if(lpa < PA_START || lpa >= PA_END) {
    printk(KERN_ERR "lpa out of bound, currently 4G~8G, check your pmem");
    return -1;
  }
  return lpa - PA_START;
}

extern int32_t pmfs_byte_issue(int is_write, uint64_t pfn, uint64_t offset, uint64_t size, void* buf);
extern int32_t pmfs_data_issue(int is_write, uint64_t pfn, uint64_t offset, uint64_t size, void* buf, 
  struct file* filp);

static inline uint64_t pmfs_virt_to_offset(uint64_t virt_addr) {
  return virt_addr % PAGE_SIZE;
}

#define PMFS_NO_DEBUG  					(0)
#define PMFS_DATA_ISSUE_DEBUG 			(1<<0)
#define PMFS_DIR_DEBUG  				(1<<1)
#define PMFS_DEBUG_64B_ALIGN 			(1<<2)
#define PMFS_DEBUG_TYPE PMFS_NO_DEBUG


// define our debug printk
#if (PMFS_DEBUG_TYPE != PMFS_NO_DEBUG)
#define pmfs_bytefs_debug(loglevel, fmt, args...) \
    printk(loglevel fmt, ##args)
#else
#define pmfs_bytefs_debug(loglevel, fmt, args...)
#endif


#if (PMFS_DEBUG_TYPE == PMFS_DEBUG_64B_ALIGN)
#define pmfs_bytefs_64b_debug(loglevel, fmt, args...) \
    printk(loglevel fmt, ##args)
#else
#define pmfs_bytefs_64b_debug(loglevel, fmt, args...)
#endif


/**
 * A series of macros used to cache and de-cache the byte issue content pointed by virt_addr to region pointed by cache_ptr.
 * Currently, the strategy for simplicity is to use one of the following calls to emulate the overhead
 * of writing / reading to / from PM.
 * 
 * Example : 
 * 		if I wan to write an inode to PM, I first call 
 * 			CACHE_BYTE_ISSUE(pi, pi_c);
 * 
 * 		After the last use of pi, we use 
 * 			DECACHE_FLUSH_BYTE_ISSUE(pi, pi_c);
 * 
 * 		Notice we say that the last of pi instead of pi_c, because current pi_c is 
 * 		only used to add overhead without actual meaning (storing and used to extract real data)
 * 
 * 		if I want to read an inode from PM, I first call
 * 			CACHE_BYTE_ISSUE(pi, pi_c);
 * 		then I call
 * 			DECACHE_END_BYTE_ISSUE(pi, pi_c)
 * 		after the last use of pi, because we don't have to write back for reading inode
 * 			
 * Assumption: 
 * 1.		The size we cache can be reflected by the size of structure obtained by `sizeof()`
 * 
 * 2.		cache_ptr and virt_addr should be of same type
*/ 
// #define CACHE_BYTE_ISSUE(virt_addr, cache_ptr) \
// do {											   \
// 	cache_ptr = vmalloc(sizeof(*cache_ptr)); \
// 	if(cache_ptr == NULL) { \
// 		cache_ptr = vmalloc(sizeof(*cache_ptr)); \
// 	} \
// } while (0)
#define CACHE_BYTE_ISSUE(virt_addr, cache_ptr)                                      \
do {                                                                                \
  PMFS_INIT_DUMMY_BUFFER();                                                         \
  cache_ptr = pmfs_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL ||                                                          \
      0 > pmfs_byte_issue(0, page_to_pfn(virt_to_page((uint64_t) virt_addr)),       \
      pmfs_virt_to_offset((uint64_t) virt_addr), sizeof(*virt_addr), cache_ptr)) {  \
    printk(KERN_ERR "%s : read failed", __func__);                                  \
  }                                                                                 \
} while (0)

// cache without allocation
#define CACHE_UPDATE_BYTE_ISSUE(virt_addr, cache_ptr)                               \
do {                                                                                \
  if (cache_ptr == NULL ||                                                          \
      0 > pmfs_byte_issue(1, page_to_pfn(virt_to_page((uint64_t) virt_addr)),       \
      pmfs_virt_to_offset((uint64_t) virt_addr), sizeof(*virt_addr), cache_ptr)) {  \
    printk(KERN_ERR "%s : write failed", __func__);                                 \
  }                                                                                 \
} while (0)


// decache but not deallocate
#define DECACHE_UPDATE_BYTE_ISSUE(virt_addr, cache_ptr)                             \
do {	                                                                              \
  if (cache_ptr == NULL ||                                                          \
      0 > pmfs_byte_issue(1, page_to_pfn(virt_to_page((uint64_t) virt_addr)),       \
      pmfs_virt_to_offset((uint64_t) virt_addr), sizeof(*virt_addr), cache_ptr)) {  \
    printk(KERN_ERR "%s : write failed", __func__);                                 \
  }                                                                                 \
} while (0)

// decache and write
// #define DECACHE_FLUSH_BYTE_ISSUE(virt_addr, cache_ptr) \
// do {	\
// 	if(cache_ptr == NULL || \
// 	0 > pmfs_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)), \
// 	pmfs_virt_to_offset(virt_addr), sizeof(*virt_addr), cache_ptr)) { \
// 		printk(KERN_ERR "%s : read failed", __func__); \
// 	} \
// 	vfree(cache_ptr);	\
// } while (0) 
#define DECACHE_FLUSH_BYTE_ISSUE(virt_addr, cache_ptr)                              \
do {	                                                                              \
  if (cache_ptr == NULL ||                                                          \
      0 > pmfs_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)),                  \
      pmfs_virt_to_offset(virt_addr), sizeof(*virt_addr), cache_ptr)) {             \
    printk(KERN_ERR "%s : write failed", __func__);                                 \
  }                                                                                 \
} while (0) 

// decache without write
// #define DECACHE_END_BYTE_ISSUE(virt_addr, cache_ptr) \
// do {	\
// 	if(cache_ptr) { \
// 		vfree(cache_ptr);	\
// 	} \
// } while (0) 
#define DECACHE_END_BYTE_ISSUE(virt_addr, cache_ptr)                                \
do {                                                                                \
} while (0) 

// #define CACHE_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len) \
// do {	\
// 	cache_ptr = vmalloc(len);	\
// } while (0)
#define CACHE_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len)                             \
do {	                                                                              \
  if (len > PMFS_DUMMY_BUFFER_LEN) {                                                \
    cache_ptr = vmalloc(len);                                                       \
  } else {                                                                          \
    PMFS_INIT_DUMMY_BUFFER();                                                       \
    cache_ptr = pmfs_dummy_buffer;                                                  \
  }                                                                                 \
  if (0 > pmfs_byte_issue(0, page_to_pfn(virt_to_page(virt_addr)),                  \
      pmfs_virt_to_offset(virt_addr), sizeof(*virt_addr), cache_ptr)) {             \
    printk(KERN_ERR "%s : read failed", __func__);                                  \
  } 	                                                                              \
} while (0)

// #define CACHE_BYTE_ALLOC_LEN(virt_addr, cache_ptr, len) \
// do {	\
// 	if (len>4096) {				\
// 		cache_ptr = vmalloc(len);			\
// 	} else {					\
// 		cache_ptr = kmalloc(len,GFP_KERNEL);	\
// 		if(cache_ptr == NULL) { \
// 			cache_ptr = kmalloc(len,GFP_NOFS); \
// 		} \
// 	} \
// } while (0)
#define CACHE_BYTE_ALLOC_LEN(virt_addr, cache_ptr, len)                             \
do {	                                                                              \
  if (len > PMFS_DUMMY_BUFFER_LEN) {                                                \
    cache_ptr = vmalloc(len);                                                       \
  } else {                                                                          \
    PMFS_INIT_DUMMY_BUFFER();                                                       \
    cache_ptr = pmfs_dummy_buffer;                                                  \
  }                                                                                 \
} while (0)

// #define DECACHE_FLUSH_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len) \
// do {	\
// 	if(cache_ptr == NULL)\
// 		printk(KERN_ERR "%s : null ptr", __func__); \
// 	else if(									\
// 	0 > pmfs_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)), \
// 	pmfs_virt_to_offset(virt_addr), len, cache_ptr)) { \
// 		printk(KERN_ERR "%s : write failed page %lu offset %lu len %lu", __func__,page_to_pfn(virt_to_page(virt_addr)), pmfs_virt_to_offset(virt_addr), len); \
// 	} \
// 	if (len>4096){				\
// 		vfree(cache_ptr);		\
// 	} else {					\
// 		kfree(cache_ptr);		\
// 	}\
// } while (0)
#define DECACHE_FLUSH_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len)                     \
do {	                                                                              \
  if (cache_ptr == NULL) {                                                          \
    PMFS_INIT_DUMMY_BUFFER();                                                       \
    if (len > PMFS_DUMMY_BUFFER_LEN) cache_ptr = vmalloc(len);                      \
    else cache_ptr = pmfs_dummy_buffer;                                             \
  }                                                                                 \
  if (0 > pmfs_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)),                  \
      pmfs_virt_to_offset(virt_addr), len, cache_ptr)) {                            \
    printk(KERN_ERR                                                                 \
        "%s : write failed page %lu, offset %lu, len %lu, cache_ptr %lu",           \
        __func__, page_to_pfn(virt_to_page(virt_addr)),                             \
        pmfs_virt_to_offset(virt_addr), len);                                       \
  }                                                                                 \
  if (cache_ptr != NULL && len > PMFS_DUMMY_BUFFER_LEN) {                           \
    vfree(cache_ptr);                                                               \
  }                                                                                 \
} while (0)

// this shortcut has several contraints
// you must have a struct pmfs_inode pionter called pi defined before using this snippet

#define MESURE_PI_SHORTCUT()                                                        \
do {                                                                                \
  struct pmfs_inode *pi_c;                                                          \
  CACHE_BYTE_ISSUE(pi, pi_c);                                                       \
  DECACHE_END_BYTE_ISSUE(pi, pi_c);                                                 \
} while (0)


#endif
