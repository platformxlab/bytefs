/*
 * BRIEF DESCRIPTION
 *
 * Inode rebuild methods.
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
#include "inode.h"

/* entry given to this function is a copy in dram */
static void bytefs_apply_setattr_entry(struct super_block *sb,
	struct bytefs_inode_rebuild *reb,	struct bytefs_inode_info_header *sih,
	struct bytefs_setattr_logentry *entry)
{
	unsigned int data_bits = blk_type_to_shift[sih->i_blk_type];
	unsigned long first_blocknr, last_blocknr;
	loff_t start, end;
	int freed = 0;

	if (entry->entry_type != SET_ATTR)
		BUG();

	reb->i_mode	= entry->mode;
	reb->i_uid	= entry->uid;
	reb->i_gid	= entry->gid;
	reb->i_atime	= entry->atime;

	if (S_ISREG(reb->i_mode)) {
		start = entry->size;
		end = reb->i_size;

		first_blocknr = (start + (1UL << data_bits) - 1) >> data_bits;

		if (end > 0)
			last_blocknr = (end - 1) >> data_bits;
		else
			last_blocknr = 0;

		freed = bytefs_delete_file_tree(sb, sih, first_blocknr,
					last_blocknr, false, false, 0);
	}
}

/* entry given to this function is a copy in dram */
static void bytefs_apply_link_change_entry(struct super_block *sb,
	struct bytefs_inode_rebuild *reb,	struct bytefs_link_change_entry *entry)
{
	if (entry->entry_type != LINK_CHANGE)
		BUG();

	reb->i_links_count	= entry->links;
	reb->i_ctime		= entry->ctime;
	reb->i_flags		= entry->flags;
	reb->i_generation	= entry->generation;

	/* Do not flush now */
}

static void bytefs_update_inode_with_rebuild(struct super_block *sb,
	struct bytefs_inode_rebuild *reb, struct bytefs_inode *pi)
{
	pi->i_size = cpu_to_le64(reb->i_size);
	pi->i_flags = cpu_to_le32(reb->i_flags);
	pi->i_uid = cpu_to_le32(reb->i_uid);
	pi->i_gid = cpu_to_le32(reb->i_gid);
	pi->i_atime = cpu_to_le32(reb->i_atime);
	pi->i_ctime = cpu_to_le32(reb->i_ctime);
	pi->i_mtime = cpu_to_le32(reb->i_mtime);
	pi->i_generation = cpu_to_le32(reb->i_generation);
	pi->i_links_count = cpu_to_le16(reb->i_links_count);
	pi->i_mode = cpu_to_le16(reb->i_mode);
}

static int bytefs_init_inode_rebuild(struct super_block *sb,
	struct bytefs_inode_rebuild *reb, struct bytefs_inode *pi)
{
	struct bytefs_inode fake_pi;
	int rc;

	struct bytefs_inode *pi_c;

	BYTEFS_CACHE_BYTE_ISSUE(pi, pi_c, BYTEFS_INODE_ISSUE); 
	BYTEFS_DECACHE_END_BYTE_ISSUE(pi, pi_c);
	
	rc = memcpy_mcsafe(&fake_pi, pi, sizeof(struct bytefs_inode));
	if (rc)
		return rc;

	reb->i_size = le64_to_cpu(fake_pi.i_size);
	reb->i_flags = le32_to_cpu(fake_pi.i_flags);
	reb->i_uid = le32_to_cpu(fake_pi.i_uid);
	reb->i_gid = le32_to_cpu(fake_pi.i_gid);
	reb->i_atime = le32_to_cpu(fake_pi.i_atime);
	reb->i_ctime = le32_to_cpu(fake_pi.i_ctime);
	reb->i_mtime = le32_to_cpu(fake_pi.i_mtime);
	reb->i_generation = le32_to_cpu(fake_pi.i_generation);
	reb->i_links_count = le16_to_cpu(fake_pi.i_links_count);
	reb->i_mode = le16_to_cpu(fake_pi.i_mode);
	reb->trans_id = 0;

	return rc;
}

static inline void bytefs_rebuild_file_time_and_size(struct super_block *sb,
	struct bytefs_inode_rebuild *reb, u32 mtime, u32 ctime, u64 size)
{
	reb->i_mtime = cpu_to_le32(mtime);
	reb->i_ctime = cpu_to_le32(ctime);
	reb->i_size = cpu_to_le64(size);
}

