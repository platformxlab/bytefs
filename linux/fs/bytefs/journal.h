#ifndef __JOURNAL_H
#define __JOURNAL_H

#include <linux/types.h>
#include <linux/fs.h>
#include "bytefs.h"
#include "super.h"


/* ======================= Lite journal ========================= */

#define BYTEFS_MAX_JOURNAL_LENGTH 128

#define	JOURNAL_INODE	1
#define	JOURNAL_ENTRY	2

/* Lightweight journal entry */
struct bytefs_lite_journal_entry {
	__le64 type;       // JOURNAL_INODE or JOURNAL_ENTRY
	__le64 data1;
	__le64 data2;
	__le32 padding;
	__le32 csum;
} __attribute((__packed__));

/* Head and tail pointers into a circular queue of journal entries.  There's
 * one of these per CPU.
 */
struct journal_ptr_pair {
	__le64 journal_head;
	__le64 journal_tail;
};

static inline
struct journal_ptr_pair *bytefs_get_journal_pointers(struct super_block *sb,
	int cpu)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	if (cpu >= sbi->cpus)
		BUG();

	return (struct journal_ptr_pair *)((char *)bytefs_get_block(sb,
		BYTEFS_DEF_BLOCK_SIZE_4K * JOURNAL_START) + cpu * CACHELINE_SIZE);
}


u64 bytefs_create_inode_transaction(struct super_block *sb,
	struct inode *inode, struct inode *dir, int cpu,
	int new_inode, int invalidate);
u64 bytefs_create_rename_transaction(struct super_block *sb,
	struct inode *old_inode, struct inode *old_dir, struct inode *new_inode,
	struct inode *new_dir, struct bytefs_dentry *father_entry, struct bytefs_dentry *new_dentry, 
	struct bytefs_dentry *old_dentry, int invalidate_new_inode, int cpu);
u64 bytefs_create_logentry_transaction(struct super_block *sb,
	void *entry, enum bytefs_entry_type type, int cpu);
void bytefs_commit_lite_transaction(struct super_block *sb, u64 tail, int cpu);
int bytefs_lite_journal_soft_init(struct super_block *sb);
int bytefs_lite_journal_hard_init(struct super_block *sb);

#endif
