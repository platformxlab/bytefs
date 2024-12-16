#ifndef __NOVA_ISSUE_H__
#define __NOVA_ISSUE_H__

#include "../../../ssd/ftl.h"

#define NOVA_PA_START (BYTEFS_PA_START) // start from 4G
#define NOVA_PA_END   (BYTEFS_PA_END) // end at 6G 

// configuration -- end

#define NOVA_DUMMY_BUFFER_LEN (4096 * 2)
extern void *nova_dummy_buffer;
#define NOVA_INIT_DUMMY_BUFFER()                                                    \
do {                                                                                \
  if (!nova_dummy_buffer)                                                           \
    nova_dummy_buffer = vmalloc(NOVA_DUMMY_BUFFER_LEN);                             \
    printk(KERN_ERR "Dummy buffer alloc: %p\n", nova_dummy_buffer);                 \
} while (0)

typedef enum {
  NOVA_DATA_BYTE_ISSUE,
  NOVA_CSUM_ISSUE,
  NOVA_INODE_ISSUE,
  NOVA_DIR_ISSUE,
  NOVA_LOG_ISSUE,
  NOVA_ISSUE_ALL
} nova_byte_issue_t;

/* specify which debug level we're on */
#define NOVA_NO_DEBUG  				(0)
#define NOVA_DATA_ISSUE_DEBUG 		(1<<0)
#define NOVA_DIR_DEBUG 				(1<<1)
#define NOVA_DEBUG_64B_ALIGN  		(1<<2)
#define NOVA_DEBUG_TYPE 			NOVA_NO_DEBUG


// define our debug printk : this does nothing when we're at particular debug level
#if (NOVA_DEBUG_TYPE != NOVA_NO_DEBUG)
#define NOVA_bytefs_debug(loglevel, fmt, args...) \
    printk(loglevel fmt, ##args)
#else
#define NOVA_bytefs_debug(loglevel, fmt, args...)
#endif


#if (NOVA_DEBUG_TYPE == NOVA_DEBUG_64B_ALIGN)
#define NOVA_bytefs_64b_debug(loglevel, fmt, args...) \
    printk(loglevel fmt, ##args)
#else
#define NOVA_bytefs_64b_debug(loglevel, fmt, args...)
#endif


/**
 * Check if lpa is within range of [NOVA_PA_START,NOVA_PA_END) 
 * return lpa w.r.t. the start of disk. 
*/
static inline int64_t nova_debug_lpa_in_range(int64_t lpa){
  if(lpa < NOVA_PA_START || lpa >= NOVA_PA_END) {
    printk(KERN_ERR "lpa out of bound, currently 4G~8G, check your pmem");
    return -1;
  }
  return lpa - NOVA_PA_START;
}

extern int32_t nova_byte_issue(int is_write, uint64_t pfn, uint64_t offset, uint64_t size, void* buf, nova_byte_issue_t issue_type);

static inline uint64_t nova_virt_to_offset(uint64_t virt_addr) {
  return virt_addr % PAGE_SIZE;
}


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

// cache and allocate the cache
// #define NOVA_CACHE_BYTE_ISSUE(virt_addr, cache_ptr, issue_type) \
// do {											   \
// 	cache_ptr = vmalloc(sizeof(*cache_ptr)); \
// 	if(cache_ptr == NULL) { \
// 		cache_ptr = vmalloc(sizeof(*cache_ptr)); \
// 	} \
// 	if(cache_ptr == NULL || \
// 	0 > nova_byte_issue(0, page_to_pfn(virt_to_page((uint64_t)virt_addr)), \
// 	nova_virt_to_offset((uint64_t)virt_addr), sizeof(*virt_addr), cache_ptr, issue_type)) {  \
// 		printk(KERN_ERR "%s : read failed", __func__); \
// 	}				\
// } while (0)
#define NOVA_CACHE_BYTE_ISSUE(virt_addr, cache_ptr, issue_type)                     \
do {                                                                                \
  /* NOVA_INIT_DUMMY_BUFFER();*/                                                    \
  cache_ptr = nova_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL){                                                           \
    cache_ptr = vmalloc(sizeof(*virt_addr));                                        \
  }                                                                                 \
  nova_byte_issue(0, page_to_pfn(virt_to_page((uint64_t) virt_addr)),       \
      nova_virt_to_offset((uint64_t) virt_addr), sizeof(*virt_addr),                \
      cache_ptr, issue_type) ;                                                      \
                                                                                  \
} while (0)

// cache without allocation
#define NOVA_CACHE_UPDATE_BYTE_ISSUE(virt_addr, cache_ptr, issue_type)              \
do {                                                                                \
  cache_ptr = nova_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL){                                                           \
    cache_ptr = vmalloc(sizeof(*virt_addr));                                        \
  }                                                                                 \
  nova_byte_issue(1, page_to_pfn(virt_to_page((uint64_t) virt_addr)),               \
      nova_virt_to_offset((uint64_t) virt_addr), sizeof(*virt_addr),                \
      cache_ptr, issue_type);                                                        \
} while (0)


// decache but not deallocate
#define NOVA_DECACHE_UPDATE_BYTE_ISSUE(virt_addr, cache_ptr, issue_type)            \
do {                                                                                \
cache_ptr = nova_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL){                                                           \
    cache_ptr = vmalloc(sizeof(*virt_addr));                                        \
  }                                                                            \
  if (0 > nova_byte_issue(1, page_to_pfn(virt_to_page((uint64_t) virt_addr)),       \
      nova_virt_to_offset((uint64_t) virt_addr), sizeof(*virt_addr),                \
      cache_ptr, issue_type)) {                                                     \
    printk(KERN_ERR "%s : write failed", __func__);                                 \
  }                                                                                 \
} while (0)