static int bytefs_rebuild_inode_start(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode_info_header *sih,
	struct bytefs_inode_rebuild *reb, u64 pi_addr)
{
	int ret;

	ret = bytefs_get_head_tail(sb, pi, sih);
	if (ret)
		return ret;

	ret = bytefs_init_inode_rebuild(sb, reb, pi);
	if (ret)
		return ret;

	sih->pi_addr = pi_addr;

	bytefs_dbg_verbose("Log head 0x%llx, tail 0x%llx\n",
				sih->log_head, sih->log_tail);
	sih->log_pages = 1;

	return ret;
}

static int bytefs_rebuild_inode_finish(struct super_block *sb,
	struct bytefs_inode *pi, struct bytefs_inode_info_header *sih,
	struct bytefs_inode_rebuild *reb, u64 curr_p)
{
	struct bytefs_inode *alter_pi;
	u64 next;
	unsigned long irq_flags = 0;

	sih->i_size = le64_to_cpu(reb->i_size);
	sih->i_mode = le64_to_cpu(reb->i_mode);
	sih->i_flags = le32_to_cpu(reb->i_flags);
	sih->trans_id = reb->trans_id + 1;

	bytefs_memunlock_inode(sb, pi, &irq_flags);
	bytefs_update_inode_with_rebuild(sb, reb, pi);
	bytefs_update_inode_checksum(pi);
	if (metadata_csum) {
		alter_pi = (struct bytefs_inode *)bytefs_get_block(sb,
							sih->alter_pi_addr);
		memcpy_to_pmem_nocache(alter_pi, pi, sizeof(struct bytefs_inode));
	}
	bytefs_memlock_inode(sb, pi, &irq_flags);

	/* Keep traversing until log ends */
	curr_p &= PAGE_MASK;
	while ((next = next_log_page(sb, curr_p)) > 0) {
		sih->log_pages++;
		curr_p = next;
	}

	if (metadata_csum)
		sih->log_pages *= 2;

	return 0;
}

static int bytefs_reset_csum_parity_page(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	unsigned long pgoff, int zero)
{
	bytefs_dbgv("%s: update page off %lu\n", __func__, pgoff);

	if (data_csum)
		bytefs_update_pgoff_csum(sb, sih, entry, pgoff, zero);

	if (data_parity)
		bytefs_update_pgoff_parity(sb, sih, entry, pgoff, zero);

	return 0;
}

int bytefs_reset_csum_parity_range(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	unsigned long start_pgoff, unsigned long end_pgoff, int zero,
	int check_entry)
{
	struct bytefs_file_write_entry *curr;
	unsigned long pgoff;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	for (pgoff = start_pgoff; pgoff < end_pgoff; pgoff++) {
		if (entry && check_entry && zero == 0) {
			curr = bytefs_get_write_entry(sb, sih, pgoff);
			if (curr != entry)
				continue;
		}

		/* FIXME: For mmap, check dirty? */
		bytefs_reset_csum_parity_page(sb, sih, entry, pgoff, zero);
	}

	return 0;
}

/* Reset data csum for updating entries */
static int bytefs_reset_data_csum_parity(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_file_write_entry *entry,
	struct bytefs_file_write_entry *entryc)
{
	unsigned long end_pgoff;

	if (data_csum == 0 && data_parity == 0)
		goto out;

	if (entryc->invalid_pages == entryc->num_pages)
		/* Dead entry */
		goto out;

	end_pgoff = entryc->pgoff + entryc->num_pages;
	bytefs_reset_csum_parity_range(sb, sih, entry, entryc->pgoff,
			end_pgoff, 0, 1);

out:
	bytefs_set_write_entry_updating(sb, entry, 0);

	return 0;
}

/* Reset data csum for mmap entries */
static int bytefs_reset_mmap_csum_parity(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_mmap_entry *entry,
	struct bytefs_mmap_entry *entryc)
{
	unsigned long end_pgoff;
	int ret = 0;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	if (entryc->invalid == 1)
		/* Dead entry */
		return 0;

	end_pgoff = entryc->pgoff + entryc->num_pages;
	bytefs_reset_csum_parity_range(sb, sih, NULL, entryc->pgoff,
			end_pgoff, 0, 0);

