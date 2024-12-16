/*
 * BRIEF DESCRIPTION
 *
 * Log methods
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

#include "bytefs.h"
#include "journal.h"
#include "inode.h"
#include "log.h"

static int bytefs_execute_invalidate_reassign_logentry(struct super_block *sb,
	void *entry, enum bytefs_entry_type type, int reassign,
	unsigned int num_free)
{
	struct bytefs_file_write_entry *fw_entry;
	int invalid = 0;

	switch (type) {
	case FILE_WRITE:
		fw_entry = (struct bytefs_file_write_entry *)entry;
		if (reassign)
			fw_entry->reassigned = 1;
		if (num_free)
			fw_entry->invalid_pages += num_free;
		if (fw_entry->invalid_pages == fw_entry->num_pages)
			invalid = 1;
		break;
	case DIR_LOG:
		if (reassign) {
			((struct bytefs_dentry *)entry)->reassigned = 1;
		} else {
			((struct bytefs_dentry *)entry)->invalid = 1;
			invalid = 1;
		}
		break;
	case SET_ATTR:
		((struct bytefs_setattr_logentry *)entry)->invalid = 1;
		invalid = 1;
		break;
	case LINK_CHANGE:
		((struct bytefs_link_change_entry *)entry)->invalid = 1;
		invalid = 1;
		break;
	case MMAP_WRITE:
		((struct bytefs_mmap_entry *)entry)->invalid = 1;
		invalid = 1;
		break;
	// case SNAPSHOT_INFO:
	// 	((struct bytefs_snapshot_info_entry *)entry)->deleted = 1;
	// 	invalid = 1;
	// 	break;
	default:
		break;
	}

	if (invalid) {
		u64 addr = bytefs_get_addr_off(BYTEFS_SB(sb), entry);

		bytefs_inc_page_invalid_entries(sb, addr);
	}

	bytefs_update_entry_csum(entry);
	return 0;
}

static int bytefs_invalidate_reassign_logentry(struct super_block *sb,
	void *entry, enum bytefs_entry_type type, int reassign,
	unsigned int num_free)
{
	unsigned long irq_flags = 0;
	bytefs_memunlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);

	bytefs_execute_invalidate_reassign_logentry(sb, entry, type,
						reassign, num_free);
	bytefs_update_alter_entry(sb, entry);
	bytefs_memlock_range(sb, entry, CACHELINE_SIZE, &irq_flags);

	return 0;
}

int bytefs_invalidate_logentry(struct super_block *sb, void *entry,
	enum bytefs_entry_type type, unsigned int num_free)
{
	return bytefs_invalidate_reassign_logentry(sb, entry, type, 0, num_free);
}

int bytefs_reassign_logentry(struct super_block *sb, void *entry,
	enum bytefs_entry_type type)
{
	return bytefs_invalidate_reassign_logentry(sb, entry, type, 1, 0);
}

static inline int bytefs_invalidate_write_entry(struct super_block *sb,
	struct bytefs_file_write_entry *entry, int reassign,
	unsigned int num_free)
{
	struct bytefs_file_write_entry *entryc, entry_copy;

	if (!entry)
		return 0;

	if (metadata_csum == 0)
		entryc = entry;
	else {
		entryc = &entry_copy;
		if (!bytefs_verify_entry_csum(sb, entry, entryc))
			return -EIO;
	}

	if (num_free == 0 && entryc->reassigned == 1)
		return 0;

	return bytefs_invalidate_reassign_logentry(sb, entry, FILE_WRITE,
							reassign, num_free);
}

unsigned int bytefs_free_old_entry(struct super_block *sb,
	struct bytefs_inode_info_header *sih,
	struct bytefs_file_write_entry *entry,
	unsigned long pgoff, unsigned int num_free,
	bool delete_dead, u64 epoch_id)
{
	struct bytefs_file_write_entry *entryc, entry_copy;
	unsigned long old_nvmm;
	int ret;
	INIT_TIMING(free_time);

	if (!entry)
		return 0;

	BYTEFS_START_TIMING(free_old_t, free_time);

	if (metadata_csum == 0)
		entryc = entry;
	else {
		entryc = &entry_copy;
		if (!bytefs_verify_entry_csum(sb, entry, entryc))
			return -EIO;
	}

	old_nvmm = get_nvmm(sb, sih, entryc, pgoff);

	if (!delete_dead) {
		// ret = bytefs_append_data_to_snapshot(sb, entryc, old_nvmm,
		// 		num_free, epoch_id);
		if (ret == 0) {
			bytefs_invalidate_write_entry(sb, entry, 1, 0);
			goto out;
		}

		bytefs_invalidate_write_entry(sb, entry, 1, num_free);
	}

	bytefs_dbgv("%s: pgoff %lu, free %u blocks\n",
				__func__, pgoff, num_free);
	bytefs_free_data_blocks(sb, sih, old_nvmm, num_free);

out:
	sih->i_blocks -= num_free;

	BYTEFS_END_TIMING(free_old_t, free_time);
	return num_free;
}

struct bytefs_file_write_entry *bytefs_find_next_entry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, pgoff_t pgoff)
{
	struct bytefs_file_write_entry *entry = NULL;
	struct bytefs_file_write_entry *entries[1];
	int nr_entries;

	nr_entries = radix_tree_gang_lookup(&sih->tree,
					(void **)entries, pgoff, 1);
	if (nr_entries == 1)
		entry = entries[0];

	return entry;
}

/*
 * Zero the tail page. Used in resize request
 * to avoid to keep data in case the file grows again.
 */
void bytefs_clear_last_page_tail(struct super_block *sb,
	struct inode *inode, loff_t newsize)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	unsigned long offset = newsize & (sb->s_blocksize - 1);
	unsigned long pgoff, length;
	u64 nvmm;
	char *nvmm_addr;
	unsigned long irq_flags = 0;

	if (offset == 0 || newsize > inode->i_size)
		return;

	length = sb->s_blocksize - offset;
	pgoff = newsize >> sb->s_blocksize_bits;

	nvmm = bytefs_find_nvmm_block(sb, sih, NULL, pgoff);
	if (nvmm == 0)
		return;

	nvmm_addr = (char *)bytefs_get_block(sb, nvmm);
	bytefs_memunlock_range(sb, nvmm_addr + offset, length, &irq_flags);
	memcpy_to_pmem_nocache(nvmm_addr + offset, sbi->zeroed_page, length);
	bytefs_memlock_range(sb, nvmm_addr + offset, length, &irq_flags);

	if (data_csum > 0)
		bytefs_update_truncated_block_csum(sb, inode, newsize);
	if (data_parity > 0)
		bytefs_update_truncated_block_parity(sb, inode, newsize);
}

