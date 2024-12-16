/*
 * BRIEF DESCRIPTION
 *
 * Inode operations for directories.
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
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "bytefs.h"
#include "journal.h"
#include "inode.h"

static ino_t bytefs_inode_by_name(struct inode *dir, struct qstr *entry,
				 struct bytefs_dentry **res_entry)
{
	struct super_block *sb = dir->i_sb;
	struct bytefs_dentry *direntry;
	struct bytefs_dentry *direntryc, entry_copy;

	direntry = bytefs_find_dentry(sb, NULL, dir,
					entry->name, entry->len);
	if (direntry == NULL)
		return 0;

	if (metadata_csum == 0)
		direntryc = direntry;
	else {
		direntryc = &entry_copy;
		if (!bytefs_verify_entry_csum(sb, direntry, direntryc))
			return 0;
	}

	*res_entry = direntry;

	struct bytefs_dentry *direntryc_c;
	// BYTEFS_CACHE_BYTE_ISSUE(direntryc, direntryc_c, BYTEFS_DIR_ISSUE);
	// BYTEFS_DECACHE_END_BYTE_ISSUE(direntryc, direntryc_c);

	return direntryc->ino;
}

// haor2 : based on the fact that dentry is cached in meory
static struct dentry *bytefs_lookup(struct inode *dir, struct dentry *dentry,
				   unsigned int flags)
{
	struct inode *inode = NULL;
	struct bytefs_dentry *de;
	ino_t ino;
	INIT_TIMING(lookup_time);

	BYTEFS_START_TIMING(lookup_t, lookup_time);
	if (dentry->d_name.len > BYTEFS_NAME_LEN) {
		bytefs_dbg("%s: namelen %u exceeds limit\n",
			__func__, dentry->d_name.len);
		return ERR_PTR(-ENAMETOOLONG);
	}

	bytefs_dbg_verbose("%s: %s\n", __func__, dentry->d_name.name);
	ino = bytefs_inode_by_name(dir, &dentry->d_name, &de);
	bytefs_dbg_verbose("%s: ino %lu\n", __func__, ino);
	if (ino) {
		inode = bytefs_iget(dir->i_sb, ino);
		if (inode == ERR_PTR(-ESTALE) || inode == ERR_PTR(-ENOMEM)
				|| inode == ERR_PTR(-EACCES)) {
			bytefs_err(dir->i_sb,
				  "%s: get inode failed: %lu\n",
				  __func__, (unsigned long)ino);
			return ERR_PTR(-EIO);
		}
	}

	BYTEFS_END_TIMING(lookup_t, lookup_time);
	return d_splice_alias(inode, dentry);
}

static void bytefs_lite_transaction_for_new_inode(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode *pidir, struct inode *inode,
	struct inode *dir, struct bytefs_inode_update *update)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	int cpu;
	u64 journal_tail;
	unsigned long irq_flags = 0;
	INIT_TIMING(trans_time);

	BYTEFS_START_TIMING(create_trans_t, trans_time);

	cpu = bytefs_get_cpuid(sb);
	spin_lock(&sbi->journal_locks[cpu]);
	bytefs_memunlock_journal(sb, &irq_flags);

	// If you change what's required to create a new inode, you need to
	// update this functions so the changes will be roll back on failure.
	journal_tail = bytefs_create_inode_transaction(sb, inode, dir, cpu, 1, 0);

	bytefs_update_inode(sb, dir, pidir, update, 0);

	pi->valid = 1;
	bytefs_update_inode_checksum(pi);

	bytefs_commit_lite_transaction(sb, journal_tail, cpu);
	bytefs_memlock_journal(sb, &irq_flags);
	spin_unlock(&sbi->journal_locks[cpu]);

	if (metadata_csum) {
		bytefs_memunlock_inode(sb, pi, &irq_flags);
		bytefs_update_alter_inode(sb, inode, pi);
		bytefs_update_alter_inode(sb, dir, pidir);
		bytefs_memlock_inode(sb, pi, &irq_flags);
	}
	BYTEFS_END_TIMING(create_trans_t, trans_time);
}

/* Returns new tail after append */
/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate().
 */
