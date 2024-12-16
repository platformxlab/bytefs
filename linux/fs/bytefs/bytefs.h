/*
 * BRIEF DESCRIPTION
 *
 * Definitions for the BYTEFS filesystem.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#ifndef __BYTEFS_H
#define __BYTEFS_H

#include <linux/fs.h>
#include <linux/dax.h>
#include <linux/init.h>
#include <linux/time.h>
#include <linux/rtc.h>
#include <linux/mm.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/rcupdate.h>
#include <linux/types.h>
#include <linux/rbtree.h>
#include <linux/radix-tree.h>
#include <linux/version.h>
#include <linux/kthread.h>
#include <linux/buffer_head.h>
#include <linux/uio.h>
#include <linux/iomap.h>
#include <linux/crc32c.h>
#include <asm/tlbflush.h>
#include <linux/version.h>
#include <linux/pfn_t.h>
#include <linux/pagevec.h>

#include "bytefs_def.h"
#include "stats.h"
#include "snapshot.h"
#include "ssd/bytefs_issue.h"

#define PAGE_SHIFT_2M 21
#define PAGE_SHIFT_1G 30


/*
 * Debug code
 */
#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#endif

/* #define bytefs_dbg(s, args...)		pr_debug(s, ## args) */
#define bytefs_dbg(s, args ...)		pr_info(s, ## args)
#define bytefs_dbg1(s, args ...)
#define bytefs_err(sb, s, args ...)	bytefs_error_mng(sb, s, ## args)
#define bytefs_warn(s, args ...)		pr_warn(s, ## args)
#define bytefs_info(s, args ...)		pr_info(s, ## args)

extern unsigned int bytefs_dbgmask;
#define BYTEFS_DBGMASK_MMAPHUGE	       (0x00000001)
#define BYTEFS_DBGMASK_MMAP4K	       (0x00000002)
#define BYTEFS_DBGMASK_MMAPVERBOSE       (0x00000004)
#define BYTEFS_DBGMASK_MMAPVVERBOSE      (0x00000008)
#define BYTEFS_DBGMASK_VERBOSE	       (0x00000010)
#define BYTEFS_DBGMASK_TRANSACTION       (0x00000020)

#define bytefs_dbg_mmap4k(s, args ...)		 \
	((bytefs_dbgmask & BYTEFS_DBGMASK_MMAP4K) ? bytefs_dbg(s, args) : 0)
#define bytefs_dbg_mmapv(s, args ...)		 \
	((bytefs_dbgmask & BYTEFS_DBGMASK_MMAPVERBOSE) ? bytefs_dbg(s, args) : 0)
#define bytefs_dbg_mmapvv(s, args ...)		 \
	((bytefs_dbgmask & BYTEFS_DBGMASK_MMAPVVERBOSE) ? bytefs_dbg(s, args) : 0)