static void bytefs_update_setattr_entry(struct inode *inode,
	struct bytefs_setattr_logentry *entry,
	struct bytefs_log_entry_info *entry_info)
{
	struct iattr *attr = entry_info->attr;
	unsigned int ia_valid = attr->ia_valid, attr_mask;

	/* These files are in the lowest byte */
	attr_mask = ATTR_MODE | ATTR_UID | ATTR_GID | ATTR_SIZE |
			ATTR_ATIME | ATTR_MTIME | ATTR_CTIME;

	entry->entry_type	= SET_ATTR;
	entry->attr	= ia_valid & attr_mask;
	entry->mode	= cpu_to_le16(inode->i_mode);
	entry->uid	= cpu_to_le32(i_uid_read(inode));
	entry->gid	= cpu_to_le32(i_gid_read(inode));
	entry->atime	= cpu_to_le32(inode->i_atime.tv_sec);
	entry->ctime	= cpu_to_le32(inode->i_ctime.tv_sec);
	entry->mtime	= cpu_to_le32(inode->i_mtime.tv_sec);
	entry->epoch_id = cpu_to_le64(entry_info->epoch_id);
	entry->trans_id	= cpu_to_le64(entry_info->trans_id);
	entry->invalid	= 0;

	if (ia_valid & ATTR_SIZE)
		entry->size = cpu_to_le64(attr->ia_size);
	else
		entry->size = cpu_to_le64(inode->i_size);

	bytefs_update_entry_csum(entry);
}

static void bytefs_update_link_change_entry(struct inode *inode,
	struct bytefs_link_change_entry *entry,
	struct bytefs_log_entry_info *entry_info)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;

	entry->entry_type	= LINK_CHANGE;
	entry->epoch_id		= cpu_to_le64(entry_info->epoch_id);
	entry->trans_id		= cpu_to_le64(entry_info->trans_id);
	entry->invalid		= 0;
	entry->links		= cpu_to_le16(inode->i_nlink);
	entry->ctime		= cpu_to_le32(inode->i_ctime.tv_sec);
	entry->flags		= cpu_to_le32(sih->i_flags);
	entry->generation	= cpu_to_le32(inode->i_generation);

	bytefs_update_entry_csum(entry);
}

static int bytefs_update_write_entry(struct super_block *sb,
	struct bytefs_file_write_entry *entry,
	struct bytefs_log_entry_info *entry_info)
{
	entry->epoch_id = cpu_to_le64(entry_info->epoch_id);
	entry->trans_id = cpu_to_le64(entry_info->trans_id);
	entry->mtime = cpu_to_le32(entry_info->time);
	entry->size = cpu_to_le64(entry_info->file_size);
	entry->updating = 0;
	bytefs_update_entry_csum(entry);
	return 0;
}

static int bytefs_update_old_dentry(struct super_block *sb,
	struct inode *dir, struct bytefs_dentry *dentry,
	struct bytefs_log_entry_info *entry_info)
{
	unsigned short links_count;
	int link_change = entry_info->link_change;
	u64 addr;

	dentry->epoch_id = entry_info->epoch_id;
	dentry->trans_id = entry_info->trans_id;
	/* Remove_dentry */
	dentry->ino = cpu_to_le64(0);
	dentry->invalid = 1;
	dentry->mtime = cpu_to_le32(dir->i_mtime.tv_sec);

	links_count = cpu_to_le16(dir->i_nlink);
	if (links_count == 0 && link_change == -1)
		links_count = 0;
	else
		links_count += link_change;
	dentry->links_count = cpu_to_le16(links_count);

	addr = bytefs_get_addr_off(BYTEFS_SB(sb), dentry);
	bytefs_inc_page_invalid_entries(sb, addr);

	/* Update checksum */
	bytefs_update_entry_csum(dentry);

	return 0;
}

static int bytefs_update_new_dentry(struct super_block *sb,
	struct inode *dir, struct bytefs_dentry *entry,
	struct bytefs_log_entry_info *entry_info)
{
	struct dentry *dentry = entry_info->data;
	unsigned short links_count;
	int link_change = entry_info->link_change;

	entry->entry_type = DIR_LOG;
	entry->epoch_id = entry_info->epoch_id;
	entry->trans_id = entry_info->trans_id;
	entry->ino = entry_info->ino;
	entry->name_len = dentry->d_name.len;
	memcpy_to_pmem_nocache(entry->name, dentry->d_name.name,
				dentry->d_name.len);
	entry->name[dentry->d_name.len] = '\0';
	entry->mtime = cpu_to_le32(dir->i_mtime.tv_sec);
	//entry->size = cpu_to_le64(dir->i_size);

	links_count = cpu_to_le16(dir->i_nlink);
	if (links_count == 0 && link_change == -1)
		links_count = 0;
	else
		links_count += link_change;
	entry->links_count = cpu_to_le16(links_count);

	/* Update actual de_len */
	entry->de_len = cpu_to_le16(entry_info->file_size);

	/* Update checksum */
	bytefs_update_entry_csum(entry);

	return 0;
}

static int bytefs_update_log_entry(struct super_block *sb, struct inode *inode,
	void *entry, struct bytefs_log_entry_info *entry_info)
{
	size_t esize;
	enum bytefs_entry_type type = entry_info->type;

	// we always update the entry in place now

	esize = bytefs_get_log_entry_size(sb, entry_info->type);
	switch (type) {
	case FILE_WRITE:
		if (entry_info->inplace){
			bytefs_update_write_entry(sb, entry, entry_info);
			bytefs_write(entry,esize);
			}
		else{
			memcpy_to_pmem_nocache(entry, entry_info->data,
				sizeof(struct bytefs_file_write_entry));
			bytefs_write(entry,sizeof(struct bytefs_file_write_entry));	
			}
		break;
	case DIR_LOG:
		if (entry_info->inplace){
			bytefs_update_old_dentry(sb, inode, entry, entry_info);
			bytefs_write(entry,esize);
			}
		else{
			bytefs_update_new_dentry(sb, inode, entry, entry_info);
			bytefs_write(entry,esize);
			}
		break;
	case SET_ATTR:
		bytefs_update_setattr_entry(inode, entry, entry_info);
		break;
	case LINK_CHANGE:
		bytefs_update_link_change_entry(inode, entry, entry_info);
		break;
	case MMAP_WRITE:
		memcpy_to_pmem_nocache(entry, entry_info->data,
				sizeof(struct bytefs_mmap_entry));
		break;
	// case SNAPSHOT_INFO:
	// 	memcpy_to_pmem_nocache(entry, entry_info->data,
	// 			sizeof(struct bytefs_snapshot_info_entry));
	// 	break;
	default:
		break;
	}

	return 0;
}