static int bytefs_create(struct inode *dir, struct dentry *dentry, umode_t mode,
			bool excl)
{
	struct inode *inode = NULL;
	int err = PTR_ERR(inode);
	struct super_block *sb = dir->i_sb;
	struct bytefs_inode *pidir, *pi;
	struct bytefs_inode_update update;
	u64 pi_addr = 0;
	u64 ino, epoch_id;
	INIT_TIMING(create_time);

	BYTEFS_START_TIMING(create_t, create_time);

	pidir = bytefs_get_inode(sb, dir); // overhead added
	if (!pidir)
		goto out_err;

	epoch_id = bytefs_get_epoch_id(sb);
	ino = bytefs_new_bytefs_inode(sb, &pi_addr);
	if (ino == 0)
		goto out_err;

	update.tail = 0;
	update.alter_tail = 0;
	err = bytefs_add_dentry(dentry, ino, 0, &update, epoch_id);
	if (err)
		goto out_err;

	bytefs_dbgv("%s: %s\n", __func__, dentry->d_name.name);
	bytefs_dbgv("%s: inode %llu, dir %lu\n", __func__, ino, dir->i_ino);
	inode = bytefs_new_vfs_inode(TYPE_CREATE, dir, pi_addr, ino, mode,
					0, 0, &dentry->d_name, epoch_id);
	if (IS_ERR(inode))
		goto out_err;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	pi = bytefs_get_block(sb, pi_addr);
	bytefs_lite_transaction_for_new_inode(sb, pi, pidir, inode, dir,
						&update);
	BYTEFS_END_TIMING(create_t, create_time);
	return err;
out_err:
	bytefs_err(sb, "%s return %d\n", __func__, err);
	BYTEFS_END_TIMING(create_t, create_time);
	return err;
}

static int bytefs_mknod(struct inode *dir, struct dentry *dentry, umode_t mode,
		       dev_t rdev)
{
	struct inode *inode = NULL;
	int err = PTR_ERR(inode);
	struct super_block *sb = dir->i_sb;
	u64 pi_addr = 0;
	struct bytefs_inode *pidir, *pi;
	struct bytefs_inode_update update;
	u64 ino;
	u64 epoch_id;
	INIT_TIMING(mknod_time);

	BYTEFS_START_TIMING(mknod_t, mknod_time);

	pidir = bytefs_get_inode(sb, dir);
	if (!pidir)
		goto out_err;

	epoch_id = bytefs_get_epoch_id(sb);
	ino = bytefs_new_bytefs_inode(sb, &pi_addr);
	if (ino == 0)
		goto out_err;

	bytefs_dbgv("%s: %s\n", __func__, dentry->d_name.name);
	bytefs_dbgv("%s: inode %llu, dir %lu\n", __func__, ino, dir->i_ino);

	update.tail = 0;
	update.alter_tail = 0;
	err = bytefs_add_dentry(dentry, ino, 0, &update, epoch_id);
	if (err)
		goto out_err;

	inode = bytefs_new_vfs_inode(TYPE_MKNOD, dir, pi_addr, ino, mode,
					0, rdev, &dentry->d_name, epoch_id);
	if (IS_ERR(inode))
		goto out_err;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	pi = bytefs_get_block(sb, pi_addr);
	bytefs_lite_transaction_for_new_inode(sb, pi, pidir, inode, dir,
						&update);
	BYTEFS_END_TIMING(mknod_t, mknod_time);
	return err;
out_err:
	bytefs_err(sb, "%s return %d\n", __func__, err);
	BYTEFS_END_TIMING(mknod_t, mknod_time);
	return err;
}