#define bytefs_dbg_verbose(s, args ...)		 \
	((bytefs_dbgmask & BYTEFS_DBGMASK_VERBOSE) ? bytefs_dbg(s, ##args) : 0)
#define bytefs_dbgv(s, args ...)	bytefs_dbg_verbose(s, ##args)
#define bytefs_dbg_trans(s, args ...)		 \
	((bytefs_dbgmask & BYTEFS_DBGMASK_TRANSACTION) ? bytefs_dbg(s, ##args) : 0)

#define BYTEFS_ASSERT(x) do {\
			       if (!(x))\
				       bytefs_warn("assertion failed %s:%d: %s\n", \
			       __FILE__, __LINE__, #x);\
		       } while (0)

#define bytefs_set_bit		       __test_and_set_bit_le
#define bytefs_clear_bit		       __test_and_clear_bit_le
#define bytefs_find_next_zero_bit	       find_next_zero_bit_le

#define clear_opt(o, opt)	(o &= ~BYTEFS_MOUNT_ ## opt)
#define set_opt(o, opt)		(o |= BYTEFS_MOUNT_ ## opt)
#define test_opt(sb, opt)	(BYTEFS_SB(sb)->s_mount_opt & BYTEFS_MOUNT_ ## opt)

#define BYTEFS_LARGE_INODE_TABLE_SIZE    (0x200000)
/* BYTEFS size threshold for using 2M blocks for inode table */
#define BYTEFS_LARGE_INODE_TABLE_THREASHOLD    (0x20000000)
/*
 * bytefs inode flags
 *
 * BYTEFS_EOFBLOCKS_FL	There are blocks allocated beyond eof
 */
#define BYTEFS_EOFBLOCKS_FL      0x20000000
/* Flags that should be inherited by new inodes from their parent. */
#define BYTEFS_FL_INHERITED (FS_SECRM_FL | FS_UNRM_FL | FS_COMPR_FL | \
			    FS_SYNC_FL | FS_NODUMP_FL | FS_NOATIME_FL |	\
			    FS_COMPRBLK_FL | FS_NOCOMP_FL | \
			    FS_JOURNAL_DATA_FL | FS_NOTAIL_FL | FS_DIRSYNC_FL)
/* Flags that are appropriate for regular files (all but dir-specific ones). */
#define BYTEFS_REG_FLMASK (~(FS_DIRSYNC_FL | FS_TOPDIR_FL))
/* Flags that are appropriate for non-directories/regular files. */
#define BYTEFS_OTHER_FLMASK (FS_NODUMP_FL | FS_NOATIME_FL)
#define BYTEFS_FL_USER_VISIBLE (FS_FL_USER_VISIBLE | BYTEFS_EOFBLOCKS_FL)

/* IOCTLs */
#define	BYTEFS_PRINT_TIMING		0xBCD00010
#define	BYTEFS_CLEAR_STATS		0xBCD00011
#define	BYTEFS_PRINT_LOG			0xBCD00013
#define	BYTEFS_PRINT_LOG_BLOCKNODE	0xBCD00014
#define	BYTEFS_PRINT_LOG_PAGES		0xBCD00015
#define	BYTEFS_PRINT_FREE_LISTS		0xBCD00018


#define	READDIR_END			(ULONG_MAX)
#define	INVALID_CPU			(-1)
#define	ANY_CPU				(65536)
#define	FREE_BATCH			(16)
#define	DEAD_ZONE_BLOCKS		(256)

extern int measure_timing;
extern int metadata_csum;
extern int unsafe_metadata;
extern int wprotect;
extern int data_csum;
extern int data_parity;
extern int dram_struct_csum;

extern unsigned int blk_type_to_shift[BYTEFS_BLOCK_TYPE_MAX];
extern unsigned int blk_type_to_size[BYTEFS_BLOCK_TYPE_MAX];



#define	MMAP_WRITE_BIT	0x20UL	// mmaped for write
#define	IS_MAP_WRITE(p)	((p) & (MMAP_WRITE_BIT))
#define	MMAP_ADDR(p)	((p) & (PAGE_MASK))


/* Mask out flags that are inappropriate for the given type of inode. */
static inline __le32 bytefs_mask_flags(umode_t mode, __le32 flags)
{
	flags &= cpu_to_le32(BYTEFS_FL_INHERITED);
	if (S_ISDIR(mode))
		return flags;
	else if (S_ISREG(mode))
		return flags & cpu_to_le32(BYTEFS_REG_FLMASK);
	else
		return flags & cpu_to_le32(BYTEFS_OTHER_FLMASK);
}

/* Update the crc32c value by appending a 64b data word. */
#define bytefs_crc32c_qword(qword, crc) do { \
	asm volatile ("crc32q %1, %0" \
		: "=r" (crc) \
		: "r" (qword), "0" (crc)); \
	} while (0)

static inline u32 bytefs_crc32c(u32 crc, const u8 *data, size_t len)
{
	u8 *ptr = (u8 *) data;
	u64 acc = crc; /* accumulator, crc32c value in lower 32b */
	u32 csum;

	/* x86 instruction crc32 is part of SSE-4.2 */
	if (static_cpu_has(X86_FEATURE_XMM4_2)) {
		/* This inline assembly implementation should be equivalent
		 * to the kernel's crc32c_intel_le_hw() function used by
		 * crc32c(), but this performs better on test machines.
		 */
		while (len > 8) {
			asm volatile(/* 64b quad words */
				"crc32q (%1), %0"
				: "=r" (acc)
				: "r"  (ptr), "0" (acc)
			);
			ptr += 8;
			len -= 8;
		}

		while (len > 0) {
			asm volatile(/* trailing bytes */
				"crc32b (%1), %0"
				: "=r" (acc)
				: "r"  (ptr), "0" (acc)
			);
			ptr++;
			len--;
		}

		csum = (u32) acc;
	} else {
		/* The kernel's crc32c() function should also detect and use the
		 * crc32 instruction of SSE-4.2. But calling in to this function
		 * is about 3x to 5x slower than the inline assembly version on
		 * some test machines.
		 */
		csum = crc32c(crc, data, len);
	}

	return csum;
}

/* uses CPU instructions to atomically write up to 8 bytes */
static inline void bytefs_memcpy_atomic(void *dst, const void *src, u8 size)
{
	// This function doesn't need memunlock
	// However, if you do grep -r bytefs_memcpy_atomic in BYTEFS, it's not called.
	switch (size) {
	case 1: {
		volatile u8 *daddr = dst;
		const u8 *saddr = src;
		*daddr = *saddr;
		break;
	}
	case 2: {
		volatile __le16 *daddr = dst;
		const u16 *saddr = src;
		*daddr = cpu_to_le16(*saddr);
		break;
	}
	case 4: {
		volatile __le32 *daddr = dst;
		const u32 *saddr = src;
		*daddr = cpu_to_le32(*saddr);
		break;
	}
	case 8: {
		volatile __le64 *daddr = dst;
		const u64 *saddr = src;
		*daddr = cpu_to_le64(*saddr);
		break;
	}
	default:
		bytefs_dbg("error: memcpy_atomic called with %d bytes\n", size);
		//BUG();
	}
}

static inline int memcpy_to_pmem_nocache(void *dst, const void *src,
	unsigned int size)
{
	int ret;

	ret = __copy_from_user_inatomic_nocache(dst, src, size);

	return ret;
}


/* assumes the length to be 4-byte aligned */
static inline void memset_nt(void *dest, uint32_t dword, size_t length)
{
	uint64_t dummy1, dummy2;
	uint64_t qword = ((uint64_t)dword << 32) | dword;

	asm volatile ("movl %%edx,%%ecx\n"
		"andl $63,%%edx\n"
		"shrl $6,%%ecx\n"
		"jz 9f\n"
		"1:	 movnti %%rax,(%%rdi)\n"
		"2:	 movnti %%rax,1*8(%%rdi)\n"
		"3:	 movnti %%rax,2*8(%%rdi)\n"
		"4:	 movnti %%rax,3*8(%%rdi)\n"
		"5:	 movnti %%rax,4*8(%%rdi)\n"
		"8:	 movnti %%rax,5*8(%%rdi)\n"
		"7:	 movnti %%rax,6*8(%%rdi)\n"
		"8:	 movnti %%rax,7*8(%%rdi)\n"
		"leaq 64(%%rdi),%%rdi\n"
		"decl %%ecx\n"
		"jnz 1b\n"
		"9:	movl %%edx,%%ecx\n"
		"andl $7,%%edx\n"
		"shrl $3,%%ecx\n"
		"jz 11f\n"
		"10:	 movnti %%rax,(%%rdi)\n"
		"leaq 8(%%rdi),%%rdi\n"
		"decl %%ecx\n"
		"jnz 10b\n"
		"11:	 movl %%edx,%%ecx\n"
		"shrl $2,%%ecx\n"
		"jz 12f\n"
		"movnti %%eax,(%%rdi)\n"
		"12:\n"
		: "=D"(dummy1), "=d" (dummy2)
		: "D" (dest), "a" (qword), "d" (length)
		: "memory", "rcx");
}


#include "super.h" // Remove when we factor out these and other functions.

/* Translate an offset the beginning of the Bytefs instance to a PMEM address.
 *
 * If this is part of a read-modify-write of the block,
 * bytefs_memunlock_block() before calling!
 */
static inline void *bytefs_get_block(struct super_block *sb, u64 block)
{
	struct bytefs_super_block *ps = bytefs_get_super(sb);

	return block ? ((void *)ps + block) : NULL;
}

static inline int bytefs_get_reference(struct super_block *sb, u64 block,
	void *dram, void **nvmm, size_t size)
{
	int rc;
	
	char* cache_ptr; // allocated for overhead simulation : haor2
	
	*nvmm = bytefs_get_block(sb, block);

	// BYTEFS_CACHE_BYTE_ISSUE_LEN((*nvmm), cache_ptr, ((u64)size), BYTEFS_INODE_ISSUE);
	// BYTEFS_DECACHE_END_BYTE_ISSUE((*nvmm), cache_ptr);
	
	rc = memcpy_mcsafe(dram, *nvmm, size);
	return rc;
}


static inline u64
bytefs_get_addr_off(struct bytefs_sb_info *sbi, void *addr)
{
	BYTEFS_ASSERT((addr >= sbi->virt_addr) &&
			(addr < (sbi->virt_addr + sbi->initsize)));
	return (u64)(addr - sbi->virt_addr);
}

static inline u64
bytefs_get_block_off(struct super_block *sb, unsigned long blocknr,
		    unsigned short btype)
{
	return (u64)blocknr << PAGE_SHIFT;
}

static inline int bytefs_get_cpuid(struct super_block *sb)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	return smp_processor_id() % sbi->cpus;
}

static inline u64 bytefs_get_epoch_id(struct super_block *sb)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	return sbi->s_epoch_id;
}