static int bytefs_append_log_entry(struct super_block *sb,
	struct bytefs_inode *pi, struct inode *inode,
	struct bytefs_inode_info_header *sih,
	struct bytefs_log_entry_info *entry_info)
{
	// entry_info must be in DRAM
	// entry can be either in DRAM or logs

	void *entry, *alter_entry;
	enum bytefs_entry_type type = entry_info->type;
	struct bytefs_inode_update *update = entry_info->update;
	u64 tail, alter_tail;
	u64 curr_p, alter_curr_p;
	size_t size;
	int extended = 0;
	unsigned long irq_flags = 0;

	if (type == DIR_LOG)
		size = entry_info->file_size;
	else
		size = bytefs_get_log_entry_size(sb, type);

	tail = update->tail;
	alter_tail = update->alter_tail;

	curr_p = bytefs_get_append_head(sb, pi, sih, tail, size,
						MAIN_LOG, 0, &extended);
	if (curr_p == 0)
		return -ENOSPC;

	bytefs_dbg_verbose("%s: inode %lu attr change entry @ 0x%llx\n",
				__func__, sih->ino, curr_p);

	entry = bytefs_get_block(sb, curr_p);
	/* inode is already updated with attr */
	bytefs_memunlock_range(sb, entry, size, &irq_flags);
	memset(entry, 0, size);
	bytefs_update_log_entry(sb, inode, entry, entry_info);
	bytefs_inc_page_num_entries(sb, curr_p);
	bytefs_memlock_range(sb, entry, size, &irq_flags);
	update->curr_entry = curr_p;
	update->tail = curr_p + size;

	if (metadata_csum) {
		alter_curr_p = bytefs_get_append_head(sb, pi, sih, alter_tail,
						size, ALTER_LOG, 0, &extended);
		if (alter_curr_p == 0)
			return -ENOSPC;

		alter_entry = bytefs_get_block(sb, alter_curr_p);
		bytefs_memunlock_range(sb, alter_entry, size, &irq_flags);
		memset(alter_entry, 0, size);
		bytefs_update_log_entry(sb, inode, alter_entry, entry_info);
		bytefs_memlock_range(sb, alter_entry, size, &irq_flags);

		update->alter_entry = alter_curr_p;
		update->alter_tail = alter_curr_p + size;
	}

	entry_info->curr_p = curr_p;
	return 0;
}

int bytefs_inplace_update_log_entry(struct super_block *sb,
	struct inode *inode, void *entry,
	struct bytefs_log_entry_info *entry_info)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	enum bytefs_entry_type type = entry_info->type;
	u64 journal_tail;
	size_t size;
	int cpu;
	unsigned long irq_flags = 0;
	INIT_TIMING(update_time);

	BYTEFS_START_TIMING(update_entry_t, update_time);
	size = bytefs_get_log_entry_size(sb, type);

	if (metadata_csum) {
		bytefs_memunlock_range(sb, entry, size, &irq_flags);
		bytefs_update_log_entry(sb, inode, entry, entry_info);
		// Also update the alter inode log entry.
		bytefs_update_alter_entry(sb, entry);
		bytefs_memlock_range(sb, entry, size, &irq_flags);
		goto out;
	}

	cpu = bytefs_get_cpuid(sb);
	spin_lock(&sbi->journal_locks[cpu]);
	bytefs_memunlock_journal(sb, &irq_flags);
	journal_tail = bytefs_create_logentry_transaction(sb, entry, type, cpu);
	bytefs_update_log_entry(sb, inode, entry, entry_info);

	PERSISTENT_BARRIER();

	bytefs_commit_lite_transaction(sb, journal_tail, cpu);
	bytefs_memlock_journal(sb, &irq_flags);
	spin_unlock(&sbi->journal_locks[cpu]);
out:
	BYTEFS_END_TIMING(update_entry_t, update_time);
	return 0;
}

/* Returns new tail after append */
static int bytefs_append_setattr_entry(struct super_block *sb,
	struct bytefs_inode *pi, struct inode *inode, struct iattr *attr,
	struct bytefs_inode_update *update, u64 *last_setattr, u64 epoch_id)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode inode_copy;
	struct bytefs_log_entry_info entry_info;
	INIT_TIMING(append_time);
	int ret;

	BYTEFS_START_TIMING(append_setattr_t, append_time);
	entry_info.type = SET_ATTR;
	entry_info.attr = attr;
	entry_info.update = update;
	entry_info.epoch_id = epoch_id;
	entry_info.trans_id = sih->trans_id;

	if (bytefs_check_inode_integrity(sb, sih->ino, sih->pi_addr,
			sih->alter_pi_addr, &inode_copy, 0) < 0) {
		ret = -EIO;
		goto out;
	}

	ret = bytefs_append_log_entry(sb, pi, inode, sih, &entry_info);
	if (ret) {
		bytefs_err(sb, "%s failed\n", __func__);
		goto out;
	}

	*last_setattr = sih->last_setattr;
	sih->last_setattr = entry_info.curr_p;

out:
	BYTEFS_END_TIMING(append_setattr_t, append_time);
	return ret;
}

/* Invalidate old link change entry */
static int bytefs_invalidate_setattr_entry(struct super_block *sb,
	u64 last_setattr)
{
	struct bytefs_setattr_logentry *old_entry;
	struct bytefs_setattr_logentry *old_entryc, old_entry_copy;
	void *addr;
	int ret;

	addr = (void *)bytefs_get_block(sb, last_setattr);
	old_entry = (struct bytefs_setattr_logentry *)addr;

	if (metadata_csum == 0)
		old_entryc = old_entry;
	else {
		old_entryc = &old_entry_copy;
		if (!bytefs_verify_entry_csum(sb, old_entry, old_entryc))
			return -EIO;
	}

	/* Do not invalidate setsize entries */
	if (!old_entry_freeable(sb, old_entryc->epoch_id) ||
			(old_entryc->attr & ATTR_SIZE))
		return 0;

	ret = bytefs_invalidate_logentry(sb, old_entry, SET_ATTR, 0);

	return ret;
}