static int bytefs_symlink(struct inode *dir, struct dentry *dentry,
			 const char *symname)
{
	struct super_block *sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned int len = strlen(symname);
	struct inode *inode;
	struct bytefs_inode_info *si;
	struct bytefs_inode_info_header *sih;
	u64 pi_addr = 0;
	struct bytefs_inode *pidir, *pi;
	struct bytefs_inode_update update;
	u64 ino;
	u64 epoch_id;
	INIT_TIMING(symlink_time);

	BYTEFS_START_TIMING(symlink_t, symlink_time);
	if (len + 1 > sb->s_blocksize)
		goto out;

	pidir = bytefs_get_inode(sb, dir);
	if (!pidir)
		goto out_fail;

	epoch_id = bytefs_get_epoch_id(sb);
	ino = bytefs_new_bytefs_inode(sb, &pi_addr);
	if (ino == 0)
		goto out_fail;

	bytefs_dbgv("%s: name %s, symname %s\n", __func__,
				dentry->d_name.name, symname);
	bytefs_dbgv("%s: inode %llu, dir %lu\n", __func__, ino, dir->i_ino);

	update.tail = 0;
	update.alter_tail = 0;
	err = bytefs_add_dentry(dentry, ino, 0, &update, epoch_id);
	if (err)
		goto out_fail;

	inode = bytefs_new_vfs_inode(TYPE_SYMLINK, dir, pi_addr, ino,
					S_IFLNK|0777, len, 0,
					&dentry->d_name, epoch_id);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_fail;
	}

	pi = bytefs_get_inode(sb, inode);

	si = BYTEFS_I(inode);
	sih = &si->header;

	err = bytefs_block_symlink(sb, pi, inode, symname, len, epoch_id);
	if (err)
		goto out_fail;

	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	bytefs_lite_transaction_for_new_inode(sb, pi, pidir, inode, dir,
					&update);
out:
	BYTEFS_END_TIMING(symlink_t, symlink_time);
	return err;

out_fail:
	bytefs_err(sb, "%s return %d\n", __func__, err);
	goto out;
}

static void bytefs_lite_transaction_for_time_and_link(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode *pidir, struct inode *inode,
	struct inode *dir, struct bytefs_inode_update *update,
	struct bytefs_inode_update *update_dir, int invalidate, u64 epoch_id)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	u64 journal_tail;
	int cpu;
	unsigned long irq_flags = 0;
	INIT_TIMING(trans_time);

	BYTEFS_START_TIMING(link_trans_t, trans_time);

	cpu = bytefs_get_cpuid(sb);
	spin_lock(&sbi->journal_locks[cpu]);
	bytefs_memunlock_journal(sb, &irq_flags);

	// If you change what's required to create a new inode, you need to
	// update this functions so the changes will be roll back on failure.
	journal_tail = bytefs_create_inode_transaction(sb, inode, dir, cpu,
						0, invalidate);

	if (invalidate) {
		pi->valid = 0;
		pi->delete_epoch_id = epoch_id;
	}
	bytefs_update_inode(sb, inode, pi, update, 0);

	bytefs_update_inode(sb, dir, pidir, update_dir, 0);

	PERSISTENT_BARRIER();

	bytefs_commit_lite_transaction(sb, journal_tail, cpu);
	bytefs_memlock_journal(sb, &irq_flags);
	spin_unlock(&sbi->journal_locks[cpu]);

	if (metadata_csum) {
		bytefs_memunlock_inode(sb, pi, &irq_flags);
		bytefs_update_alter_inode(sb, inode, pi);
		bytefs_update_alter_inode(sb, dir, pidir);
		bytefs_memlock_inode(sb, pi, &irq_flags);
	}

	BYTEFS_END_TIMING(link_trans_t, trans_time);
}