static inline void bytefs_print_curr_epoch_id(struct super_block *sb)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	u64 ret;

	ret = sbi->s_epoch_id;
	bytefs_dbg("Current epoch id: %llu\n", ret);
}

#include "inode.h"
static inline int bytefs_get_head_tail(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode_info_header *sih)
{
	struct bytefs_inode fake_pi;
	int rc;

	struct bytefs_inode *pi_c;
	// BYTEFS_CACHE_BYTE_ISSUE(pi, pi_c, BYTEFS_INODE_ISSUE);

	// BYTEFS_DECACHE_END_BYTE_ISSUE(pi, pi_c);

	rc = memcpy_mcsafe(&fake_pi, pi, sizeof(struct bytefs_inode));
	if (rc)
		return rc;

	sih->i_blk_type = fake_pi.i_blk_type;
	sih->log_head = fake_pi.log_head;
	sih->log_tail = fake_pi.log_tail;
	sih->alter_log_head = fake_pi.alter_log_head;
	sih->alter_log_tail = fake_pi.alter_log_tail;

	return rc;
}

struct bytefs_range_node_lowhigh {
	__le64 range_low;
	__le64 range_high;
};

#define	RANGENODE_PER_PAGE	254

/* A node in the RB tree representing a range of pages */
struct bytefs_range_node {
	struct rb_node node;
	struct vm_area_struct *vma;
	unsigned long mmap_entry;
	union {
		/* Block, inode */
		struct {
			unsigned long range_low;
			unsigned long range_high;
		};
		/* Dir node */
		struct {
			unsigned long hash;
			void *direntry;
		};
	};
	u32	csum;		/* Protect vma, range low/high */
};

struct vma_item {
	/* Reuse header of bytefs_range_node struct */
	struct rb_node node;
	struct vm_area_struct *vma;
	unsigned long mmap_entry;
};

