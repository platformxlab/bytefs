/*
 * BRIEF DESCRIPTION
 *
 * Memory protection definitions for the BYTEFS filesystem.
 *
 * Copyright 2015-2016 Regents of the University of California,
 * UCSD Non-Volatile Systems Lab, Andiry Xu <jix024@cs.ucsd.edu>
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 *
 * This program is free software; you can redistribute it and/or modify it
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#ifndef __WPROTECT_H
#define __WPROTECT_H

#include <linux/fs.h>
#include "bytefs_def.h"
#include "super.h"

#include "ssd/bytefs_issue.h"

extern void bytefs_error_mng(struct super_block *sb, const char *fmt, ...);

static inline int bytefs_range_check(struct super_block *sb, void *p,
					 unsigned long len)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	if (p < sbi->virt_addr ||
			p + len > sbi->virt_addr + sbi->initsize) {
		bytefs_err(sb, "access pmem out of range: pmem range 0x%lx - 0x%lx, "
				"access range 0x%lx - 0x%lx\n",
				(unsigned long)sbi->virt_addr,
				(unsigned long)(sbi->virt_addr + sbi->initsize),
				(unsigned long)p, (unsigned long)(p + len));
		dump_stack();
		return -EINVAL;
	}

	return 0;
}

static inline void wprotect_disable(void)
{
	unsigned long cr0_val;

	cr0_val = read_cr0();
	cr0_val &= (~X86_CR0_WP);
	write_cr0(cr0_val);
}

static inline void wprotect_enable(void)
{
	unsigned long cr0_val;

	cr0_val = read_cr0();
	cr0_val |= X86_CR0_WP;
	write_cr0(cr0_val);
}

/* FIXME: Assumes that we are always called in the right order.
 * bytefs_writeable(vaddr, size, 1);
 * bytefs_writeable(vaddr, size, 0);
 */
static inline int
bytefs_writeable(void *vaddr, unsigned long size, int rw, unsigned long *flags)
{
	INIT_TIMING(wprotect_time);

	BYTEFS_START_TIMING(wprotect_t, wprotect_time);
	if (rw) {
		local_irq_save(*flags);
		wprotect_disable();
	} else {
		wprotect_enable();
		local_irq_restore(*flags);
	}
	BYTEFS_END_TIMING(wprotect_t, wprotect_time);
	return 0;
}

static inline int bytefs_is_protected(struct super_block *sb)
{
	struct bytefs_sb_info *sbi = (struct bytefs_sb_info *)sb->s_fs_info;

	if (wprotect)
		return wprotect;
	return sbi->s_mount_opt & BYTEFS_MOUNT_PROTECT;
}

static inline int bytefs_is_wprotected(struct super_block *sb)
{
	return bytefs_is_protected(sb);
}

static inline void
__bytefs_memunlock_range(void *p, unsigned long len, unsigned long *flags)
{
	/*
	 * NOTE: Ideally we should lock all the kernel to be memory safe
	 * and avoid to write in the protected memory,
	 * obviously it's not possible, so we only serialize
	 * the operations at fs level. We can't disable the interrupts
	 * because we could have a deadlock in this path.
	 */
	bytefs_writeable(p, len, 1, flags);
	
	void* cache_ptr;
	// haor2
	// still, same as PMFS, we need to read verify write in order to prevent hole.
	// Adding overhead over whole struct prevents issuing too many times. So this is current strategy to add overhead.
	// This is aligned with PMFS 
	// BYTEFS_CACHE_BYTE_ISSUE_LEN(p, cache_ptr, len, BYTEFS_ISSUE_ALL);
	// BYTEFS_DECACHE_FLUSH_BYTE_ISSUE_LEN(p, cache_ptr, len, BYTEFS_ISSUE_ALL);
}

static inline void
bytefs_write(void *p, unsigned long len){
	void* cache_ptr;

	if(len < (4<<20)){
		cache_ptr = kmalloc(len, GFP_KERNEL);
	}else{
		cache_ptr = vmalloc(len);
	}
	BYTEFS_DECACHE_FLUSH_BYTE_ISSUE_LEN(p, cache_ptr, len, BYTEFS_ISSUE_ALL);
}