static int bytefs_link(struct dentry *dest_dentry, struct inode *dir,
		      struct dentry *dentry)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode = dest_dentry->d_inode;
	struct bytefs_inode *pi = bytefs_get_inode(sb, inode);
	struct bytefs_inode *pidir;
	struct bytefs_inode_update update_dir;
	struct bytefs_inode_update update;
	u64 old_linkc = 0;
	u64 epoch_id;
	int err = -ENOMEM;
	INIT_TIMING(link_time);

	BYTEFS_START_TIMING(link_t, link_time);
	if (inode->i_nlink >= BYTEFS_LINK_MAX) {
		err = -EMLINK;
		goto out;
	}

	pidir = bytefs_get_inode(sb, dir);
	if (!pidir) {
		err = -EINVAL;
		goto out;
	}

	ihold(inode);
	epoch_id = bytefs_get_epoch_id(sb);

	bytefs_dbgv("%s: name %s, dest %s\n", __func__,
			dentry->d_name.name, dest_dentry->d_name.name);
	bytefs_dbgv("%s: inode %lu, dir %lu\n", __func__,
			inode->i_ino, dir->i_ino);

	update_dir.tail = 0;
	update_dir.alter_tail = 0;
	err = bytefs_add_dentry(dentry, inode->i_ino, 0, &update_dir, epoch_id);
	if (err) {
		iput(inode);
		goto out;
	}

	inode->i_ctime = current_time(inode);
	inc_nlink(inode);

	update.tail = 0;
	update.alter_tail = 0;
	err = bytefs_append_link_change_entry(sb, pi, inode, &update,
						&old_linkc, epoch_id);
	if (err) {
		iput(inode);
		goto out;
	}

	d_instantiate(dentry, inode);
	bytefs_lite_transaction_for_time_and_link(sb, pi, pidir, inode, dir,
					&update, &update_dir, 0, epoch_id);

	bytefs_invalidate_link_change_entry(sb, old_linkc);

out:
	BYTEFS_END_TIMING(link_t, link_time);
	return err;
}

static int bytefs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = dir->i_sb;
	int retval = -ENOMEM;
	struct bytefs_inode *pi = bytefs_get_inode(sb, inode);
	struct bytefs_inode *pidir;
	struct bytefs_inode_update update_dir;
	struct bytefs_inode_update update;
	u64 old_linkc = 0;
	u64 epoch_id;
	int invalidate = 0;
	INIT_TIMING(unlink_time);

	BYTEFS_START_TIMING(unlink_t, unlink_time);

	pidir = bytefs_get_inode(sb, dir);
	if (!pidir)
		goto out;

	epoch_id = bytefs_get_epoch_id(sb);
	bytefs_dbgv("%s: %s\n", __func__, dentry->d_name.name);
	bytefs_dbgv("%s: inode %lu, dir %lu\n", __func__,
				inode->i_ino, dir->i_ino);

	update_dir.tail = 0;
	update_dir.alter_tail = 0;
	retval = bytefs_remove_dentry(dentry, 0, &update_dir, epoch_id, false);
	if (retval)
		goto out;

	inode->i_ctime = dir->i_ctime;

	if (inode->i_nlink == 1)
		invalidate = 1;

	if (inode->i_nlink)
		drop_nlink(inode);

	update.tail = 0;
	update.alter_tail = 0;
	retval = bytefs_append_link_change_entry(sb, pi, inode, &update,
						&old_linkc, epoch_id);
	if (retval)
		goto out;

	bytefs_lite_transaction_for_time_and_link(sb, pi, pidir, inode, dir,
				&update, &update_dir, invalidate, epoch_id);

	bytefs_invalidate_link_change_entry(sb, old_linkc);
	bytefs_invalidate_dentries(sb, &update_dir);

	BYTEFS_END_TIMING(unlink_t, unlink_time);
	return 0;
out:
	bytefs_err(sb, "%s return %d\n", __func__, retval);
	BYTEFS_END_TIMING(unlink_t, unlink_time);
	return retval;
}

