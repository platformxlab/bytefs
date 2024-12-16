/* SPDX-License-Identifier: GPL-2.0 */
/*
 *	fs/bytefs/bytefs.h
 *	Copyright (C) 1999-2018 Tigran Aivazian <aivazian.tigran@gmail.com>
 */
#if 0

#ifndef _FS_BYTEFS_BYTEFS_H
#define _FS_BYTEFS_BYTEFS_H

#include "bytefs_fs.h"

/* In theory BYTEFS supports up to 512 inodes, numbered from 2 (for /) up to 513 inclusive.
   In actual fact, attempting to create the 512th inode (i.e. inode No. 513 or file No. 511)
   will fail with ENOSPC in bytefs_add_entry(): the root directory cannot contain so many entries, counting '..'.
   So, mkfs.bytefs(8) should really limit its -N option to 511 and not 512. For now, we just print a warning
   if a filesystem is mounted with such "impossible to fill up" number of inodes */
#define BYTEFS_MAX_LASTI	513

/*
 * BYTEFS file system in-core superblock info
 */
struct bytefs_sb_info {
	unsigned long si_blocks;
	unsigned long si_freeb;
	unsigned long si_freei;
	unsigned long si_lf_eblk;
	unsigned long si_lasti;
	DECLARE_BITMAP(si_imap, BYTEFS_MAX_LASTI+1);
	struct mutex bytefs_lock;
};

/*
 * BYTEFS file system in-core inode info
 */
struct bytefs_inode_info {
	unsigned long i_dsk_ino; /* inode number from the disk, can be 0 */
	unsigned long i_sblock;
	unsigned long i_eblock;
	struct inode vfs_inode;
};

static inline struct bytefs_sb_info *BYTEFS_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}

static inline struct bytefs_inode_info *BYTEFS_I(struct inode *inode)
{
	return container_of(inode, struct bytefs_inode_info, vfs_inode);
}


#define printf(format, args...) \
	printk(KERN_ERR "BYTEFS-fs: %s(): " format, __func__, ## args)

/* inode.c */
extern struct inode *bytefs_iget(struct super_block *sb, unsigned long ino);
extern void bytefs_dump_imap(const char *, struct super_block *);

/* file.c */
extern const struct inode_operations bytefs_file_inops;
extern const struct file_operations bytefs_file_operations;
extern const struct address_space_operations bytefs_aops;

/* dir.c */
extern const struct inode_operations bytefs_dir_inops;
extern const struct file_operations bytefs_dir_operations;

/* backend.c*/

#endif /* _FS_BYTEFS_BYTEFS_H */

#endif