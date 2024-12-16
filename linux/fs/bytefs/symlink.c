/*
 * BRIEF DESCRIPTION
 *
 * Symlink operations
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

#include <linux/fs.h>
#include <linux/namei.h>
#include <linux/version.h>
#include "bytefs.h"
#include "inode.h"

int bytefs_block_symlink(struct super_block *sb, struct bytefs_inode *pi,
	struct inode *inode, const char *symname, int len, u64 epoch_id)
{
	struct bytefs_file_write_entry entry_data;
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode_update update;
	unsigned long name_blocknr = 0;
	int allocated;
	u64 block;
	char *blockp;
	u32 time;
	int ret;
	unsigned long irq_flags = 0;

	update.tail = sih->log_tail;
	update.alter_tail = sih->alter_log_tail;

	allocated = bytefs_new_data_blocks(sb, sih, &name_blocknr, 0, 1,
				 ALLOC_INIT_ZERO, ANY_CPU, ALLOC_FROM_TAIL);    //ANCHOR
	if (allocated != 1 || name_blocknr == 0) {
		ret = allocated;
		return ret;
	}

	/* First copy name to name block */
	block = bytefs_get_block_off(sb, name_blocknr, BYTEFS_BLOCK_TYPE_4K);   //ANCHOR
	blockp = (char *)bytefs_get_block(sb, block);  //ANCHOR

	bytefs_memunlock_block(sb, blockp, &irq_flags);
	memcpy_to_pmem_nocache(blockp, symname, len);    //ANCHOR
	blockp[len] = '\0';
	bytefs_memlock_block(sb, blockp, &irq_flags);

	/* Apply a write entry to the log page */
	time = current_time(inode).tv_sec;
	bytefs_init_file_write_entry(sb, sih, &entry_data, epoch_id, 0, 1,
					name_blocknr, time, len + 1);

	ret = bytefs_append_file_write_entry(sb, pi, inode, &entry_data, &update);   //ANCHOR
	if (ret) {
		bytefs_dbg("%s: append file write entry failed %d\n",
					__func__, ret);
		bytefs_free_data_blocks(sb, sih, name_blocknr, 1);   //ANCHOR
		return ret;
	}

	bytefs_memunlock_inode(sb, pi, &irq_flags);
	bytefs_update_inode(sb, inode, pi, &update, 1);
	bytefs_memlock_inode(sb, pi, &irq_flags);
	sih->trans_id++;

	return 0;
}

/* FIXME: Temporary workaround */
static int bytefs_readlink_copy(char __user *buffer, int buflen, const char *link)
{
	int len = PTR_ERR(link);

	if (IS_ERR(link))
		goto out;

	len = strlen(link);
	if (len > (unsigned int) buflen)
		len = buflen;
	if (copy_to_user(buffer, link, len))   //ANCHOR
		len = -EFAULT;
out:
	return len;
}

static int bytefs_readlink(struct dentry *dentry, char __user *buffer, int buflen)
{
	struct bytefs_file_write_entry *entry;
	struct bytefs_file_write_entry *entryc, entry_copy;
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	char *blockp;

	entry = (struct bytefs_file_write_entry *)bytefs_get_block(sb,
							sih->log_head);   //ANCHOR

	if (metadata_csum == 0)
		entryc = entry;
	else {
		entryc = &entry_copy;
		if (!bytefs_verify_entry_csum(sb, entry, entryc))
			return -EIO;
	}

	blockp = (char *)bytefs_get_block(sb, BLOCK_OFF(entryc->block));   //ANCHOR

	return bytefs_readlink_copy(buffer, buflen, blockp);
}

static const char *bytefs_get_link(struct dentry *dentry, struct inode *inode,
	struct delayed_call *done)
{
	struct bytefs_file_write_entry *entry;
	struct bytefs_file_write_entry *entryc, entry_copy;
	struct super_block *sb = inode->i_sb;
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	char *blockp;

	entry = (struct bytefs_file_write_entry *)bytefs_get_block(sb,
							sih->log_head);    //ANCHOR
	if (metadata_csum == 0)
		entryc = entry;
	else {
		entryc = &entry_copy;
		if (!bytefs_verify_entry_csum(sb, entry, entryc))   //ANCHOR
			return NULL;
	}

	blockp = (char *)bytefs_get_block(sb, BLOCK_OFF(entryc->block));   //ANCHOR

	return blockp;
}

const struct inode_operations bytefs_symlink_inode_operations = {
	.readlink	= bytefs_readlink,
	.get_link	= bytefs_get_link,
	.setattr	= bytefs_notify_change,
};