static int bytefs_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct bytefs_inode *pidir, *pi;
	struct bytefs_inode_info *si, *sidir;
	struct bytefs_inode_info_header *sih = NULL;
	struct bytefs_inode_update update;
	u64 pi_addr = 0;
	u64 ino;
	u64 epoch_id;
	int err = -EMLINK;
	INIT_TIMING(mkdir_time);

	BYTEFS_START_TIMING(mkdir_t, mkdir_time);
	if (dir->i_nlink >= BYTEFS_LINK_MAX)
		goto out;

	ino = bytefs_new_bytefs_inode(sb, &pi_addr);
	if (ino == 0)
		goto out_err;

	epoch_id = bytefs_get_epoch_id(sb);
	bytefs_dbgv("%s: name %s\n", __func__, dentry->d_name.name);
	bytefs_dbgv("%s: inode %llu, dir %lu, link %d\n", __func__,
				ino, dir->i_ino, dir->i_nlink);

	update.tail = 0;
	update.alter_tail = 0;
	err = bytefs_add_dentry(dentry, ino, 1, &update, epoch_id);
	if (err) {
		bytefs_dbg("failed to add dir entry\n");
		goto out_err;
	}

	inode = bytefs_new_vfs_inode(TYPE_MKDIR, dir, pi_addr, ino,
					S_IFDIR | mode, sb->s_blocksize,
					0, &dentry->d_name, epoch_id);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		goto out_err;
	}

	pi = bytefs_get_inode(sb, inode);
	err = bytefs_append_dir_init_entries(sb, pi, inode->i_ino, dir->i_ino,
					epoch_id);
	if (err < 0)
		goto out_err;

	/* Build the dir tree */
	si = BYTEFS_I(inode);
	sih = &si->header;
	bytefs_rebuild_dir_inode_tree(sb, pi, pi_addr, sih);

	pidir = bytefs_get_inode(sb, dir);
	sidir = BYTEFS_I(dir);
	sih = &si->header;
	dir->i_blocks = sih->i_blocks;
	inc_nlink(dir);
	d_instantiate(dentry, inode);
	unlock_new_inode(inode);

	bytefs_lite_transaction_for_new_inode(sb, pi, pidir, inode, dir,
					&update);
out:
	BYTEFS_END_TIMING(mkdir_t, mkdir_time);
	return err;

out_err:
//	clear_nlink(inode);
	bytefs_err(sb, "%s return %d\n", __func__, err);
	goto out;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
static int bytefs_empty_dir(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_range_node *curr;
	struct bytefs_dentry *entry;
	struct bytefs_dentry *entryc, entry_copy;
	struct rb_node *temp;

	entryc = (metadata_csum == 0) ? entry : &entry_copy;

	temp = rb_first(&sih->rb_tree);
	while (temp) {
		curr = container_of(temp, struct bytefs_range_node, node);
		entry = curr->direntry;

		if (metadata_csum == 0)
			entryc = entry;
		else if (!bytefs_verify_entry_csum(sb, entry, entryc))
			return 0;

		if (!is_dir_init_entry(sb, entryc))
			return 0;

		temp = rb_next(temp);
	}

	return 1;
}