static inline u32 bytefs_calculate_range_node_csum(struct bytefs_range_node *node)
{
	u32 crc;

	crc = bytefs_crc32c(~0, (__u8 *)&node->vma,
			(unsigned long)&node->csum - (unsigned long)&node->vma);

	return crc;
}

static inline int bytefs_update_range_node_checksum(struct bytefs_range_node *node)
{
	if (dram_struct_csum)
		node->csum = bytefs_calculate_range_node_csum(node);

	return 0;
}

static inline bool bytefs_range_node_checksum_ok(struct bytefs_range_node *node)
{
	bool ret;

	if (dram_struct_csum == 0)
		return true;

	ret = node->csum == bytefs_calculate_range_node_csum(node);
	if (!ret) {
		bytefs_dbg("%s: checksum failure, vma %p, range low %lu, range high %lu, csum 0x%x\n",
			 __func__, node->vma, node->range_low, node->range_high,
			 node->csum);
	}

	return ret;
}


enum bm_type {
	BM_4K = 0,
	BM_2M,
	BM_1G,
};

struct single_scan_bm {
	unsigned long bitmap_size;
	unsigned long *bitmap;
};

struct scan_bitmap {
	struct single_scan_bm scan_bm_4K;
	struct single_scan_bm scan_bm_2M;
	struct single_scan_bm scan_bm_1G;
};



struct inode_map {
	struct mutex		inode_table_mutex;
	struct rb_root		inode_inuse_tree;
	unsigned long		num_range_node_inode;
	struct bytefs_range_node *first_inode_range;
	int			allocated;
	int			freed;
};







/* Old entry is freeable if it is appended after the latest snapshot */
static inline int old_entry_freeable(struct super_block *sb, u64 epoch_id)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	if (epoch_id == sbi->s_epoch_id)
		return 1;

	return 0;
}

static inline int pass_mount_snapshot(struct super_block *sb, u64 epoch_id)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	if (epoch_id > sbi->mount_snapshot_epoch_id)
		return 1;

	return 0;
}


// BKDR String Hash Function
static inline unsigned long BKDRHash(const char *str, int length)
{
	unsigned int seed = 131; // 31 131 1313 13131 131313 etc..
	unsigned long hash = 0;
	int i;

	for (i = 0; i < length; i++)
		hash = hash * seed + (*str++);

	return hash;
}


#include "mprotect.h"

#include "log.h"

static inline struct bytefs_file_write_entry *
bytefs_get_write_entry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long blocknr)
{
	struct bytefs_file_write_entry *entry;

	entry = radix_tree_lookup(&sih->tree, blocknr);

	return entry;
}


/*
 * Find data at a file offset (pgoff) in the data pointed to by a write log
 * entry.
 */
static inline unsigned long get_nvmm(struct super_block *sb,
	struct bytefs_inode_info_header *sih,
	struct bytefs_file_write_entry *entry, unsigned long pgoff)
{
	/* entry is already verified before this call and resides in dram
	 * or we can do memcpy_mcsafe here but have to avoid double copy and
	 * verification of the entry.
	 */

	// Note : entry is in DRAM

	if (entry->pgoff > pgoff || (unsigned long) entry->pgoff +
			(unsigned long) entry->num_pages <= pgoff) {
		struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
		u64 curr;

		curr = bytefs_get_addr_off(sbi, entry);
		bytefs_dbg("Entry ERROR: inode %lu, curr 0x%llx, pgoff %lu, entry pgoff %llu, num %u\n",
			sih->ino,
			curr, pgoff, entry->pgoff, entry->num_pages);
		bytefs_print_bytefs_log_pages(sb, sih);
		bytefs_print_bytefs_log(sb, sih);
		BYTEFS_ASSERT(0);
	}

	return (unsigned long) (entry->block >> PAGE_SHIFT) + pgoff
		- entry->pgoff;
}

bool bytefs_verify_entry_csum(struct super_block *sb, void *entry, void *entryc);

static inline u64 bytefs_find_nvmm_block(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	unsigned long blocknr)
{
	unsigned long nvmm;
	struct bytefs_file_write_entry *entryc, entry_copy;

	if (!entry) {
		entry = bytefs_get_write_entry(sb, sih, blocknr);
		if (!entry)
			return 0;
	}

	

	/* Don't check entry here as someone else may be modifying it
	 * when called from reset_vma_csum_parity
	 */

	// entry in NVMM : haor2
	entryc = &entry_copy;
	if (memcpy_mcsafe(entryc, entry,
			sizeof(struct bytefs_file_write_entry)) < 0)
		return 0;

	nvmm = get_nvmm(sb, sih, entryc, blocknr);
	return nvmm << PAGE_SHIFT;
}



static inline unsigned long
bytefs_get_numblocks(unsigned short btype)
{
	unsigned long num_blocks;

	if (btype == BYTEFS_BLOCK_TYPE_4K) {
		num_blocks = 1;
	} else if (btype == BYTEFS_BLOCK_TYPE_2M) {
		num_blocks = 512;
	} else {
		//btype == BYTEFS_BLOCK_TYPE_1G
		num_blocks = 0x40000;
	}
	return num_blocks;
}

static inline unsigned long
bytefs_get_blocknr(struct super_block *sb, u64 block, unsigned short btype)
{
	return block >> PAGE_SHIFT;
}

