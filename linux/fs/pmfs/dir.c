/*
 * BRIEF DESCRIPTION
 *
 * File operations for directories.
 *
 * Copyright 2012-2013 Intel Corporation
 * Copyright 2009-2011 Marco Stornelli <marco.stornelli@gmail.com>
 * Copyright 2003 Sony Corporation
 * Copyright 2003 Matsushita Electric Industrial Co., Ltd.
 * 2003-2004 (c) MontaVista Software, Inc. , Steve Longerbeam
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include "pmfs.h"
#include "ssd/pmfs_bytefs.h"
/*
 *	Parent is locked.
 */

#define DT2IF(dt) (((dt) << 12) & S_IFMT)
#define IF2DT(sif) (((sif) & S_IFMT) >> 12)

static int pmfs_add_dirent_to_buf(pmfs_transaction_t *trans,
	struct dentry *dentry, struct inode *inode,
	struct pmfs_direntry *de, u8 *blk_base,  struct pmfs_inode *pidir)
{
	struct inode *dir = dentry->d_parent->d_inode;
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned short reclen;
	int nlen, rlen;
	char *top;
	// cache buffer for pmem 
	struct pmfs_direntry * de_c;
	struct pmfs_inode *pidir_c;

	de_c = kmalloc(sizeof(*de_c), GFP_KERNEL);

	// header length
	reclen = PMFS_DIR_REC_LEN(namelen);
	if (!de) {
		de = (struct pmfs_direntry *)blk_base;
		// if(0 < pmfs_byte_issue(0, page_to_pfn(virt_to_page(de)), pmfs_virt_to_offset(de), sizeof(*de), de_c)) { 
		// 	printk(KERN_ERR "%s : read filed", __func__);
		// }
		// go one by one to see where to insert
		top = blk_base + dir->i_sb->s_blocksize - reclen;
		while ((char *)de <= top) {
#if 0
			if (!pmfs_check_dir_entry("pmfs_add_dirent_to_buf",
			    dir, de, blk_base, offset))
				return -EIO;
			if (pmfs_match(namelen, name, de))
				return -EEXIST;
#endif
			rlen = le16_to_cpu(de->de_len);
			if (de->ino) {
				nlen = PMFS_DIR_REC_LEN(de->name_len);
				if ((rlen - nlen) >= reclen)
					break;
			} else if (rlen >= reclen)
				break;
			de = (struct pmfs_direntry *)((char *)de + rlen);
		}
		if ((char *)de > top){
			kfree(de_c);
			return -ENOSPC;
		}
	}
	// else if(0 < pmfs_byte_issue(0, page_to_pfn(virt_to_page(de)), pmfs_virt_to_offset(de), sizeof(*de), de_c)) { 
	// 	printk(KERN_ERR "%s : read filed", __func__);
	// }
	rlen = le16_to_cpu(de->de_len);

	//@ TODO here is a lot of optimization a lot of write can be avoided


	if (de->ino) {
		struct pmfs_direntry *de1;
	#ifdef ENABLE_JOURNAL
		pmfs_add_logentry(dir->i_sb, trans, &de->de_len,
			sizeof(de->de_len), LE_DATA);
	#endif
		nlen = PMFS_DIR_REC_LEN(de->name_len);
		de1 = (struct pmfs_direntry *)((char *)de + nlen);
		pmfs_memunlock_block(dir->i_sb, blk_base);
		de1->de_len = cpu_to_le16(rlen - nlen);
		de->de_len = cpu_to_le16(nlen);
		pmfs_memlock_block(dir->i_sb, blk_base);
		de = de1;
	} else {
	#ifdef ENABLE_JOURNAL
		pmfs_add_logentry(dir->i_sb, trans, &de->ino,
			sizeof(de->ino), LE_DATA);
	#endif
	}
	pmfs_memunlock_block(dir->i_sb, blk_base);
	/*de->file_type = 0;*/
	if (inode) {
		de->ino = cpu_to_le64(inode->i_ino);
		/*de->file_type = IF2DT(inode->i_mode); */
	} else {
		de->ino = 0;
	}
	de->name_len = namelen;
	memcpy(de->name, name, namelen);
	pmfs_memlock_block(dir->i_sb, blk_base);
	//@ TODO check the alignment here
	pmfs_ssd_write(de,reclen); 
	pmfs_flush_buffer(de, reclen, false);