static int bytefs_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = dentry->d_inode;
	struct bytefs_dentry *de;
	struct super_block *sb = inode->i_sb;
	struct bytefs_inode *pi = bytefs_get_inode(sb, inode), *pidir;
	struct bytefs_inode_update update_dir;
	struct bytefs_inode_update update;
	u64 old_linkc = 0;
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	int err = -ENOTEMPTY;
	u64 epoch_id;
	INIT_TIMING(rmdir_time);

	BYTEFS_START_TIMING(rmdir_t, rmdir_time);
	if (!inode)
		return -ENOENT;

	bytefs_dbgv("%s: name %s\n", __func__, dentry->d_name.name);
	pidir = bytefs_get_inode(sb, dir);
	if (!pidir)
		return -EINVAL;

	if (bytefs_inode_by_name(dir, &dentry->d_name, &de) == 0)
		return -ENOENT;

	if (!bytefs_empty_dir(inode))
		return err;

	bytefs_dbgv("%s: inode %lu, dir %lu, link %d\n", __func__,
				inode->i_ino, dir->i_ino, dir->i_nlink);

	if (inode->i_nlink != 2)
		bytefs_dbg("empty directory %lu has nlink!=2 (%d), dir %lu",
				inode->i_ino, inode->i_nlink, dir->i_ino);

	epoch_id = bytefs_get_epoch_id(sb);

	update_dir.tail = 0;
	update_dir.alter_tail = 0;
	err = bytefs_remove_dentry(dentry, -1, &update_dir, epoch_id, false);
	if (err)
		goto end_rmdir;

	/*inode->i_version++; */
	clear_nlink(inode);
	inode->i_ctime = dir->i_ctime;

	if (dir->i_nlink)
		drop_nlink(dir);

	bytefs_delete_dir_tree(sb, sih);

	update.tail = 0;
	update.alter_tail = 0;
	err = bytefs_append_link_change_entry(sb, pi, inode, &update,
						&old_linkc, epoch_id);
	if (err)
		goto end_rmdir;

	bytefs_lite_transaction_for_time_and_link(sb, pi, pidir, inode, dir,
					&update, &update_dir, 1, epoch_id);

	bytefs_invalidate_link_change_entry(sb, old_linkc);
	bytefs_invalidate_dentries(sb, &update_dir);

	BYTEFS_END_TIMING(rmdir_t, rmdir_time);
	return err;

end_rmdir:
	bytefs_err(sb, "%s return %d\n", __func__, err);
	BYTEFS_END_TIMING(rmdir_t, rmdir_time);
	return err;
}