static inline unsigned long bytefs_get_pfn(struct super_block *sb, u64 block)
{
	return (BYTEFS_SB(sb)->phys_addr + block) >> PAGE_SHIFT;
}

static inline u64 next_log_page(struct super_block *sb, u64 curr)
{
	struct bytefs_inode_log_page *curr_page;
	u64 next = 0;
	int rc;

	curr = BLOCK_OFF(curr);
	curr_page = (struct bytefs_inode_log_page *)bytefs_get_block(sb, curr);
	
	u64* sim_ptr;
	// BYTEFS_CACHE_BYTE_ISSUE(&curr_page->page_tail.next_page, sim_ptr, BYTEFS_LOG_ISSUE);
	// BYTEFS_DECACHE_END_BYTE_ISSUE(&curr_page->page_tail.next_page, sim_ptr);

	rc = memcpy_mcsafe(&next, &curr_page->page_tail.next_page,
				sizeof(u64));
	if (rc)
		return rc;

	return next;
}

static inline u64 alter_log_page(struct super_block *sb, u64 curr)
{
	struct bytefs_inode_log_page *curr_page;
	u64 next = 0;
	int rc;

	if (metadata_csum == 0)
		return 0;

	curr = BLOCK_OFF(curr);
	curr_page = (struct bytefs_inode_log_page *)bytefs_get_block(sb, curr);

	u64* sim_ptr;
	// BYTEFS_CACHE_BYTE_ISSUE(&curr_page->page_tail.alter_page, sim_ptr, BYTEFS_LOG_ISSUE);
	// BYTEFS_DECACHE_END_BYTE_ISSUE(&curr_page->page_tail.alter_page, sim_ptr);

	rc = memcpy_mcsafe(&next, &curr_page->page_tail.alter_page,
				sizeof(u64));
	if (rc)
		return rc;

	return next;
}

#if 0
static inline u64 next_log_page(struct super_block *sb, u64 curr_p)
{
	void *curr_addr = bytefs_get_block(sb, curr_p);
	unsigned long page_tail = BLOCK_OFF((unsigned long)curr_addr)
					+ LOG_BLOCK_TAIL;
	return ((struct bytefs_inode_page_tail *)page_tail)->next_page;
}

static inline u64 alter_log_page(struct super_block *sb, u64 curr_p)
{
	void *curr_addr = bytefs_get_block(sb, curr_p);
	unsigned long page_tail = BLOCK_OFF((unsigned long)curr_addr)
					+ LOG_BLOCK_TAIL;
	if (metadata_csum == 0)
		return 0;

	return ((struct bytefs_inode_page_tail *)page_tail)->alter_page;
}
#endif

static inline u64 alter_log_entry(struct super_block *sb, u64 curr_p)
{
	u64 alter_page;
	void *curr_addr = bytefs_get_block(sb, curr_p);
	unsigned long page_tail = BLOCK_OFF((unsigned long)curr_addr)
					+ LOG_BLOCK_TAIL;
	if (metadata_csum == 0)
		return 0;

	alter_page = ((struct bytefs_inode_page_tail *)page_tail)->alter_page;
	return alter_page + ENTRY_LOC(curr_p);
}

static inline void bytefs_set_next_page_flag(struct super_block *sb, u64 curr_p)
{
	void *p;

	if (ENTRY_LOC(curr_p) >= LOG_BLOCK_TAIL)
		return;

	p = bytefs_get_block(sb, curr_p);
	bytefs_set_entry_type(p, NEXT_PAGE);
	bytefs_flush_buffer(p, CACHELINE_SIZE, 1);
}

static inline void bytefs_set_next_page_address(struct super_block *sb,
	struct bytefs_inode_log_page *curr_page, u64 next_page, int fence)
{
	curr_page->page_tail.next_page = next_page;
	bytefs_flush_buffer(&curr_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);
	if (fence)
		PERSISTENT_BARRIER();
}

static inline void bytefs_set_page_num_entries(struct super_block *sb,
	struct bytefs_inode_log_page *curr_page, int num, int flush)
{
	curr_page->page_tail.num_entries = num;
	if (flush)
		bytefs_flush_buffer(&curr_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);
}

static inline void bytefs_set_page_invalid_entries(struct super_block *sb,
	struct bytefs_inode_log_page *curr_page, int num, int flush)
{
	curr_page->page_tail.invalid_entries = num;
	if (flush)
		bytefs_flush_buffer(&curr_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);
}

static inline void bytefs_inc_page_num_entries(struct super_block *sb,
	u64 curr)
{
	struct bytefs_inode_log_page *curr_page;

	curr = BLOCK_OFF(curr);
	curr_page = (struct bytefs_inode_log_page *)bytefs_get_block(sb, curr);

	curr_page->page_tail.num_entries++;
	bytefs_flush_buffer(&curr_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);
}

u64 bytefs_print_log_entry(struct super_block *sb, u64 curr);