	ret = bytefs_invalidate_logentry(sb, entry, MMAP_WRITE, 0);

	return ret;
}

int bytefs_reset_mapping_csum_parity(struct super_block *sb,
	struct inode *inode, struct address_space *mapping,
	unsigned long start_pgoff, unsigned long end_pgoff)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	pgoff_t indices[PAGEVEC_SIZE];
	struct pagevec pvec;
	bool done = false;
	int count = 0;
	unsigned long start = 0;
	INIT_TIMING(reset_time);
	int i;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	BYTEFS_START_TIMING(reset_mapping_t, reset_time);
	bytefs_dbgv("%s: pgoff %lu to %lu\n",
			__func__, start_pgoff, end_pgoff);

	while (!done) {
		pvec.nr = find_get_entries_tag(mapping, start_pgoff,
				PAGECACHE_TAG_DIRTY, PAGEVEC_SIZE,
				pvec.pages, indices);

		if (pvec.nr == 0)
			break;

		if (count == 0)
			start = indices[0];

		for (i = 0; i < pvec.nr; i++) {
			if (indices[i] >= end_pgoff) {
				done = true;
				break;
			}

			BYTEFS_STATS_ADD(dirty_pages, 1);
			bytefs_reset_csum_parity_page(sb, sih, NULL,
						indices[i], 0);
		}

		count += pvec.nr;
		if (pvec.nr < PAGEVEC_SIZE)
			break;

		start_pgoff = indices[pvec.nr - 1] + 1;
	}

	if (count)
		bytefs_dbgv("%s: inode %lu, reset %d pages, start pgoff %lu\n",
				__func__, sih->ino, count, start);

	BYTEFS_END_TIMING(reset_mapping_t, reset_time);
	return 0;
}

int bytefs_reset_vma_csum_parity(struct super_block *sb,
	struct vma_item *item)
{
	struct vm_area_struct *vma = item->vma;
	struct address_space *mapping = vma->vm_file->f_mapping;
	struct inode *inode = mapping->host;
	struct bytefs_mmap_entry *entry;
	unsigned long num_pages;
	unsigned long start_index, end_index;
	INIT_TIMING(reset_time);
	int ret = 0;

	if (data_csum == 0 && data_parity == 0)
		return 0;

	BYTEFS_START_TIMING(reset_vma_t, reset_time);
	num_pages = (vma->vm_end - vma->vm_start) >> PAGE_SHIFT;
	start_index = vma->vm_pgoff;
	end_index = vma->vm_pgoff + num_pages;

	bytefs_dbgv("%s: inode %lu, pgoff %lu - %lu\n",
			__func__, inode->i_ino, start_index, end_index);

	ret = bytefs_reset_mapping_csum_parity(sb, inode, mapping,
					start_index, end_index);

	if (item->mmap_entry) {
		entry = bytefs_get_block(sb, item->mmap_entry);
		ret = bytefs_invalidate_logentry(sb, entry, MMAP_WRITE, 0);
	}

	BYTEFS_END_TIMING(reset_vma_t, reset_time);
	return ret;
}

static void bytefs_rebuild_handle_write_entry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_inode_rebuild *reb,
	struct bytefs_file_write_entry *entry,
	struct bytefs_file_write_entry *entryc)
{
	if (entryc->num_pages != entryc->invalid_pages) {
		/*
		 * The overlaped blocks are already freed.
		 * Don't double free them, just re-assign the pointers.
		 */
		bytefs_assign_write_entry(sb, sih, entry, entryc, false);
	}

	if (entryc->trans_id >= reb->trans_id) {
		bytefs_rebuild_file_time_and_size(sb, reb,
					entryc->mtime, entryc->mtime,
					entryc->size);
		reb->trans_id = entryc->trans_id;
	}

	if (entryc->updating)
		bytefs_reset_data_csum_parity(sb, sih, entry, entryc);

	/* Update sih->i_size for setattr apply operations */
	sih->i_size = le64_to_cpu(reb->i_size);
}