static int bytefs_rename(struct inode *old_dir,
			struct dentry *old_dentry,
			struct inode *new_dir, struct dentry *new_dentry,
			unsigned int flags)
{
	struct inode *old_inode = old_dentry->d_inode;
	struct inode *new_inode = new_dentry->d_inode;
	struct super_block *sb = old_inode->i_sb;
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	struct bytefs_inode *old_pi = NULL, *new_pi = NULL;
	struct bytefs_inode *new_pidir = NULL, *old_pidir = NULL;
	struct bytefs_dentry *father_entry = NULL;
	struct bytefs_dentry *father_entryc, entry_copy;
	char *head_addr = NULL;
	int invalidate_new_inode = 0;
	struct bytefs_inode_update update_dir_new;
	struct bytefs_inode_update update_dir_old;
	struct bytefs_inode_update update_new;
	struct bytefs_inode_update update_old;
	u64 old_linkc1 = 0, old_linkc2 = 0;
	int err = -ENOENT;
	int inc_link = 0, dec_link = 0;
	int cpu;
	int change_parent = 0;
	u64 journal_tail;
	u64 epoch_id;
	unsigned long irq_flags = 0;
	INIT_TIMING(rename_time);

	bytefs_dbgv("%s: rename %s to %s,\n", __func__,
			old_dentry->d_name.name, new_dentry->d_name.name);
	bytefs_dbgv("%s: %s inode %lu, old dir %lu, new dir %lu, new inode %lu\n",
			__func__, S_ISDIR(old_inode->i_mode) ? "dir" : "normal",
			old_inode->i_ino, old_dir->i_ino, new_dir->i_ino,
			new_inode ? new_inode->i_ino : 0);

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	BYTEFS_START_TIMING(rename_t, rename_time);

	if (new_inode) {
		err = -ENOTEMPTY;
		if (S_ISDIR(old_inode->i_mode) && !bytefs_empty_dir(new_inode))
			goto out;
	} else {
		if (S_ISDIR(old_inode->i_mode)) {
			err = -EMLINK;
			if (new_dir->i_nlink >= BYTEFS_LINK_MAX)
				goto out;
		}
	}

	if (S_ISDIR(old_inode->i_mode)) {
		dec_link = -1;
		if (!new_inode)
			inc_link = 1;
		/*
		 * Tricky for in-place update:
		 * New dentry is always after renamed dentry, so we have to
		 * make sure new dentry has the correct links count
		 * to workaround the rebuild nlink issue.
		 */
		if (old_dir == new_dir) {
			inc_link--;
			if (inc_link == 0)
				dec_link = 0;
		}
	}

	epoch_id = bytefs_get_epoch_id(sb);
	new_pidir = bytefs_get_inode(sb, new_dir);
	old_pidir = bytefs_get_inode(sb, old_dir);

	old_pi = bytefs_get_inode(sb, old_inode);
	old_inode->i_ctime = current_time(old_inode);
	update_old.tail = 0;
	update_old.alter_tail = 0;
	err = bytefs_append_link_change_entry(sb, old_pi, old_inode,
					&update_old, &old_linkc1, epoch_id);
	if (err)
		goto out;

	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		/* My father is changed. Update .. entry */
		/* For simplicity, we use in-place update and journal it */
		change_parent = 1;
		head_addr = (char *)bytefs_get_block(sb, old_pi->log_head);
		father_entry = (struct bytefs_dentry *)(head_addr +
					BYTEFS_DIR_LOG_REC_LEN(1));

		if (metadata_csum == 0)
			father_entryc = father_entry;
		else {
			father_entryc = &entry_copy;
			if (!bytefs_verify_entry_csum(sb, father_entry,
							father_entryc)) {
				err = -EIO;
				goto out;
			}
		}

		if (le64_to_cpu(father_entryc->ino) != old_dir->i_ino)
			bytefs_err(sb, "%s: dir %lu parent should be %lu, but actually %lu\n",
				__func__,
				old_inode->i_ino, old_dir->i_ino,
				le64_to_cpu(father_entry->ino));
	}

	update_dir_new.tail = 0;
	update_dir_new.alter_tail = 0;
	if (new_inode) {
		/* First remove the old entry in the new directory */
		err = bytefs_remove_dentry(new_dentry, 0, &update_dir_new,
					epoch_id, true);
		if (err)
			goto out;
	}

	/* link into the new directory. */
	err = bytefs_add_dentry(new_dentry, old_inode->i_ino,
				inc_link, &update_dir_new, epoch_id);
	if (err)
		goto out;

	if (inc_link > 0)
		inc_nlink(new_dir);

	update_dir_old.tail = 0;
	update_dir_old.alter_tail = 0;
	if (old_dir == new_dir) {
		update_dir_old.tail = update_dir_new.tail;
		update_dir_old.alter_tail = update_dir_new.alter_tail;
	}

	err = bytefs_remove_dentry(old_dentry, dec_link, &update_dir_old,
					epoch_id, true);
	if (err)
		goto out;

	if (dec_link < 0)
		drop_nlink(old_dir);

	if (new_inode) {
		new_pi = bytefs_get_inode(sb, new_inode);
		new_inode->i_ctime = current_time(new_inode);

		if (S_ISDIR(old_inode->i_mode)) {
			if (new_inode->i_nlink)
				drop_nlink(new_inode);
		}
		if (new_inode->i_nlink)
			drop_nlink(new_inode);

		update_new.tail = 0;
		update_new.alter_tail = 0;
		err = bytefs_append_link_change_entry(sb, new_pi, new_inode,
						&update_new, &old_linkc2,
						epoch_id);
		if (err)
			goto out;
	}

	cpu = bytefs_get_cpuid(sb);
	spin_lock(&sbi->journal_locks[cpu]);
	bytefs_memunlock_journal(sb, &irq_flags);
	if (new_inode && new_inode->i_nlink == 0)
		invalidate_new_inode = 1;
	journal_tail = bytefs_create_rename_transaction(sb, old_inode, old_dir,
				new_inode,
				old_dir != new_dir ? new_dir : NULL,
				father_entry,
				new_inode ? update_dir_new.create_dentry : NULL,
				update_dir_old.create_dentry,
				invalidate_new_inode,
				cpu);
	if (new_inode)
		bytefs_reassign_logentry(sb, update_dir_new.create_dentry, DIR_LOG);
	bytefs_reassign_logentry(sb, update_dir_old.create_dentry, DIR_LOG);
	bytefs_update_inode(sb, old_inode, old_pi, &update_old, 0);
	bytefs_update_inode(sb, old_dir, old_pidir, &update_dir_old, 0);

	if (old_pidir != new_pidir)
		bytefs_update_inode(sb, new_dir, new_pidir, &update_dir_new, 0);

	if (change_parent && father_entry) {
		father_entry->ino = cpu_to_le64(new_dir->i_ino);
		bytefs_update_entry_csum(father_entry);
		bytefs_update_alter_entry(sb, father_entry);
	}

	if (new_inode) {
		if (invalidate_new_inode) {
			new_pi->valid = 0;
			new_pi->delete_epoch_id = epoch_id;
		}
		bytefs_update_inode(sb, new_inode, new_pi, &update_new, 0);
	}

	PERSISTENT_BARRIER();

	bytefs_commit_lite_transaction(sb, journal_tail, cpu);
	bytefs_memlock_journal(sb, &irq_flags);
	spin_unlock(&sbi->journal_locks[cpu]);

	bytefs_memunlock_inode(sb, old_pi, &irq_flags);
	bytefs_update_alter_inode(sb, old_inode, old_pi);
	bytefs_update_alter_inode(sb, old_dir, old_pidir);
	if (old_dir != new_dir)
		bytefs_update_alter_inode(sb, new_dir, new_pidir);
	if (new_inode)
		bytefs_update_alter_inode(sb, new_inode, new_pi);
	bytefs_memlock_inode(sb, old_pi, &irq_flags);

	bytefs_invalidate_link_change_entry(sb, old_linkc1);
	bytefs_invalidate_link_change_entry(sb, old_linkc2);
	if (new_inode)
		bytefs_invalidate_dentries(sb, &update_dir_new);
	bytefs_invalidate_dentries(sb, &update_dir_old);

	BYTEFS_END_TIMING(rename_t, rename_time);
	return 0;