	// if(0 < pmfs_byte_issue(1, page_to_pfn(virt_to_page(de)), pmfs_virt_to_offset(de), sizeof(*de), de_c)) { 
	// 	printk(KERN_ERR "%s : write filed", __func__);
	// }
	kfree(de_c);


	/*
	 * XXX shouldn't update any times until successful
	 * completion of syscall, but too many callers depend
	 * on this.
	 */
	dir->i_mtime = dir->i_ctime = current_time(dir);
	/*dir->i_version++; */
	pidir_c = kmalloc(sizeof(*pidir_c), GFP_KERNEL);
	// if(0 < pmfs_byte_issue(0, page_to_pfn(virt_to_page(pidir)), pmfs_virt_to_offset(pidir), sizeof(*pidir), pidir_c)) { 
	// 	printk(KERN_ERR "%s : read filed", __func__);
	// }
	pmfs_memunlock_inode(dir->i_sb, pidir);
	pidir->i_mtime = cpu_to_le32(dir->i_mtime.tv_sec);
	pidir->i_ctime = cpu_to_le32(dir->i_ctime.tv_sec);
	pmfs_memlock_inode(dir->i_sb, pidir);
	// @TOD modify access time Should be delayed flush
	pmfs_ssd_write(pidir,64); 
	// if(0 < pmfs_byte_issue(1, page_to_pfn(virt_to_page(pidir)), pmfs_virt_to_offset(pidir), sizeof(*pidir), pidir_c)) { 
	// 	printk(KERN_ERR "%s : write filed", __func__);
	// }
	kfree(pidir_c);
	return 0;
}

/* adds a directory entry pointing to the inode. assumes the inode has
 * already been logged for consistency
 */
int pmfs_add_entry(pmfs_transaction_t *trans, struct dentry *dentry,
		struct inode *inode)
{
	struct inode *dir = dentry->d_parent->d_inode;
	struct super_block *sb = dir->i_sb;
	int retval = -EINVAL;
	unsigned long block, blocks;
	struct pmfs_direntry *de;
	char *blk_base;
	struct pmfs_inode *pidir;

	struct pmfs_direntry *de_c;




	if (!dentry->d_name.len)
		return -EINVAL;

	pidir = pmfs_get_inode(sb, dir->i_ino);

#ifdef ENABLE_JOURNAL
	pmfs_add_logentry(sb, trans, pidir, MAX_DATA_PER_LENTRY, LE_DATA);
#endif
	blocks = dir->i_size >> sb->s_blocksize_bits;
	for (block = 0; block < blocks; block++) {
		blk_base =
			pmfs_get_block(sb, pmfs_find_data_block(dir, block));
		if (!blk_base) {
			retval = -EIO;
			goto out;
		}
		retval = pmfs_add_dirent_to_buf(trans, dentry, inode,
				NULL, blk_base, pidir);
		if (retval != -ENOSPC)
			goto out;
	}
	// if no free entry found, allocate a new block
	retval = pmfs_alloc_blocks(trans, dir, blocks, 1, false);
	if (retval)
		goto out;

	dir->i_size += dir->i_sb->s_blocksize;
	pmfs_update_isize(dir, pidir);

	blk_base = pmfs_get_block(sb, pmfs_find_data_block(dir, blocks));
	if (!blk_base) {
		retval = -ENOSPC;
		goto out;
	}
	/* No need to log the changes to this de because its a new block */
	de = (struct pmfs_direntry *)blk_base;
	de_c = kmalloc(sizeof(*de_c), GFP_KERNEL);
	// if(0 < pmfs_byte_issue(0, page_to_pfn(virt_to_page(de)), pmfs_virt_to_offset(de), sizeof(*de), de_c)) { 
	// 	printk(KERN_ERR "%s : read filed", __func__);
	// }