#if 0
static void setattr_copy_to_bytefs_inode(struct super_block *sb,
	struct inode *inode, struct bytefs_inode *pi, u64 epoch_id)
{
	pi->i_mode  = cpu_to_le16(inode->i_mode);
	pi->i_uid	= cpu_to_le32(i_uid_read(inode));
	pi->i_gid	= cpu_to_le32(i_gid_read(inode));
	pi->i_atime	= cpu_to_le32(inode->i_atime.tv_sec);
	pi->i_ctime	= cpu_to_le32(inode->i_ctime.tv_sec);
	pi->i_mtime	= cpu_to_le32(inode->i_mtime.tv_sec);
	pi->create_epoch_id = epoch_id;

	bytefs_update_alter_inode(sb, inode, pi);
}
#endif

static int bytefs_can_inplace_update_setattr(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u64 epoch_id)
{
	u64 last_log = 0;
	struct bytefs_setattr_logentry *entry = NULL;

	last_log = sih->last_setattr;
	if (last_log) {
		entry = (struct bytefs_setattr_logentry *)bytefs_get_block(sb,
								last_log);
		struct bytefs_setattr_logentry *entry_c;
		BYTEFS_CACHE_BYTE_ISSUE(entry, entry_c, BYTEFS_LOG_ISSUE);
		BYTEFS_DECACHE_END_BYTE_ISSUE(entry, entry_c);
		/* Do not overwrite setsize entry */
		if (entry->attr & ATTR_SIZE)
			return 0;
		if (entry->epoch_id == epoch_id)
			return 1;
	}

	return 0;
}

static int bytefs_inplace_update_setattr_entry(struct super_block *sb,
	struct inode *inode, struct bytefs_inode_info_header *sih,
	struct iattr *attr, u64 epoch_id)
{
	struct bytefs_setattr_logentry *entry = NULL;
	struct bytefs_log_entry_info entry_info;
	u64 last_log = 0;

	bytefs_dbgv("%s : Modifying last log entry for inode %lu\n",
				__func__, inode->i_ino);
	last_log = sih->last_setattr;
	entry = (struct bytefs_setattr_logentry *)bytefs_get_block(sb,
							last_log);

	entry_info.type = SET_ATTR;
	entry_info.attr = attr;
	entry_info.epoch_id = epoch_id;
	entry_info.trans_id = sih->trans_id;

	return bytefs_inplace_update_log_entry(sb, inode, entry,
					&entry_info);
}

int bytefs_handle_setattr_operation(struct super_block *sb, struct inode *inode,
	struct bytefs_inode *pi, unsigned int ia_valid, struct iattr *attr,
	u64 epoch_id)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode_update update;
	u64 last_setattr = 0;
	int ret;
	unsigned long irq_flags = 0;

	if (ia_valid & ATTR_MODE)
		sih->i_mode = inode->i_mode;

	/*
	 * Let's try to do inplace update.
	 * If there are currently no snapshots holding this inode,
	 * we can update the inode in place. If a snapshot creation
	 * is in progress, we will use the create_snapshot_epoch_id
	 * as the latest snapshot id.
	 */
	if (!(ia_valid & ATTR_SIZE) &&
			bytefs_can_inplace_update_setattr(sb, sih, epoch_id)) {
		bytefs_inplace_update_setattr_entry(sb, inode, sih,
						attr, epoch_id);
	} else {
		/* We are holding inode lock so OK to append the log */
		bytefs_dbgv("%s : Appending last log entry for inode ino = %lu\n",
				__func__, inode->i_ino);
		update.tail = update.alter_tail = 0;
		ret = bytefs_append_setattr_entry(sb, pi, inode, attr, &update,
						&last_setattr, epoch_id);
		if (ret) {
			bytefs_dbg("%s: append setattr entry failure\n",
								__func__);
			return ret;
		}

		bytefs_memunlock_inode(sb, pi, &irq_flags);
		bytefs_update_inode(sb, inode, pi, &update, 1);
		bytefs_memlock_inode(sb, pi, &irq_flags);
	}

	/* Invalidate old setattr entry */
	if (last_setattr)
		bytefs_invalidate_setattr_entry(sb, last_setattr);

	return 0;
}

/* Invalidate old link change entry */
int bytefs_invalidate_link_change_entry(struct super_block *sb,
	u64 old_link_change)
{
	struct bytefs_link_change_entry *old_entry;
	struct bytefs_link_change_entry *old_entryc, old_entry_copy;
	void *addr;
	int ret;

	if (old_link_change == 0)
		return 0;

	addr = (void *)bytefs_get_block(sb, old_link_change);
	old_entry = (struct bytefs_link_change_entry *)addr;

	if (metadata_csum == 0)
		old_entryc = old_entry;
	else {
		old_entryc = &old_entry_copy;
		if (!bytefs_verify_entry_csum(sb, old_entry, old_entryc))
			return -EIO;
	}

	if (!old_entry_freeable(sb, old_entryc->epoch_id))
		return 0;

	ret = bytefs_invalidate_logentry(sb, old_entry, LINK_CHANGE, 0);

	return ret;
}

static int bytefs_can_inplace_update_lcentry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u64 epoch_id)
{
	/* FIXME: There is a crash consistency issue with link after rename.
	 * rename will create a link change entry to the log. bytefs_link() after
	 * that will inplace update the link change entry. A crash after
	 * bytefs_append_link_change_entry() in bytefs_link() will leave the inode
	 * in inconsistent state (number of links) in this case.
	 * Currently, disable inplace update link change entry.
	 */
# if 0
	u64 last_log = 0;
	struct bytefs_link_change_entry *entry = NULL;

	last_log = sih->last_link_change;
	if (last_log) {
		entry = (struct bytefs_link_change_entry *)bytefs_get_block(sb,
								last_log);
		if (entry->epoch_id == epoch_id)
			return 1;
	}
#endif
	return 0;
}