static inline void bytefs_inc_page_invalid_entries(struct super_block *sb,
	u64 curr)
{
	struct bytefs_inode_log_page *curr_page;
	u64 old_curr = curr;

	curr = BLOCK_OFF(curr);
	curr_page = (struct bytefs_inode_log_page *)bytefs_get_block(sb, curr);

	curr_page->page_tail.invalid_entries++;
	if (curr_page->page_tail.invalid_entries >
			curr_page->page_tail.num_entries) {
		bytefs_dbg("Page 0x%llx has %u entries, %u invalid\n",
				curr,
				curr_page->page_tail.num_entries,
				curr_page->page_tail.invalid_entries);
		bytefs_print_log_entry(sb, old_curr);
	}

	bytefs_flush_buffer(&curr_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);
}

static inline void bytefs_set_alter_page_address(struct super_block *sb,
	u64 curr, u64 alter_curr)
{
	struct bytefs_inode_log_page *curr_page;
	struct bytefs_inode_log_page *alter_page;

	if (metadata_csum == 0)
		return;

	curr_page = bytefs_get_block(sb, BLOCK_OFF(curr));
	alter_page = bytefs_get_block(sb, BLOCK_OFF(alter_curr));

	curr_page->page_tail.alter_page = alter_curr;
	bytefs_flush_buffer(&curr_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);

	alter_page->page_tail.alter_page = curr;
	bytefs_flush_buffer(&alter_page->page_tail,
				sizeof(struct bytefs_inode_page_tail), 0);
}

#define	CACHE_ALIGN(p)	((p) & ~(CACHELINE_SIZE - 1))

static inline bool is_last_entry(u64 curr_p, size_t size)
{
	unsigned int entry_end;

	entry_end = ENTRY_LOC(curr_p) + size;

	return entry_end > LOG_BLOCK_TAIL;
}

static inline bool goto_next_page(struct super_block *sb, u64 curr_p)
{
	void *addr;
	u8 type;
	int rc;

	/* Each kind of entry takes at least 32 bytes */
	if (ENTRY_LOC(curr_p) + 32 > LOG_BLOCK_TAIL)
		return true;

	addr = bytefs_get_block(sb, curr_p);

	u8* sim_ptr;
	// BYTEFS_CACHE_BYTE_ISSUE(addr, sim_ptr, BYTEFS_LOG_ISSUE);
	// BYTEFS_DECACHE_END_BYTE_ISSUE(addr, sim_ptr);

	rc = memcpy_mcsafe(&type, addr, sizeof(u8));

	if (rc < 0)
		return true;

	if (type == NEXT_PAGE)
		return true;

	return false;
}

static inline int is_dir_init_entry(struct super_block *sb,
	struct bytefs_dentry *entry)
{
	if (entry->name_len == 1 && strncmp(entry->name, ".", 1) == 0)
		return 1;
	if (entry->name_len == 2 && strncmp(entry->name, "..", 2) == 0)
		return 1;

	return 0;
}

#include "balloc.h" // remove once we move the following functions away

/* Checksum methods */
static inline void *bytefs_get_data_csum_addr(struct super_block *sb, u64 strp_nr,
	int replica)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	struct free_list *free_list;
	unsigned long blocknr;
	void *data_csum_addr;
	u64 blockoff;
	int index;
	int BLOCK_SHIFT = PAGE_SHIFT - BYTEFS_STRIPE_SHIFT;

	if (!data_csum) {
		bytefs_dbg("%s: Data checksum is disabled!\n", __func__);
		return NULL;
	}

	blocknr = strp_nr >> BLOCK_SHIFT;
	index = blocknr / sbi->per_list_blocks;

	if (index >= sbi->cpus) {
		bytefs_dbg("%s: Invalid blocknr %lu\n", __func__, blocknr);
		return NULL;
	}

	strp_nr -= (index * sbi->per_list_blocks) << BLOCK_SHIFT;
	free_list = bytefs_get_free_list(sb, index);
	if (replica == 0)
		blockoff = free_list->csum_start << PAGE_SHIFT;
	else
		blockoff = free_list->replica_csum_start << PAGE_SHIFT;

	/* Range test */
	if (((BYTEFS_DATA_CSUM_LEN * strp_nr) >> PAGE_SHIFT) >=
			free_list->num_csum_blocks) {
		bytefs_dbg("%s: Invalid strp number %llu, free list %d\n",
				__func__, strp_nr, free_list->index);
		return NULL;
	}

	data_csum_addr = (u8 *) bytefs_get_block(sb, blockoff)
				+ BYTEFS_DATA_CSUM_LEN * strp_nr;

	return data_csum_addr;
}

static inline void *bytefs_get_parity_addr(struct super_block *sb,
	unsigned long blocknr)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	struct free_list *free_list;
	void *data_csum_addr;
	u64 blockoff;
	int index;
	int BLOCK_SHIFT = PAGE_SHIFT - BYTEFS_STRIPE_SHIFT;

	if (data_parity == 0) {
		bytefs_dbg("%s: Data parity is disabled!\n", __func__);
		return NULL;
	}

	index = blocknr / sbi->per_list_blocks;

	if (index >= sbi->cpus) {
		bytefs_dbg("%s: Invalid blocknr %lu\n", __func__, blocknr);
		return NULL;
	}

	free_list = bytefs_get_free_list(sb, index);
	blockoff = free_list->parity_start << PAGE_SHIFT;

	/* Range test */
	if (((blocknr - free_list->block_start) >> BLOCK_SHIFT) >=
			free_list->num_parity_blocks) {
		bytefs_dbg("%s: Invalid blocknr %lu, free list %d\n",
				__func__, blocknr, free_list->index);
		return NULL;
	}

	data_csum_addr = (u8 *) bytefs_get_block(sb, blockoff) +
				((blocknr - free_list->block_start)
				 << BYTEFS_STRIPE_SHIFT);

	return data_csum_addr;
}