static int bytefs_rebuild_file_inode_tree(struct super_block *sb,
	struct bytefs_inode *pi, u64 pi_addr,
	struct bytefs_inode_info_header *sih)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	struct bytefs_file_write_entry *entry = NULL;
	struct bytefs_setattr_logentry *attr_entry = NULL;
	struct bytefs_link_change_entry *link_change_entry = NULL;
	struct bytefs_mmap_entry *mmap_entry = NULL;
	char entry_copy[BYTEFS_MAX_ENTRY_LEN];
	struct bytefs_inode_rebuild rebuild, *reb;
	unsigned int data_bits = blk_type_to_shift[sih->i_blk_type];
	u64 ino = pi->bytefs_ino;
	INIT_TIMING(rebuild_time);
	void *addr, *entryc;
	u64 curr_p;
	u8 type;
	int ret;

	BYTEFS_START_TIMING(rebuild_file_t, rebuild_time);
	bytefs_dbg_verbose("Rebuild file inode %llu tree\n", ino);

	reb = &rebuild;
	ret = bytefs_rebuild_inode_start(sb, pi, sih, reb, pi_addr);
	if (ret)
		goto out;

	curr_p = sih->log_head;
	// if (curr_p == 0 && sih->log_tail == 0)
	// 	goto out;
	if (curr_p == 0 || sih->log_tail == 0) {
		bytefs_warn("NULL log pointer(s) in file inode %llu\n", ino);
		pi->log_head = 0;
		pi->log_tail = 0;
		bytefs_flush_buffer(pi, sizeof(struct bytefs_inode), 1);
		goto out;
	}

	entryc = (metadata_csum == 0) ? NULL : entry_copy;

//	bytefs_print_bytefs_log(sb, sih);

	while (curr_p != sih->log_tail) {
		if (goto_next_page(sb, curr_p)) {
			sih->log_pages++;
			curr_p = next_log_page(sb, curr_p);
		}

		if (curr_p == 0) {
			bytefs_err(sb, "File inode %llu log is NULL!\n", ino);
			BUG();
		}

		addr = (void *)bytefs_get_block(sb, curr_p);

		if (metadata_csum == 0)
			entryc = addr;
		else if (!bytefs_verify_entry_csum(sb, addr, entryc))
			return 0;

		type = bytefs_get_entry_type(entryc);

		// if (sbi->mount_snapshot) {
		// 	if (bytefs_encounter_mount_snapshot(sb, addr, type))
		// 		break;
		// }

		switch (type) {
		case SET_ATTR:
			attr_entry = (struct bytefs_setattr_logentry *)entryc;
			bytefs_apply_setattr_entry(sb, reb, sih, attr_entry);
			sih->last_setattr = curr_p;
			if (attr_entry->trans_id >= reb->trans_id) {
				bytefs_rebuild_file_time_and_size(sb, reb,
							attr_entry->mtime,
							attr_entry->ctime,
							attr_entry->size);
				reb->trans_id = attr_entry->trans_id;
			}

			/* Update sih->i_size for setattr operation */
			sih->i_size = le64_to_cpu(reb->i_size);
			curr_p += sizeof(struct bytefs_setattr_logentry);
			break;
		case LINK_CHANGE:
			link_change_entry =
				(struct bytefs_link_change_entry *)entryc;
			bytefs_apply_link_change_entry(sb, reb,
						link_change_entry);
			sih->last_link_change = curr_p;
			curr_p += sizeof(struct bytefs_link_change_entry);
			break;
		case FILE_WRITE:
			entry = (struct bytefs_file_write_entry *)addr;
			bytefs_rebuild_handle_write_entry(sb, sih, reb,
					entry, WENTRY(entryc));
			curr_p += sizeof(struct bytefs_file_write_entry);
			break;
		case MMAP_WRITE:
			mmap_entry = (struct bytefs_mmap_entry *)addr;
			bytefs_reset_mmap_csum_parity(sb, sih,
					mmap_entry, MMENTRY(entryc));
			curr_p += sizeof(struct bytefs_mmap_entry);
			break;
		default:
			bytefs_err(sb, "unknown type %d, 0x%llx\n", type, curr_p);
			BYTEFS_ASSERT(0);
			curr_p += sizeof(struct bytefs_file_write_entry);
			break;
		}

	}

	ret = bytefs_rebuild_inode_finish(sb, pi, sih, reb, curr_p);
	sih->i_blocks = sih->i_size >> data_bits;
	if (sih->i_size % (1 << data_bits) > 0)
		++sih->i_blocks;