static int bytefs_inplace_update_lcentry(struct super_block *sb,
	struct inode *inode, struct bytefs_inode_info_header *sih,
	u64 epoch_id)
{
	struct bytefs_link_change_entry *entry = NULL;
	struct bytefs_log_entry_info entry_info;
	u64 last_log = 0;

	last_log = sih->last_link_change;
	entry = (struct bytefs_link_change_entry *)bytefs_get_block(sb,
							last_log);

	entry_info.type = LINK_CHANGE;
	entry_info.epoch_id = epoch_id;
	entry_info.trans_id = sih->trans_id;

	return bytefs_inplace_update_log_entry(sb, inode, entry,
					&entry_info);
}

/* Returns new tail after append */
int bytefs_append_link_change_entry(struct super_block *sb,
	struct bytefs_inode *pi, struct inode *inode,
	struct bytefs_inode_update *update, u64 *old_linkc, u64 epoch_id)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode inode_copy;
	struct bytefs_log_entry_info entry_info;
	int ret = 0;
	INIT_TIMING(append_time);

	BYTEFS_START_TIMING(append_link_change_t, append_time);

	if (bytefs_check_inode_integrity(sb, sih->ino, sih->pi_addr,
			sih->alter_pi_addr, &inode_copy, 0) < 0) {
		ret = -EIO;
		goto out;
	}

	if (bytefs_can_inplace_update_lcentry(sb, sih, epoch_id)) {
		bytefs_inplace_update_lcentry(sb, inode, sih, epoch_id);
		update->tail = sih->log_tail;
		update->alter_tail = sih->alter_log_tail;

		*old_linkc = 0;
		sih->trans_id++;
		goto out;
	}

	entry_info.type = LINK_CHANGE;
	entry_info.update = update;
	entry_info.epoch_id = epoch_id;
	entry_info.trans_id = sih->trans_id;

	ret = bytefs_append_log_entry(sb, pi, inode, sih, &entry_info);
	if (ret) {
		bytefs_err(sb, "%s failed\n", __func__);
		goto out;
	}

	*old_linkc = sih->last_link_change;
	sih->last_link_change = entry_info.curr_p;
	sih->trans_id++;
out:
	BYTEFS_END_TIMING(append_link_change_t, append_time);
	return ret;
}

int bytefs_assign_write_entry(struct super_block *sb,
	struct bytefs_inode_info_header *sih,
	struct bytefs_file_write_entry *entry,
	struct bytefs_file_write_entry *entryc,
	bool free)
{
	struct bytefs_file_write_entry *old_entry;
	struct bytefs_file_write_entry *start_old_entry = NULL;
	void **pentry;
	unsigned long start_pgoff = entryc->pgoff;
	unsigned long start_old_pgoff = 0;
	unsigned int num = entryc->num_pages;
	unsigned int num_free = 0;
	unsigned long curr_pgoff;
	int i;
	int ret = 0;
	INIT_TIMING(assign_time);

	BYTEFS_START_TIMING(assign_t, assign_time);
	for (i = 0; i < num; i++) {
		curr_pgoff = start_pgoff + i;

		pentry = radix_tree_lookup_slot(&sih->tree, curr_pgoff);
		if (pentry) {
			old_entry = radix_tree_deref_slot(pentry);
			if (old_entry != start_old_entry) {
				if (start_old_entry && free)
					bytefs_free_old_entry(sb, sih,
							start_old_entry,
							start_old_pgoff,
							num_free, false,
							entryc->epoch_id);
				bytefs_invalidate_write_entry(sb,
						start_old_entry, 1, 0);

				start_old_entry = old_entry;
				start_old_pgoff = curr_pgoff;
				num_free = 1;
			} else {
				num_free++;
			}

			radix_tree_replace_slot(&sih->tree, pentry, entry);
		} else {
			ret = radix_tree_insert(&sih->tree, curr_pgoff, entry);
			if (ret) {
				bytefs_dbg("%s: ERROR %d\n", __func__, ret);
				goto out;
			}
		}
	}

	if (start_old_entry && free)
		bytefs_free_old_entry(sb, sih, start_old_entry,
					start_old_pgoff, num_free, false,
					entryc->epoch_id);

	bytefs_invalidate_write_entry(sb, start_old_entry, 1, 0);

out:
	BYTEFS_END_TIMING(assign_t, assign_time);

	return ret;
}

int bytefs_inplace_update_write_entry(struct super_block *sb,
	struct inode *inode, struct bytefs_file_write_entry *entry,
	struct bytefs_log_entry_info *entry_info)
{
	return bytefs_inplace_update_log_entry(sb, inode, entry,
					entry_info);
}

int bytefs_set_write_entry_updating(struct super_block *sb,
	struct bytefs_file_write_entry *entry, int set)
{
	unsigned long irq_flags = 0;
	bytefs_memunlock_range(sb, entry, sizeof(*entry), &irq_flags);
	entry->updating = set ? 1 : 0;
	bytefs_update_entry_csum(entry);
	bytefs_update_alter_entry(sb, entry);
	bytefs_memlock_range(sb, entry, sizeof(*entry), &irq_flags);

	return 0;
}

/*
 * Append a bytefs_file_write_entry to the current bytefs_inode_log_page.
 * blocknr and start_blk are pgoff.
 * We cannot update pi->log_tail here because a transaction may contain
 * multiple entries.
 */
int bytefs_append_file_write_entry(struct super_block *sb, struct bytefs_inode *pi,
	struct inode *inode, struct bytefs_file_write_entry *data,
	struct bytefs_inode_update *update)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_log_entry_info entry_info;
	INIT_TIMING(append_time);
	int ret;

	// data is memunlock-ed in update functions so no overhead will be attached here

	BYTEFS_START_TIMING(append_file_entry_t, append_time);

	bytefs_update_entry_csum(data);

	entry_info.type = FILE_WRITE;
	entry_info.update = update;
	entry_info.data = data;
	entry_info.epoch_id = data->epoch_id;
	entry_info.trans_id = data->trans_id;
	entry_info.inplace = 0;

	ret = bytefs_append_log_entry(sb, pi, inode, sih, &entry_info);
	if (ret)
		bytefs_err(sb, "%s failed\n", __func__);

	BYTEFS_END_TIMING(append_file_entry_t, append_time);
	return ret;
}