/* Function Prototypes */



/* bbuild.c */
inline void set_bm(unsigned long bit, struct scan_bitmap *bm,
	enum bm_type type);
void bytefs_save_blocknode_mappings_to_log(struct super_block *sb);
void bytefs_save_inode_list_to_log(struct super_block *sb);
void bytefs_init_header(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u16 i_mode);
int bytefs_recovery(struct super_block *sb);

/* checksum.c */
void bytefs_update_entry_csum(void *entry);
int bytefs_update_block_csum(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u8 *block, unsigned long blocknr,
	size_t offset, size_t bytes, int zero);
int bytefs_update_alter_entry(struct super_block *sb, void *entry);
int bytefs_check_inode_integrity(struct super_block *sb, u64 ino, u64 pi_addr,
	u64 alter_pi_addr, struct bytefs_inode *pic, int check_replica);
int bytefs_update_pgoff_csum(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	unsigned long pgoff, int zero);
bool bytefs_verify_data_csum(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long blocknr,
	size_t offset, size_t bytes);
int bytefs_update_truncated_block_csum(struct super_block *sb,
	struct inode *inode, loff_t newsize);

/*
 * Inodes and files operations
 */

/* dax.c */
int bytefs_cleanup_incomplete_write(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long blocknr,
	int allocated, u64 begin_tail, u64 end_tail);
void bytefs_init_file_write_entry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	u64 epoch_id, u64 pgoff, int num_pages, u64 blocknr, u32 time,
	u64 size);
int bytefs_reassign_file_tree(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u64 begin_tail);
unsigned long bytefs_check_existing_entry(struct super_block *sb,
	struct inode *inode, unsigned long num_blocks, unsigned long start_blk,
	struct bytefs_file_write_entry **ret_entry,
	struct bytefs_file_write_entry *ret_entryc, int check_next, u64 epoch_id,
	int *inplace, int locked);
int bytefs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
	unsigned int flags, struct iomap *iomap, bool taking_lock);
int bytefs_iomap_end(struct inode *inode, loff_t offset, loff_t length,
	ssize_t written, unsigned int flags, struct iomap *iomap);
int bytefs_insert_write_vma(struct vm_area_struct *vma);

int bytefs_check_overlap_vmas(struct super_block *sb,
			    struct bytefs_inode_info_header *sih,
			    unsigned long pgoff, unsigned long num_pages);
int bytefs_handle_head_tail_blocks(struct super_block *sb,
				 struct inode *inode, loff_t pos,
				 size_t count, void *kmem);
int bytefs_protect_file_data(struct super_block *sb, struct inode *inode,
	loff_t pos, size_t count, const char __user *buf, unsigned long blocknr,
	bool inplace);
ssize_t bytefs_inplace_file_write(struct file *filp, const char __user *buf,
				size_t len, loff_t *ppos);
ssize_t do_bytefs_inplace_file_write(struct file *filp, const char __user *buf,
				   size_t len, loff_t *ppos);

extern const struct vm_operations_struct bytefs_dax_vm_ops;


/* dir.c */
extern const struct file_operations bytefs_dir_operations;
int bytefs_insert_dir_tree(struct super_block *sb,
	struct bytefs_inode_info_header *sih, const char *name,
	int namelen, struct bytefs_dentry *direntry);
int bytefs_remove_dir_tree(struct super_block *sb,
	struct bytefs_inode_info_header *sih, const char *name, int namelen,
	int replay, struct bytefs_dentry **create_dentry);
int bytefs_append_dentry(struct super_block *sb, struct bytefs_inode *pi,
	struct inode *dir, struct dentry *dentry, u64 ino,
	unsigned short de_len, struct bytefs_inode_update *update,
	int link_change, u64 epoch_id);
int bytefs_append_dir_init_entries(struct super_block *sb,
	struct bytefs_inode *pi, u64 self_ino, u64 parent_ino, u64 epoch_id);
int bytefs_add_dentry(struct dentry *dentry, u64 ino, int inc_link,
	struct bytefs_inode_update *update, u64 epoch_id);
int bytefs_remove_dentry(struct dentry *dentry, int dec_link,
	struct bytefs_inode_update *update, u64 epoch_id, bool rename);
int bytefs_invalidate_dentries(struct super_block *sb,
	struct bytefs_inode_update *update);
void bytefs_print_dir_tree(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long ino);
void bytefs_delete_dir_tree(struct super_block *sb,
	struct bytefs_inode_info_header *sih);
struct bytefs_dentry *bytefs_find_dentry(struct super_block *sb,
	struct bytefs_inode *pi, struct inode *inode, const char *name,
	unsigned long name_len);

/* file.c */
extern const struct inode_operations bytefs_file_inode_operations;
extern const struct file_operations bytefs_dax_file_operations;
extern const struct file_operations bytefs_wrap_file_operations;