out:
//	bytefs_print_inode_log_page(sb, inode);
	BYTEFS_END_TIMING(rebuild_file_t, rebuild_time);
	return ret;
}

/******************* Directory rebuild *********************/

static inline void bytefs_rebuild_dir_time_and_size(struct super_block *sb,
	struct bytefs_inode_rebuild *reb, struct bytefs_dentry *entry,
	struct bytefs_dentry *entryc)
{
	if (!entry || !reb)
		return;

	reb->i_ctime = entryc->mtime;
	reb->i_mtime = entryc->mtime;
	reb->i_links_count = entryc->links_count;
	//reb->i_size = entryc->size;
}

static void bytefs_reassign_last_dentry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, u64 curr_p)
{
	struct bytefs_dentry *dentry, *old_dentry;

	if (sih->last_dentry == 0) {
		sih->last_dentry = curr_p;
	} else {
		old_dentry = (struct bytefs_dentry *)bytefs_get_block(sb,
							sih->last_dentry);
		dentry = (struct bytefs_dentry *)bytefs_get_block(sb, curr_p);
		if (dentry->trans_id >= old_dentry->trans_id)
			sih->last_dentry = curr_p;
	}
}

static inline int bytefs_replay_add_dentry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_dentry *entry,
	struct bytefs_dentry *entryc)
{
	if (!entryc->name_len)
		return -EINVAL;

	bytefs_dbg_verbose("%s: add %s\n", __func__, entry->name);
	return bytefs_insert_dir_tree(sb, sih,
			entryc->name, entryc->name_len, entry);
}

/* entry given to this function is a copy in dram */
static inline int bytefs_replay_remove_dentry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_dentry *entry)
{
	bytefs_dbg_verbose("%s: remove %s\n", __func__, entry->name);
	bytefs_remove_dir_tree(sb, sih, entry->name,
					entry->name_len, 1, NULL);
	return 0;
}

static int bytefs_rebuild_handle_dentry(struct super_block *sb,
	struct bytefs_inode_info_header *sih, struct bytefs_inode_rebuild *reb,
	struct bytefs_dentry *entry, struct bytefs_dentry *entryc, u64 curr_p)
{
	int ret = 0;

	bytefs_dbgv("curr_p: 0x%llx, type %d, ino %llu, name %s, namelen %u, csum 0x%x, rec len %u\n",
			curr_p,
			entry->entry_type, le64_to_cpu(entry->ino),
			entry->name, entry->name_len, entry->csum,
			le16_to_cpu(entry->de_len));

	bytefs_reassign_last_dentry(sb, sih, curr_p);

	if (entryc->invalid == 0) {
		if (entryc->ino > 0)
			ret = bytefs_replay_add_dentry(sb, sih, entry, entryc);
		else
			ret = bytefs_replay_remove_dentry(sb, sih, entryc);
	}

	if (ret) {
		bytefs_err(sb, "%s ERROR %d\n", __func__, ret);
		return ret;
	}

	if (entryc->trans_id >= reb->trans_id) {
		bytefs_rebuild_dir_time_and_size(sb, reb, entry, entryc);
		reb->trans_id = entryc->trans_id;
	}

	return ret;
}