// decache and write
// #define NOVA_DECACHE_FLUSH_BYTE_ISSUE(virt_addr, cache_ptr, issue_type) \
// do {	\
// 	if(cache_ptr == NULL || \
// 	0 > nova_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)), \
// 	nova_virt_to_offset(virt_addr), sizeof(*virt_addr), cache_ptr, issue_type)) { \
// 		printk(KERN_ERR "%s : read failed", __func__); \
// 	} \
// 	vfree(cache_ptr);	\
// } while (0) 
#define NOVA_DECACHE_FLUSH_BYTE_ISSUE(virt_addr, cache_ptr, issue_type)             \
do {	                                                                              \
  cache_ptr = nova_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL){                                                           \
    cache_ptr = vmalloc(sizeof(*virt_addr));                                        \
  }     \
  nova_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)),                            \
      nova_virt_to_offset(virt_addr), sizeof(*virt_addr), cache_ptr, issue_type);\
 } while (0) 

// decache without write
// #define NOVA_DECACHE_END_BYTE_ISSUE(virt_addr, cache_ptr) \
// do {	\
// 	if(cache_ptr) { \
// 		vfree(cache_ptr);	\
// 	} \
// } while (0) 
#define NOVA_DECACHE_END_BYTE_ISSUE(virt_addr, cache_ptr)                           \
do {	                                                                              \
} while (0) 

// #define NOVA_CACHE_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len, issue_type) \
// do {	\
// 	cache_ptr = vmalloc(len);	\
// 	if(cache_ptr == NULL) { \
// 		cache_ptr = vmalloc(len); \
// 	} \
// 	if(cache_ptr == NULL || \
// 	0 > nova_byte_issue(0, page_to_pfn(virt_to_page((uint64_t)virt_addr)), \
// 	 nova_virt_to_offset((uint64_t)virt_addr), len, cache_ptr, issue_type)) { \
// 		printk(KERN_ERR "%s : read failed", __func__); \
// 	} 	\
// } while (0)
#define NOVA_CACHE_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len, issue_type)            \
do {	                                                                              \
  cache_ptr = nova_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL){                                                           \
    cache_ptr = vmalloc(sizeof(*virt_addr));                                        \
  }     \
  if (len > NOVA_DUMMY_BUFFER_LEN) {                                                \
    cache_ptr = vmalloc(len);                                                       \
  }                                                                        \
  nova_byte_issue(0, page_to_pfn(virt_to_page((uint64_t) virt_addr)),       \
      nova_virt_to_offset((uint64_t) virt_addr), len, cache_ptr, issue_type);      \
} while (0)

// #define NOVA_CACHE_BYTE_ALLOC_LEN(virt_addr, cache_ptr, len, issue_type) \
// do {	\
// 	cache_ptr = vmalloc(len);	\
// 	if(cache_ptr == NULL) { \
// 		cache_ptr = vmalloc(len); \
// 	} \
// } while (0)
#define NOVA_CACHE_BYTE_ALLOC_LEN(virt_addr, cache_ptr, len, issue_type)            \
do {                                                                                \
  if (len > NOVA_DUMMY_BUFFER_LEN) {                                                \
    cache_ptr = vmalloc(len);                                                       \
  } else {                                                                          \
    NOVA_INIT_DUMMY_BUFFER();                                                       \
    cache_ptr = nova_dummy_buffer;	                                                \
  }                                                                                 \
} while (0)

// #define NOVA_DECACHE_FLUSH_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len, issue_type) \
// do {	\
// 	if(cache_ptr == NULL || \
// 	0 > nova_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)), \
// 	nova_virt_to_offset(virt_addr), len, cache_ptr, issue_type)) { \
// 		printk(KERN_ERR "%s : write failed", __func__); \
// 	} \
// 	if(cache_ptr) { \
// 		vfree(cache_ptr);	\
// 	}else { \
// 		printk(KERN_ERR "bad ptr in %s", __func__); \
// 	}  \
// } while (0)
#define NOVA_DECACHE_FLUSH_BYTE_ISSUE_LEN(virt_addr, cache_ptr, len, issue_type)    \
do {                                                                                \
  cache_ptr = nova_dummy_buffer; /* buffer size always enough */                    \
  if (cache_ptr == NULL){                                                           \
    cache_ptr = vmalloc(sizeof(*virt_addr));                                        \
  }     \
  if (len > NOVA_DUMMY_BUFFER_LEN) {                                                \
    cache_ptr = vmalloc(len);                                                       \
  }                                                                        \
  if (0 > nova_byte_issue(1, page_to_pfn(virt_to_page(virt_addr)),                  \
      nova_virt_to_offset(virt_addr), len, cache_ptr, issue_type)) {                \
    printk(KERN_ERR                                                                 \
        "%s : write failed page %lu, offset %lu, len %lu, cache_ptr %lu",           \
        __func__, page_to_pfn(virt_to_page(virt_addr)),                             \
        nova_virt_to_offset(virt_addr), len);                                       \
  }                                                                                 \
  if (cache_ptr != NULL && len > NOVA_DUMMY_BUFFER_LEN) {                           \
    vfree(cache_ptr);                                                               \
  }                                                                                 \
} while (0)

// this shortcut has several contraints
// you must have a struct nova_inode pionter called pi defined before using this snippet

#define NOVA_MESURE_PI_SHORTCUT()                                                   \
do {                                                                                \
  struct nova_inode *pi_c;                                                          \
  CACHE_BYTE_ISSUE(pi, pi_c);                                                       \
  DECACHE_END_BYTE_ISSUE(pi, pi_c);                                                 \
} while (0)

#endif