/* gc.c */
int bytefs_inode_log_fast_gc(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode_info_header *sih,
	u64 curr_tail, u64 new_block, u64 alter_new_block, int num_pages,
	int force_thorough);

/* ioctl.c */
extern long bytefs_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);
#ifdef CONFIG_COMPAT
extern long bytefs_compat_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg);
#endif



/* mprotect.c */
extern int bytefs_dax_mem_protect(struct super_block *sb,
				 void *vaddr, unsigned long size, int rw);
int bytefs_get_vma_overlap_range(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct vm_area_struct *vma,
	unsigned long entry_pgoff, unsigned long entry_pages,
	unsigned long *start_pgoff, unsigned long *num_pages);
int bytefs_mmap_to_new_blocks(struct vm_area_struct *vma,
	unsigned long address);
bool bytefs_find_pgoff_in_vma(struct inode *inode, unsigned long pgoff);
int bytefs_set_vmas_readonly(struct super_block *sb);

/* namei.c */
extern const struct inode_operations bytefs_dir_inode_operations;
extern const struct inode_operations bytefs_special_inode_operations;
extern struct dentry *bytefs_get_parent(struct dentry *child);

/* parity.c */
int bytefs_update_pgoff_parity(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	unsigned long pgoff, int zero);
int bytefs_update_block_csum_parity(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u8 *block, unsigned long blocknr,
	size_t offset, size_t bytes);
int bytefs_restore_data(struct super_block *sb, unsigned long blocknr,
	unsigned int badstrip_id, void *badstrip, int nvmmerr, u32 csum0,
	u32 csum1, u32 *csum_good);
int bytefs_update_truncated_block_parity(struct super_block *sb,
	struct inode *inode, loff_t newsize);

/* rebuild.c */
int bytefs_reset_csum_parity_range(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	unsigned long start_pgoff, unsigned long end_pgoff, int zero,
	int check_entry);
int bytefs_reset_mapping_csum_parity(struct super_block *sb,
	struct inode *inode, struct address_space *mapping,
	unsigned long start_pgoff, unsigned long end_pgoff);
int bytefs_reset_vma_csum_parity(struct super_block *sb,
	struct vma_item *item);
int bytefs_rebuild_dir_inode_tree(struct super_block *sb,
	struct bytefs_inode *pi, u64 pi_addr,
	struct bytefs_inode_info_header *sih);
int bytefs_rebuild_inode(struct super_block *sb, struct bytefs_inode_info *si,
	u64 ino, u64 pi_addr, int rebuild_dir);
int bytefs_restore_snapshot_table(struct super_block *sb, int just_init);

/* snapshot.c */
// int bytefs_encounter_mount_snapshot(struct super_block *sb, void *addr,
// 	u8 type);
// int bytefs_save_snapshots(struct super_block *sb);
// int bytefs_destroy_snapshot_infos(struct super_block *sb);
// int bytefs_restore_snapshot_entry(struct super_block *sb,
// 	struct bytefs_snapshot_info_entry *entry, u64 curr_p, int just_init);
// int bytefs_mount_snapshot(struct super_block *sb);
// int bytefs_append_data_to_snapshot(struct super_block *sb,
// 	struct bytefs_file_write_entry *entry, u64 nvmm, u64 num_pages,
// 	u64 delete_epoch_id);
// int bytefs_append_inode_to_snapshot(struct super_block *sb,
// 	struct bytefs_inode *pi);
// int bytefs_print_snapshots(struct super_block *sb, struct seq_file *seq);
// int bytefs_print_snapshot_lists(struct super_block *sb, struct seq_file *seq);
// int bytefs_delete_dead_inode(struct super_block *sb, u64 ino);
// int bytefs_create_snapshot(struct super_block *sb);
// int bytefs_delete_snapshot(struct super_block *sb, u64 epoch_id);
// int bytefs_snapshot_init(struct super_block *sb);


/* symlink.c */
int bytefs_block_symlink(struct super_block *sb, struct bytefs_inode *pi,
	struct inode *inode, const char *symname, int len, u64 epoch_id);
extern const struct inode_operations bytefs_symlink_inode_operations;

/* sysfs.c */
// extern const char *proc_dirname;
// extern struct proc_dir_entry *bytefs_proc_root;
// void bytefs_sysfs_init(struct super_block *sb);
// void bytefs_sysfs_exit(struct super_block *sb);

/* bytefs_stats.c */
// void bytefs_get_timing_stats(void);
// void bytefs_get_IO_stats(void);
// void bytefs_print_timing_stats(struct super_block *sb);
// void bytefs_clear_stats(struct super_block *sb);
// void bytefs_print_inode(struct bytefs_inode *pi);
// void bytefs_print_inode_log(struct super_block *sb, struct inode *inode);
// void bytefs_print_inode_log_pages(struct super_block *sb, struct inode *inode);
// int bytefs_check_inode_logs(struct super_block *sb, struct bytefs_inode *pi);
// void bytefs_print_free_lists(struct super_block *sb);

// remove
/* perf.c */
// int bytefs_test_perf(struct super_block *sb, unsigned int func_id,
// 	unsigned int poolmb, size_t size, unsigned int disks);

#endif /* __BYTEFS_H */