int bytefs_append_mmap_entry(struct super_block *sb, struct bytefs_inode *pi,
	struct inode *inode, struct bytefs_mmap_entry *data,
	struct bytefs_inode_update *update, struct vma_item *item)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode inode_copy;
	struct bytefs_log_entry_info entry_info;
	INIT_TIMING(append_time);
	int ret;

	BYTEFS_START_TIMING(append_mmap_entry_t, append_time);

	bytefs_update_entry_csum(data);

	entry_info.type = MMAP_WRITE;
	entry_info.update = update;
	entry_info.data = data;
	entry_info.epoch_id = data->epoch_id;

	if (bytefs_check_inode_integrity(sb, sih->ino, sih->pi_addr,
			sih->alter_pi_addr, &inode_copy, 0) < 0) {
		ret = -EIO;
		goto out;
	}

	ret = bytefs_append_log_entry(sb, pi, inode, sih, &entry_info);
	if (ret)
		bytefs_err(sb, "%s failed\n", __func__);

	item->mmap_entry = entry_info.curr_p;
out:
	BYTEFS_END_TIMING(append_mmap_entry_t, append_time);
	return ret;
}

int bytefs_append_snapshot_info_entry(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode_info *si,
	struct snapshot_info *info, struct bytefs_snapshot_info_entry *data,
	struct bytefs_inode_update *update)
{
	// struct bytefs_inode_info_header *sih = &si->header;
	// struct bytefs_inode inode_copy;
	// struct bytefs_log_entry_info entry_info;
	// INIT_TIMING(append_time);
	int ret = 0;

// 	BYTEFS_START_TIMING(append_snapshot_info_t, append_time);

// 	bytefs_update_entry_csum(data);

// 	entry_info.type = SNAPSHOT_INFO;
// 	entry_info.update = update;
// 	entry_info.data = data;
// 	entry_info.epoch_id = data->epoch_id;
// 	entry_info.inplace = 0;

// 	if (bytefs_check_inode_integrity(sb, sih->ino, sih->pi_addr,
// 			sih->alter_pi_addr, &inode_copy, 0) < 0) {
// 		ret = -EIO;
// 		goto out;
// 	}

// 	ret = bytefs_append_log_entry(sb, pi, NULL, sih, &entry_info);
// 	if (ret)
// 		bytefs_err(sb, "%s failed\n", __func__);

// 	info->snapshot_entry = entry_info.curr_p;
// out:
// 	BYTEFS_END_TIMING(append_snapshot_info_t, append_time);
	return ret;
}

int bytefs_append_dentry(struct super_block *sb, struct bytefs_inode *pi,
	struct inode *dir, struct dentry *dentry, u64 ino,
	unsigned short de_len, struct bytefs_inode_update *update,
	int link_change, u64 epoch_id)
{
	struct bytefs_inode_info *si = BYTEFS_I(dir);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode inode_copy;
	struct bytefs_log_entry_info entry_info;
	INIT_TIMING(append_time);
	int ret;

	BYTEFS_START_TIMING(append_dir_entry_t, append_time);

	entry_info.type = DIR_LOG;
	entry_info.update = update;
	entry_info.data = dentry;
	entry_info.ino = ino;
	entry_info.link_change = link_change;
	entry_info.file_size = de_len;
	entry_info.epoch_id = epoch_id;
	entry_info.trans_id = sih->trans_id;
	entry_info.inplace = 0;

	/* bytefs_inode tail pointer will be updated and we make sure all other
	 * inode fields are good before checksumming the whole structure
	 */
	if (bytefs_check_inode_integrity(sb, sih->ino, sih->pi_addr,
			sih->alter_pi_addr, &inode_copy, 0) < 0) {
		ret = -EIO;
		goto out;
	}

	ret = bytefs_append_log_entry(sb, pi, dir, sih, &entry_info);
	if (ret)
		bytefs_err(sb, "%s failed\n", __func__);

	dir->i_blocks = sih->i_blocks;
out:
	BYTEFS_END_TIMING(append_dir_entry_t, append_time);
	return ret;
}

int bytefs_update_alter_pages(struct super_block *sb, struct bytefs_inode *pi,
	u64 curr, u64 alter_curr)
{
	if (curr == 0 || alter_curr == 0 || metadata_csum == 0)
		return 0;

	while (curr && alter_curr) {
		bytefs_set_alter_page_address(sb, curr, alter_curr);
		curr = next_log_page(sb, curr);
		alter_curr = next_log_page(sb, alter_curr);
	}

	if (curr || alter_curr)
		bytefs_dbg("%s: curr 0x%llx, alter_curr 0x%llx\n",
					__func__, curr, alter_curr);

	return 0;
}

static int bytefs_coalesce_log_pages(struct super_block *sb,
	unsigned long prev_blocknr, unsigned long first_blocknr,
	unsigned long num_pages)
{
	unsigned long next_blocknr;
	u64 curr_block, next_page;
	struct bytefs_inode_log_page *curr_page;
	int i;
	unsigned long irq_flags = 0;

	if (prev_blocknr) {
		/* Link prev block and newly allocated head block */
		curr_block = bytefs_get_block_off(sb, prev_blocknr,
						BYTEFS_BLOCK_TYPE_4K);
		curr_page = (struct bytefs_inode_log_page *)
				bytefs_get_block(sb, curr_block);
		next_page = bytefs_get_block_off(sb, first_blocknr,
				BYTEFS_BLOCK_TYPE_4K);
		bytefs_memunlock_block(sb, curr_page, &irq_flags);
		bytefs_set_next_page_address(sb, curr_page, next_page, 0);
		bytefs_memlock_block(sb, curr_page, &irq_flags);
	}

	next_blocknr = first_blocknr + 1;
	curr_block = bytefs_get_block_off(sb, first_blocknr,
						BYTEFS_BLOCK_TYPE_4K);
	curr_page = (struct bytefs_inode_log_page *)
				bytefs_get_block(sb, curr_block);
	for (i = 0; i < num_pages - 1; i++) {
		next_page = bytefs_get_block_off(sb, next_blocknr,
				BYTEFS_BLOCK_TYPE_4K);
		bytefs_memunlock_block(sb, curr_page, &irq_flags);
		bytefs_set_page_num_entries(sb, curr_page, 0, 0);
		bytefs_set_page_invalid_entries(sb, curr_page, 0, 0);
		bytefs_set_next_page_address(sb, curr_page, next_page, 0);
		bytefs_memlock_block(sb, curr_page, &irq_flags);
		curr_page++;
		next_blocknr++;
	}

	/* Last page */
	bytefs_memunlock_block(sb, curr_page, &irq_flags);
	bytefs_set_page_num_entries(sb, curr_page, 0, 0);
	bytefs_set_page_invalid_entries(sb, curr_page, 0, 0);
	bytefs_set_next_page_address(sb, curr_page, 0, 1);
	bytefs_memlock_block(sb, curr_page, &irq_flags);
	return 0;
}