int bytefs_rebuild_dir_inode_tree(struct super_block *sb,
	struct bytefs_inode *pi, u64 pi_addr,
	struct bytefs_inode_info_header *sih)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	struct bytefs_dentry *entry = NULL;
	struct bytefs_setattr_logentry *attr_entry = NULL;
	struct bytefs_link_change_entry *lc_entry = NULL;
	char entry_copy[BYTEFS_MAX_ENTRY_LEN];
	struct bytefs_inode_rebuild rebuild, *reb;
	u64 ino = pi->bytefs_ino;
	unsigned short de_len;
	INIT_TIMING(rebuild_time);
	void *addr, *entryc;
	u64 curr_p;
	u8 type;
	int ret;

	BYTEFS_START_TIMING(rebuild_dir_t, rebuild_time);
	bytefs_dbgv("Rebuild dir %llu tree\n", ino);

	reb = &rebuild;
	ret = bytefs_rebuild_inode_start(sb, pi, sih, reb, pi_addr);
	if (ret)
		goto out;

	curr_p = sih->log_head;
	if (curr_p == 0) {
		bytefs_err(sb, "Dir %llu log is NULL!\n", ino);
		ret = -ENOSPC;
		goto out;
	}

	entryc = (metadata_csum == 0) ? NULL : entry_copy;

	while (curr_p != sih->log_tail) {
		if (goto_next_page(sb, curr_p)) {
			sih->log_pages++;
			curr_p = next_log_page(sb, curr_p);
		}

		if (curr_p == 0) {
			bytefs_err(sb, "Dir %llu log is NULL!\n", ino);
			BUG();
		}

		addr = (void *)bytefs_get_block(sb, curr_p);

		if (metadata_csum == 0)
			entryc = addr;
		else if (!bytefs_verify_entry_csum(sb, addr, entryc))
			return 0;

		type = bytefs_get_entry_type(entryc);

		if (sbi->mount_snapshot) {
			// if (bytefs_encounter_mount_snapshot(sb, addr, type))
				// break;
		}

		switch (type) {
		case SET_ATTR:
			attr_entry = (struct bytefs_setattr_logentry *)entryc;
			bytefs_apply_setattr_entry(sb, reb, sih, attr_entry);
			sih->last_setattr = curr_p;
			curr_p += sizeof(struct bytefs_setattr_logentry);
			break;
		case LINK_CHANGE:
			lc_entry = (struct bytefs_link_change_entry *)entryc;
			if (lc_entry->trans_id >= reb->trans_id) {
				bytefs_apply_link_change_entry(sb, reb, lc_entry);
				reb->trans_id = lc_entry->trans_id;
			}
			sih->last_link_change = curr_p;
			curr_p += sizeof(struct bytefs_link_change_entry);
			break;
		case DIR_LOG:
			entry = (struct bytefs_dentry *)addr;
			ret = bytefs_rebuild_handle_dentry(sb, sih, reb,
					entry, DENTRY(entryc), curr_p);
			if (ret)
				goto out;
			de_len = le16_to_cpu(DENTRY(entryc)->de_len);
			curr_p += de_len;
			break;
		default:
			bytefs_dbg("%s: unknown type %d, 0x%llx\n",
					__func__, type, curr_p);
			BYTEFS_ASSERT(0);
			break;
		}
	}

	ret = bytefs_rebuild_inode_finish(sb, pi, sih, reb, curr_p);
	sih->i_blocks = 0;

out:
//	bytefs_print_dir_tree(sb, sih, ino);
	BYTEFS_END_TIMING(rebuild_dir_t, rebuild_time);
	return ret;
}

/* initialize bytefs inode header and other DRAM data structures */
int bytefs_rebuild_inode(struct super_block *sb, struct bytefs_inode_info *si,
	u64 ino, u64 pi_addr, int rebuild_dir)
{
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode *pi;
	struct bytefs_inode inode_copy;
	u64 alter_pi_addr = 0;
	int ret;

	if (metadata_csum) {
		/* Get alternate inode address */
		ret = bytefs_get_alter_inode_address(sb, ino, &alter_pi_addr);
		if (ret)  {
			bytefs_dbg("%s: failed alt ino addr for inode %llu\n",
				 __func__, ino);
			return ret;
		}
	}

	ret = bytefs_check_inode_integrity(sb, ino, pi_addr, alter_pi_addr,
					 &inode_copy, 1);

	if (ret)
		return ret;

	pi = (struct bytefs_inode *)bytefs_get_block(sb, pi_addr);
	// We need this valid in case we need to evict the inode.

	bytefs_init_header(sb, sih, __le16_to_cpu(pi->i_mode));
	sih->pi_addr = pi_addr;

	if (pi->deleted == 1) {
		bytefs_dbg("%s: inode %llu has been deleted.\n", __func__, ino);
		return -ESTALE;
	}

	bytefs_dbgv("%s: inode %llu, addr 0x%llx, valid %d, head 0x%llx, tail 0x%llx\n",
			__func__, ino, pi_addr, pi->valid,
			pi->log_head, pi->log_tail);

	sih->ino = ino;
	sih->alter_pi_addr = alter_pi_addr;

	switch (__le16_to_cpu(pi->i_mode) & S_IFMT) {
	case S_IFLNK:
		/* Treat symlink files as normal files */
		/* Fall through */
	case S_IFREG:
		bytefs_rebuild_file_inode_tree(sb, pi, pi_addr, sih);
		break;
	case S_IFDIR:
		if (rebuild_dir)
			bytefs_rebuild_dir_inode_tree(sb, pi, pi_addr, sih);
		break;
	default:
		/* In case of special inode, walk the log */
		if (pi->log_head)
			bytefs_rebuild_file_inode_tree(sb, pi, pi_addr, sih);
		sih->pi_addr = pi_addr;
		break;
	}

	return 0;
}