	pmfs_memunlock_block(sb, blk_base);
	de->ino = 0;
	de->de_len = cpu_to_le16(sb->s_blocksize);
	pmfs_memlock_block(sb, blk_base);
	/* Since this is a new block, no need to log changes to this block */
	retval = pmfs_add_dirent_to_buf(NULL, dentry, inode, de, blk_base,
		pidir);

	//@TODO here should create new function to write empoty block


	// if(0 < pmfs_byte_issue(1, page_to_pfn(virt_to_page(de)), pmfs_virt_to_offset(de), sizeof(*de), de_c)) { 
	// 	printk(KERN_ERR "%s : read filed", __func__);
	// }
	kfree(de_c);
out:
	return retval;
}

/* removes a directory entry pointing to the inode. assumes the inode has
 * already been logged for consistency
 */
int pmfs_remove_entry(pmfs_transaction_t *trans, struct dentry *de,
		struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct inode *dir = de->d_parent->d_inode;
	struct pmfs_inode *pidir;
	struct qstr *entry = &de->d_name;
	struct pmfs_direntry *res_entry, *prev_entry;
	int retval = -EINVAL;
	unsigned long blocks, block;
	char *blk_base = NULL;

	struct pmfs_inode *pidir_c;

	if (!de->d_name.len)
		return -EINVAL;

	blocks = dir->i_size >> sb->s_blocksize_bits;

	for (block = 0; block < blocks; block++) {
		blk_base =
			pmfs_get_block(sb, pmfs_find_data_block(dir, block));
		if (!blk_base)
			goto out;
		if (pmfs_search_dirblock(blk_base, dir, entry,
					  block << sb->s_blocksize_bits,
					  &res_entry, &prev_entry) == 1)
			break;
	}

	if (block == blocks)
		goto out;
	if (prev_entry) {
	#ifdef ENABLE_JOURNAL
		pmfs_add_logentry(sb, trans, &prev_entry->de_len,
				sizeof(prev_entry->de_len), LE_DATA);
	#endif
		pmfs_memunlock_block(sb, blk_base);
		prev_entry->de_len =
			cpu_to_le16(le16_to_cpu(prev_entry->de_len) +
				    le16_to_cpu(res_entry->de_len));
		pmfs_memlock_block(sb, blk_base);
		//@TODO here is a bit vague, should have more accurate align
		pmfs_ssd_write(prev_entry,128); 
		
	} else {
	#ifdef ENABLE_JOURNAL
		pmfs_add_logentry(sb, trans, &res_entry->ino,
				sizeof(res_entry->ino), LE_DATA);
	#endif
		pmfs_memunlock_block(sb, blk_base);
		res_entry->ino = 0;
		pmfs_memlock_block(sb, blk_base);
		pmfs_ssd_write(res_entry,64); 
	}
	/*dir->i_version++; */
	dir->i_ctime = dir->i_mtime = current_time(dir);

	pidir = pmfs_get_inode(sb, dir->i_ino);

	pidir_c = kmalloc(sizeof(*pidir_c), GFP_KERNEL);
	// if(0 < pmfs_byte_issue(0, page_to_pfn(virt_to_page(pidir)), pmfs_virt_to_offset(pidir), sizeof(*pidir), pidir_c)) { 
	// 	printk(KERN_ERR "%s : read filed", __func__);
	// }
#ifdef ENABLE_JOURNAL
	pmfs_add_logentry(sb, trans, pidir, MAX_DATA_PER_LENTRY, LE_DATA);
#endif
	pmfs_memunlock_inode(sb, pidir);
	pidir->i_mtime = cpu_to_le32(dir->i_mtime.tv_sec);
	pidir->i_ctime = cpu_to_le32(dir->i_ctime.tv_sec);
	pmfs_memlock_inode(sb, pidir);
	// @TOD modify access time Should be delayed flush
	pmfs_ssd_write(pidir,64); 
	retval = 0;

	// if(0 < pmfs_byte_issue(1, page_to_pfn(virt_to_page(pidir)), pmfs_virt_to_offset(pidir), sizeof(*pidir), pidir_c)) { 
	// 	printk(KERN_ERR "%s : read filed", __func__);
	// }
	kfree(pidir_c);
out:
	return retval;
}