/* Log block resides in NVMM */
int bytefs_allocate_inode_log_pages(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long num_pages,
	u64 *new_block, int cpuid, enum bytefs_alloc_direction from_tail)
{
	unsigned long new_inode_blocknr;
	unsigned long first_blocknr;
	unsigned long prev_blocknr;
	int allocated;
	int ret_pages = 0;

	allocated = bytefs_new_log_blocks(sb, sih, &new_inode_blocknr,
			num_pages, ALLOC_NO_INIT, cpuid, from_tail);

	if (allocated <= 0) {
		bytefs_err(sb, "ERROR: no inode log page available: %d %d\n",
			num_pages, allocated);
		return allocated;
	}
	ret_pages += allocated;
	num_pages -= allocated;
	bytefs_dbg_verbose("Pi %lu: Alloc %d log blocks @ 0x%lx\n",
			sih->ino, allocated, new_inode_blocknr);

	/* Coalesce the pages */
	bytefs_coalesce_log_pages(sb, 0, new_inode_blocknr, allocated);
	first_blocknr = new_inode_blocknr;
	prev_blocknr = new_inode_blocknr + allocated - 1;

	/* Allocate remaining pages */
	while (num_pages) {
		allocated = bytefs_new_log_blocks(sb, sih,
					&new_inode_blocknr, num_pages,
					ALLOC_NO_INIT, cpuid, from_tail);

		bytefs_dbg_verbose("Alloc %d log blocks @ 0x%lx\n",
					allocated, new_inode_blocknr);
		if (allocated <= 0) {
			bytefs_dbg("%s: no inode log page available: %lu %d\n",
				__func__, num_pages, allocated);
			/* Return whatever we have */
			break;
		}
		ret_pages += allocated;
		num_pages -= allocated;
		bytefs_coalesce_log_pages(sb, prev_blocknr, new_inode_blocknr,
						allocated);
		prev_blocknr = new_inode_blocknr + allocated - 1;
	}

	*new_block = bytefs_get_block_off(sb, first_blocknr,
						BYTEFS_BLOCK_TYPE_4K);

	return ret_pages;
}

static int bytefs_initialize_inode_log(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode_info_header *sih,
	int log_id)
{
	u64 new_block;
	int allocated;
	unsigned long irq_flags = 0;
	// pi will be memunlock-ed

	allocated = bytefs_allocate_inode_log_pages(sb, sih,
					1, &new_block, ANY_CPU,
					log_id == MAIN_LOG ? 0 : 1);
	if (allocated != 1) {
		bytefs_err(sb, "%s ERROR: no inode log page available\n",
					__func__);
		return -ENOSPC;
	}


	bytefs_memunlock_inode(sb, pi, &irq_flags);
	if (log_id == MAIN_LOG) {
		pi->log_tail = new_block;
		bytefs_flush_buffer(&pi->log_tail, CACHELINE_SIZE, 0);
		pi->log_head = new_block;
		sih->log_head = sih->log_tail = new_block;
		sih->log_pages = 1;
		bytefs_flush_buffer(&pi->log_head, CACHELINE_SIZE, 1);
	} else {
		pi->alter_log_tail = new_block;
		bytefs_flush_buffer(&pi->alter_log_tail, CACHELINE_SIZE, 0);
		pi->alter_log_head = new_block;
		sih->alter_log_head = sih->alter_log_tail = new_block;
		sih->log_pages++;
		bytefs_flush_buffer(&pi->alter_log_head, CACHELINE_SIZE, 1);
	}
	bytefs_memlock_inode(sb, pi, &irq_flags);

	return 0;
}

/*
 * Extend the log.  If the log is less than EXTEND_THRESHOLD pages, double its
 * allocated size.  Otherwise, increase by EXTEND_THRESHOLD. Then, do GC.
 */
static u64 bytefs_extend_inode_log(struct super_block *sb, struct bytefs_inode *pi,
	struct bytefs_inode_info_header *sih, u64 curr_p)
{
	u64 new_block, alter_new_block = 0;
	int allocated;
	unsigned long num_pages;
	int ret;
	unsigned long irq_flags = 0;

	bytefs_dbgv("%s: inode %lu, curr 0x%llx\n", __func__, sih->ino, curr_p);

	if (curr_p == 0) {
		ret = bytefs_initialize_inode_log(sb, pi, sih, MAIN_LOG);
		if (ret)
			return 0;

		if (metadata_csum) {
			ret = bytefs_initialize_inode_log(sb, pi, sih, ALTER_LOG);
			if (ret)
				return 0;

			bytefs_memunlock_inode(sb, pi, &irq_flags);
			bytefs_update_alter_pages(sb, pi, sih->log_head,
							sih->alter_log_head);
			bytefs_memlock_inode(sb, pi, &irq_flags);
		}

		bytefs_memunlock_inode(sb, pi, &irq_flags);
		bytefs_update_inode_checksum(pi);
		bytefs_memlock_inode(sb, pi, &irq_flags);

		return sih->log_head;
	}

	num_pages = sih->log_pages >= EXTEND_THRESHOLD ?
				EXTEND_THRESHOLD : sih->log_pages;
//	bytefs_dbg("Before append log pages:\n");
//	bytefs_print_inode_log_page(sb, inode);
	allocated = bytefs_allocate_inode_log_pages(sb, sih,
					num_pages, &new_block, ANY_CPU, 0);
	bytefs_dbg_verbose("Link block %llu to block %llu\n",
					curr_p >> PAGE_SHIFT,
					new_block >> PAGE_SHIFT);
	if (allocated <= 0) {
		bytefs_err(sb, "%s ERROR: no inode log page available\n",
					__func__);
		bytefs_dbg("curr_p 0x%llx, %lu pages\n", curr_p,
					sih->log_pages);
		return 0;
	}