/******************* Snapshot log rebuild *********************/

/* For power failure recovery, just initialize the infos */
// int bytefs_restore_snapshot_table(struct super_block *sb, int just_init)
// {
// 	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
// 	struct bytefs_snapshot_info_entry *entry = NULL;
// 	struct bytefs_inode *pi;
// 	struct bytefs_inode_info_header *sih;
// 	struct bytefs_inode_rebuild rebuild, *reb;
// 	unsigned int data_bits;
// 	char entry_copy[BYTEFS_MAX_ENTRY_LEN];
// 	size_t size = sizeof(struct bytefs_snapshot_info_entry);
// 	u64 ino = BYTEFS_SNAPSHOT_INO;
// 	INIT_TIMING(rebuild_time);
// 	int count = 0;
// 	void *addr, *entryc;
// 	u64 curr_p;
// 	u8 type;
// 	int ret;

// 	BYTEFS_START_TIMING(rebuild_snapshot_t, rebuild_time);
// 	bytefs_dbg_verbose("Rebuild snapshot table\n");

// 	entryc = (metadata_csum == 0) ? NULL : entry_copy;

// 	pi = bytefs_get_reserved_inode(sb, ino);
// 	sih = &sbi->snapshot_si->header;
// 	data_bits = blk_type_to_shift[sih->i_blk_type];
// 	reb = &rebuild;
// 	ret = bytefs_rebuild_inode_start(sb, pi, sih, reb, sih->pi_addr);
// 	if (ret)
// 		goto out;

// 	curr_p = sih->log_head;
// 	if (curr_p == 0 && sih->log_tail == 0)
// 		goto out;

// //	bytefs_print_bytefs_log(sb, sih);

// 	while (curr_p != sih->log_tail) {
// 		if (goto_next_page(sb, curr_p)) {
// 			sih->log_pages++;
// 			curr_p = next_log_page(sb, curr_p);
// 		}

// 		if (curr_p == 0) {
// 			bytefs_err(sb, "File inode %llu log is NULL!\n", ino);
// 			BUG();
// 		}

// 		addr = (void *)bytefs_get_block(sb, curr_p);

// 		if (metadata_csum == 0)
// 			entryc = addr;
// 		else if (!bytefs_verify_entry_csum(sb, addr, entryc))
// 			return 0;

// 		type = bytefs_get_entry_type(entryc);

// 		switch (type) {
// 		// case SNAPSHOT_INFO:
// 		// 	entry = (struct bytefs_snapshot_info_entry *)addr;
// 		// 	ret = bytefs_restore_snapshot_entry(sb, entry,
// 		// 				curr_p, just_init);
// 		// 	if (ret) {
// 		// 		bytefs_err(sb, "Restore entry %llu failed\n",
// 		// 			entry->epoch_id);
// 		// 		goto out;
// 		// 	}
// 		// 	if (SNENTRY(entryc)->deleted == 0)
// 		// 		count++;
// 		// 	curr_p += size;
// 		// 	break;
// 		default:
// 			bytefs_err(sb, "unknown type %d, 0x%llx\n", type, curr_p);
// 			BYTEFS_ASSERT(0);
// 			curr_p += size;
// 			break;
// 		}

// 	}

// 	ret = bytefs_rebuild_inode_finish(sb, pi, sih, reb, curr_p);
// 	sih->i_blocks = sih->i_size >> data_bits;
// 	if (sih->i_size % (1 << data_bits) > 0)
// 		++sih->i_blocks;

// out:
// //	bytefs_print_inode_log_page(sb, inode);
// 	BYTEFS_END_TIMING(rebuild_snapshot_t, rebuild_time);

// 	bytefs_dbg("Recovered %d snapshots, latest epoch ID %llu\n",
// 			count, sbi->s_epoch_id);

// 	return ret;
// }