static inline void
__bytefs_memlock_range(void *p, unsigned long len, unsigned long *flags)
{
	bytefs_writeable(p, len, 0, flags);
}

static inline void bytefs_memunlock_range(struct super_block *sb, void *p,
					 unsigned long len, unsigned long *flags)
{
	// printk(KERN_ERR "hi");
	if (bytefs_range_check(sb, p, len))
		return;

	if (bytefs_is_protected(sb))
		__bytefs_memunlock_range(p, len, flags);
}

static inline void bytefs_memlock_range(struct super_block *sb, void *p,
				       unsigned long len, unsigned long *flags)
{
	if (bytefs_is_protected(sb))
		__bytefs_memlock_range(p, len, flags);
}

static inline void bytefs_memunlock_super(struct super_block *sb, unsigned long *flags)
{
	struct bytefs_super_block *ps = bytefs_get_super(sb);

	if (bytefs_is_protected(sb))
		__bytefs_memunlock_range(ps, BYTEFS_SB_SIZE, flags);
}

static inline void bytefs_memlock_super(struct super_block *sb, unsigned long *flags)
{
	struct bytefs_super_block *ps = bytefs_get_super(sb);

	if (bytefs_is_protected(sb))
		__bytefs_memlock_range(ps, BYTEFS_SB_SIZE, flags);
}

static inline void bytefs_memunlock_reserved(struct super_block *sb,
					 struct bytefs_super_block *ps, unsigned long *flags)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	if (bytefs_is_protected(sb))
		__bytefs_memunlock_range(ps,
			sbi->head_reserved_blocks * BYTEFS_DEF_BLOCK_SIZE_4K, flags);
}

static inline void bytefs_memlock_reserved(struct super_block *sb,
				       struct bytefs_super_block *ps, unsigned long *flags)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	if (bytefs_is_protected(sb))
		__bytefs_memlock_range(ps,
			sbi->head_reserved_blocks * BYTEFS_DEF_BLOCK_SIZE_4K, flags);
}

static inline void bytefs_memunlock_journal(struct super_block *sb, unsigned long *flags)
{
	void *addr = bytefs_get_block(sb, BYTEFS_DEF_BLOCK_SIZE_4K * JOURNAL_START);

	if (bytefs_range_check(sb, addr, BYTEFS_DEF_BLOCK_SIZE_4K))
		return;

	if (bytefs_is_protected(sb))
		__bytefs_memunlock_range(addr, BYTEFS_DEF_BLOCK_SIZE_4K, flags);
}

static inline void bytefs_memlock_journal(struct super_block *sb, unsigned long *flags)
{
	void *addr = bytefs_get_block(sb, BYTEFS_DEF_BLOCK_SIZE_4K * JOURNAL_START);

	if (bytefs_is_protected(sb))
		__bytefs_memlock_range(addr, BYTEFS_DEF_BLOCK_SIZE_4K, flags);
}

static inline void bytefs_memunlock_inode(struct super_block *sb,
					 struct bytefs_inode *pi, unsigned long *flags)
{
	if (bytefs_range_check(sb, pi, BYTEFS_INODE_SIZE))
		return;

	if (bytefs_is_protected(sb))
		__bytefs_memunlock_range(pi, BYTEFS_INODE_SIZE, flags);
}

static inline void bytefs_memlock_inode(struct super_block *sb,
				       struct bytefs_inode *pi, unsigned long *flags)
{
	/* bytefs_sync_inode(pi); */
	if (bytefs_is_protected(sb))
		__bytefs_memlock_range(pi, BYTEFS_INODE_SIZE, flags);
}

static inline void bytefs_memunlock_block(struct super_block *sb, void *bp, unsigned long *flags)
{
	if (bytefs_range_check(sb, bp, sb->s_blocksize))
		return;

	if (bytefs_is_protected(sb))
		__bytefs_memunlock_range(bp, sb->s_blocksize, flags);
}

static inline void bytefs_memlock_block(struct super_block *sb, void *bp, unsigned long *flags)
{
	if (bytefs_is_protected(sb))
		__bytefs_memlock_range(bp, sb->s_blocksize, flags);
}


#endif