	if (metadata_csum) {
		allocated = bytefs_allocate_inode_log_pages(sb, sih,
				num_pages, &alter_new_block, ANY_CPU, 1);
		if (allocated <= 0) {
			bytefs_err(sb, "%s ERROR: no inode log page available\n",
					__func__);
			bytefs_dbg("curr_p 0x%llx, %lu pages\n", curr_p,
					sih->log_pages);
			return 0;
		}

		bytefs_memunlock_inode(sb, pi, &irq_flags);
		bytefs_update_alter_pages(sb, pi, new_block, alter_new_block);
		bytefs_memlock_inode(sb, pi, &irq_flags);
	}


	bytefs_inode_log_fast_gc(sb, pi, sih, curr_p,
			       new_block, alter_new_block, allocated, 0);

//	bytefs_dbg("After append log pages:\n");
//	bytefs_print_inode_log_page(sb, inode);
	/* Atomic switch to new log */
//	bytefs_switch_to_new_log(sb, pi, new_block, num_pages);

	return new_block;
}

/* For thorough GC, simply append one more page */
static u64 bytefs_append_one_log_page(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u64 curr_p)
{
	struct bytefs_inode_log_page *curr_page;
	u64 new_block;
	u64 curr_block;
	int allocated;
	unsigned long irq_flags = 0;

	allocated = bytefs_allocate_inode_log_pages(sb, sih, 1, &new_block,
							ANY_CPU, 0);
	if (allocated != 1) {
		bytefs_err(sb, "%s: ERROR: no inode log page available\n",
				__func__);
		return 0;
	}

	if (curr_p == 0) {
		curr_p = new_block;
	} else {
		/* Link prev block and newly allocated head block */
		curr_block = BLOCK_OFF(curr_p);
		curr_page = (struct bytefs_inode_log_page *)
				bytefs_get_block(sb, curr_block);
		bytefs_memunlock_block(sb, curr_page, &irq_flags);
		bytefs_set_next_page_address(sb, curr_page, new_block, 1);
		bytefs_memlock_block(sb, curr_page, &irq_flags);
	}

	return curr_p;
}

u64 bytefs_get_append_head(struct super_block *sb, struct bytefs_inode *pi,
	struct bytefs_inode_info_header *sih, u64 tail, size_t size, int log_id,
	int thorough_gc, int *extended)
{
	u64 curr_p;
	unsigned long irq_flags = 0;

	if (tail)
		curr_p = tail;
	else if (log_id == MAIN_LOG)
		curr_p = sih->log_tail;
	else
		curr_p = sih->alter_log_tail;

	if (curr_p == 0 || (is_last_entry(curr_p, size) &&
				next_log_page(sb, curr_p) == 0)) {
		if (is_last_entry(curr_p, size)) {
			bytefs_memunlock_block(sb, bytefs_get_block(sb, curr_p), &irq_flags);
			bytefs_set_next_page_flag(sb, curr_p);
			bytefs_memlock_block(sb, bytefs_get_block(sb, curr_p), &irq_flags);
		}

		/* Alternate log should not go here */
		if (log_id != MAIN_LOG)
			return 0;

		if (thorough_gc == 0) {
			curr_p = bytefs_extend_inode_log(sb, pi, sih, curr_p);
		} else {
			curr_p = bytefs_append_one_log_page(sb, sih, curr_p);
			/* For thorough GC */
			*extended = 1;
		}

		if (curr_p == 0)
			return 0;
	}

	if (is_last_entry(curr_p, size)) {
		bytefs_memunlock_block(sb, bytefs_get_block(sb, curr_p), &irq_flags);
		bytefs_set_next_page_flag(sb, curr_p);
		bytefs_memlock_block(sb, bytefs_get_block(sb, curr_p), &irq_flags);
		curr_p = next_log_page(sb, curr_p);
	}

	return curr_p;
}

int bytefs_free_contiguous_log_blocks(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u64 head)
{
	unsigned long blocknr, start_blocknr = 0;
	u64 curr_block = head;
	u8 btype = sih->i_blk_type;
	int num_free = 0;
	int freed = 0;

	while (curr_block > 0) {
		if (ENTRY_LOC(curr_block)) {
			bytefs_dbg("%s: ERROR: invalid block %llu\n",
					__func__, curr_block);
			break;
		}

		blocknr = bytefs_get_blocknr(sb, le64_to_cpu(curr_block),
				    btype);
		bytefs_dbg_verbose("%s: free page %llu\n", __func__, curr_block);
		curr_block = next_log_page(sb, curr_block);

		if (start_blocknr == 0) {
			start_blocknr = blocknr;
			num_free = 1;
		} else {
			if (blocknr == start_blocknr + num_free) {
				num_free++;
			} else {
				/* A new start */
				bytefs_free_log_blocks(sb, sih, start_blocknr,
							num_free);
				freed += num_free;
				start_blocknr = blocknr;
				num_free = 1;
			}
		}
	}
	if (start_blocknr) {
		bytefs_free_log_blocks(sb, sih, start_blocknr, num_free);
		freed += num_free;
	}

	return freed;
}

int bytefs_free_inode_log(struct super_block *sb, struct bytefs_inode *pi,
	struct bytefs_inode_info_header *sih)
{
	struct bytefs_inode *alter_pi;
	int freed = 0;
	unsigned long irq_flags = 0;
	INIT_TIMING(free_time);

	if (sih->log_head == 0 || sih->log_tail == 0)
		return 0;

	BYTEFS_START_TIMING(free_inode_log_t, free_time);

	/* The inode is invalid now, no need to fence */
	if (pi) {
		bytefs_memunlock_inode(sb, pi, &irq_flags);
		pi->log_head = pi->log_tail = 0;
		pi->alter_log_head = pi->alter_log_tail = 0;
		bytefs_update_inode_checksum(pi);
		if (metadata_csum) {
			alter_pi = (struct bytefs_inode *)bytefs_get_block(sb,
						sih->alter_pi_addr);
			if (alter_pi) {
				memcpy_to_pmem_nocache(alter_pi, pi,
						sizeof(struct bytefs_inode));
			}
		}
		bytefs_memlock_inode(sb, pi, &irq_flags);
	}

	freed = bytefs_free_contiguous_log_blocks(sb, sih, sih->log_head);
	if (metadata_csum)
		freed += bytefs_free_contiguous_log_blocks(sb, sih,
					sih->alter_log_head);

	BYTEFS_END_TIMING(free_inode_log_t, free_time);
	return 0;
}