static int pmfs_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct pmfs_inode *pi;
	char *blk_base;
	unsigned long offset;
	struct pmfs_direntry *de;
	ino_t ino;
	timing_t readdir_time;

	struct pmfs_inode *pi_c;


	PMFS_START_TIMING(readdir_t, readdir_time);

	offset = ctx->pos & (sb->s_blocksize - 1);
	while (ctx->pos < inode->i_size) {
		unsigned long blk = ctx->pos >> sb->s_blocksize_bits;

		blk_base =
			pmfs_get_block(sb, pmfs_find_data_block(inode, blk));
		if (!blk_base) {
			pmfs_dbg("directory %lu contains a hole at offset %lld\n",
				inode->i_ino, ctx->pos);
			ctx->pos += sb->s_blocksize - offset;
			continue;
		}
#if 0
		if (file->f_version != inode->i_version) {
			for (i = 0; i < sb->s_blocksize && i < offset; ) {
				de = (struct pmfs_direntry *)(blk_base + i);
				/* It's too expensive to do a full
				 * dirent test each time round this
				 * loop, but we do have to test at
				 * least that it is non-zero.  A
				 * failure will be detected in the
				 * dirent test below. */
				if (le16_to_cpu(de->de_len) <
				    PMFS_DIR_REC_LEN(1))
					break;
				i += le16_to_cpu(de->de_len);
			}
			offset = i;
			ctx->pos =
				(ctx->pos & ~(sb->s_blocksize - 1)) | offset;
			file->f_version = inode->i_version;
		}
#endif
		while (ctx->pos < inode->i_size
		       && offset < sb->s_blocksize) {
			de = (struct pmfs_direntry *)(blk_base + offset);
			if (!pmfs_check_dir_entry("pmfs_readdir", inode, de,
						   blk_base, offset)) {
				/* On error, skip to the next block. */
				ctx->pos = ALIGN(ctx->pos, sb->s_blocksize);
				break;
			}
			offset += le16_to_cpu(de->de_len);
			if (de->ino) {
				ino = le64_to_cpu(de->ino);
				pi = pmfs_get_inode(sb, ino);

				pi_c = kmalloc(sizeof(*pi_c), GFP_KERNEL);
				// if(0 < pmfs_byte_issue(0, page_to_pfn(virt_to_page(pi)), pmfs_virt_to_offset(pi), sizeof(*pi), pi_c)) { 
				// 	printk(KERN_ERR "%s : read filed", __func__);
				// }

				if (!dir_emit(ctx, de->name, de->name_len,
					ino, IF2DT(le16_to_cpu(pi->i_mode)))){
					// if(0 < pmfs_byte_issue(1, page_to_pfn(virt_to_page(pi)), pmfs_virt_to_offset(pi), sizeof(*pi), pi_c)) { 
					// 	printk(KERN_ERR "%s : read filed", __func__);
					// }
					kfree(pi_c);
					return 0;
				}
				// if(0 < pmfs_byte_issue(1, page_to_pfn(virt_to_page(pi)), pmfs_virt_to_offset(pi), sizeof(*pi), pi_c)) { 
				// 	printk(KERN_ERR "%s : read filed", __func__);
				// }
				kfree(pi_c);

			}
			ctx->pos += le16_to_cpu(de->de_len);
		}
		offset = 0;
	}
	PMFS_END_TIMING(readdir_t, readdir_time);
	return 0;
}

const struct file_operations pmfs_dir_operations = {
	.read		= generic_read_dir,
	.iterate	= pmfs_readdir,
	.fsync		= noop_fsync,
	.unlocked_ioctl = pmfs_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= pmfs_compat_ioctl,
#endif
};
