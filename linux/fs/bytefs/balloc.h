#ifndef __BALLOC_H
#define __BALLOC_H

#include "inode.h"

/* DRAM structure to hold a list of free PMEM blocks */
struct free_list {
	spinlock_t s_lock;
	struct rb_root	block_free_tree;
	struct bytefs_range_node *first_node; // lowest address free range
	struct bytefs_range_node *last_node; // highest address free range

	int		index; // Which CPU do I belong to?

	/* Where are the data checksum blocks */
	unsigned long	csum_start;
	unsigned long	replica_csum_start;
	unsigned long	num_csum_blocks;

	/* Where are the data parity blocks */
	unsigned long	parity_start;
	unsigned long	replica_parity_start;
	unsigned long	num_parity_blocks;

	/* Start and end of allocatable range, inclusive. Excludes csum and
	 * parity blocks.
	 */
	unsigned long	block_start;
	unsigned long	block_end;

	unsigned long	num_free_blocks;

	/* How many nodes in the rb tree? */
	unsigned long	num_blocknode;

	u32		csum;		/* Protect integrity */

	/* Statistics */
	unsigned long	alloc_log_count;
	unsigned long	alloc_data_count;
	unsigned long	free_log_count;
	unsigned long	free_data_count;
	unsigned long	alloc_log_pages;
	unsigned long	alloc_data_pages;
	unsigned long	freed_log_pages;
	unsigned long	freed_data_pages;

	u64		padding[8];	/* Cache line break */
};

static inline
struct free_list *bytefs_get_free_list(struct super_block *sb, int cpu)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	return &sbi->free_lists[cpu];
}

enum bytefs_alloc_direction {ALLOC_FROM_HEAD = 0,
			   ALLOC_FROM_TAIL = 1};

enum bytefs_alloc_init {ALLOC_NO_INIT = 0,
		      ALLOC_INIT_ZERO = 1};

enum alloc_type {
	LOG = 1,
	DATA,
};


/* Range node type */
enum node_type {
	NODE_BLOCK = 1,
	NODE_INODE,
	NODE_DIR,
};



int bytefs_alloc_block_free_lists(struct super_block *sb);
void bytefs_delete_free_lists(struct super_block *sb);
struct bytefs_range_node *bytefs_alloc_blocknode(struct super_block *sb);
struct bytefs_range_node *bytefs_alloc_inode_node(struct super_block *sb);
struct bytefs_range_node *bytefs_alloc_dir_node(struct super_block *sb);
struct vma_item *bytefs_alloc_vma_item(struct super_block *sb);
void bytefs_free_range_node(struct bytefs_range_node *node);
void bytefs_free_snapshot_info(struct snapshot_info *info);
void bytefs_free_blocknode(struct bytefs_range_node *bnode);
void bytefs_free_inode_node(struct bytefs_range_node *bnode);
void bytefs_free_dir_node(struct bytefs_range_node *bnode);
void bytefs_free_vma_item(struct super_block *sb,
	struct vma_item *item);
extern void bytefs_init_blockmap(struct super_block *sb, int recovery);
extern int bytefs_free_data_blocks(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long blocknr, int num);
extern int bytefs_free_log_blocks(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long blocknr, int num);
extern int bytefs_new_data_blocks(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long *blocknr,
	unsigned long start_blk, unsigned int num,
	enum bytefs_alloc_init zero, int cpu,
	enum bytefs_alloc_direction from_tail);
extern int bytefs_new_log_blocks(struct super_block *sb,
	struct bytefs_inode_info_header *sih,
	unsigned long *blocknr, unsigned int num,
	enum bytefs_alloc_init zero, int cpu,
	enum bytefs_alloc_direction from_tail);
extern unsigned long bytefs_count_free_blocks(struct super_block *sb);
int bytefs_search_inodetree(struct bytefs_sb_info *sbi,
	unsigned long ino, struct bytefs_range_node **ret_node);
int bytefs_insert_blocktree(struct rb_root *tree,
	struct bytefs_range_node *new_node);
int bytefs_insert_inodetree(struct bytefs_sb_info *sbi,
	struct bytefs_range_node *new_node, int cpu);
int bytefs_find_free_slot(struct rb_root *tree, unsigned long range_low,
	unsigned long range_high, struct bytefs_range_node **prev,
	struct bytefs_range_node **next);

extern int bytefs_insert_range_node(struct rb_root *tree,
	struct bytefs_range_node *new_node, enum node_type type);
extern int bytefs_find_range_node(struct rb_root *tree,
	unsigned long key, enum node_type type,
	struct bytefs_range_node **ret_node);
extern void bytefs_destroy_range_node_tree(struct super_block *sb,
	struct rb_root *tree);
#endif