out:
	bytefs_err(sb, "%s return %d\n", __func__, err);
	BYTEFS_END_TIMING(rename_t, rename_time);
	return err;
}
// haor2 : based on the fact that dentry is cached in meory
struct dentry *bytefs_get_parent(struct dentry *child)
{
	struct inode *inode;
	struct qstr dotdot = QSTR_INIT("..", 2);
	struct bytefs_dentry *de = NULL;
	ino_t ino;

	bytefs_inode_by_name(child->d_inode, &dotdot, &de);
	if (!de)
		return ERR_PTR(-ENOENT);

	/* FIXME: can de->ino be avoided by using the return value of
	 * bytefs_inode_by_name()?
	 */
	ino = le64_to_cpu(de->ino);

	if (ino)
		inode = bytefs_iget(child->d_inode->i_sb, ino);
	else
		return ERR_PTR(-ENOENT);

	return d_obtain_alias(inode);
}

const struct inode_operations bytefs_dir_inode_operations = {
	.create		= bytefs_create,
	.lookup		= bytefs_lookup,
	.link		= bytefs_link,
	.unlink		= bytefs_unlink,
	.symlink	= bytefs_symlink,
	.mkdir		= bytefs_mkdir,
	.rmdir		= bytefs_rmdir,
	.mknod		= bytefs_mknod,
	.rename		= bytefs_rename,
	.setattr	= bytefs_notify_change,
	.get_acl	= NULL,
};

const struct inode_operations bytefs_special_inode_operations = {
	.setattr	= bytefs_notify_change,
	.get_acl	= NULL,
};
