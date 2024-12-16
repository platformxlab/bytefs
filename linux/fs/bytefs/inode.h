#ifndef __INODE_H
#define __INODE_H

struct bytefs_inode_info_header;
struct bytefs_inode;

#include "super.h"
#include "log.h"

enum bytefs_new_inode_type {
	TYPE_CREATE = 0,
	TYPE_MKNOD,
	TYPE_SYMLINK,
	TYPE_MKDIR
};


/*
 * Structure of an inode in PMEM
 * Keep the inode size to within 120 bytes: We use the last eight bytes
 * as inode table tail pointer.
 */
struct bytefs_inode {

	/* first 40 bytes */
	u8	i_rsvd;		 /* reserved. used to be checksum */
	u8	valid;		 /* Is this inode valid? */
	u8	deleted;	 /* Is this inode deleted? */
	u8	i_blk_type;	 /* data block size this inode uses */
	__le32	i_flags;	 /* Inode flags */
	__le64	i_size;		 /* Size of data in bytes */
	__le32	i_ctime;	 /* Inode modification time */
	__le32	i_mtime;	 /* Inode b-tree Modification time */
	__le32	i_atime;	 /* Access time */
	__le16	i_mode;		 /* File mode */
	__le16	i_links_count;	 /* Links count */

	__le64	i_xattr;	 /* Extended attribute block */

	/* second 40 bytes */
	__le32	i_uid;		 /* Owner Uid */
	__le32	i_gid;		 /* Group Id */
	__le32	i_generation;	 /* File version (for NFS) */
	__le32	i_create_time;	 /* Create time */
	__le64	bytefs_ino;	 /* bytefs inode number */

	__le64	log_head;	 /* Log head pointer */
	__le64	log_tail;	 /* Log tail pointer */

	/* last 40 bytes */
	__le64	alter_log_head;	 /* Alternate log head pointer */
	__le64	alter_log_tail;	 /* Alternate log tail pointer */

	__le64	create_epoch_id; /* Transaction ID when create */
	__le64	delete_epoch_id; /* Transaction ID when deleted */

	struct {
		__le32 rdev;	 /* major/minor # */
	} dev;			 /* device inode */

	__le32	csum;            /* CRC32 checksum */

	/* Leave 8 bytes for inode table tail pointer */
} __attribute((__packed__));

/*
 * Inode table.  It's a linked list of pages.
 */
struct inode_table {
	__le64 log_head;
};

/*
 * BYTEFS-specific inode state kept in DRAM
 */
struct bytefs_inode_info_header {
	/* Map from file offsets to write log entries. */
	struct radix_tree_root tree;
	struct rb_root rb_tree;		/* RB tree for directory */
	struct rb_root vma_tree;	/* Write vmas */
	struct list_head list;		/* SB list of mmap sih */
	int num_vmas;
	unsigned short i_mode;		/* Dir or file? */
	unsigned int i_flags;
	unsigned long log_pages;	/* Num of log pages */
	unsigned long i_size;
	unsigned long i_blocks;
	unsigned long ino;
	unsigned long pi_addr;
	unsigned long alter_pi_addr;
	unsigned long valid_entries;	/* For thorough GC */
	unsigned long num_entries;	/* For thorough GC */
	u64 last_setattr;		/* Last setattr entry */
	u64 last_link_change;		/* Last link change entry */
	u64 last_dentry;		/* Last updated dentry */
	u64 trans_id;			/* Transaction ID */
	u64 log_head;			/* Log head pointer */
	u64 log_tail;			/* Log tail pointer */
	u64 alter_log_head;		/* Alternate log head pointer */
	u64 alter_log_tail;		/* Alternate log tail pointer */
	u8  i_blk_type;
};

/* For rebuild purpose, temporarily store pi infomation */
struct bytefs_inode_rebuild {
	u64	i_size;
	u32	i_flags;	/* Inode flags */
	u32	i_ctime;	/* Inode modification time */
	u32	i_mtime;	/* Inode b-tree Modification time */
	u32	i_atime;	/* Access time */
	u32	i_uid;		/* Owner Uid */
	u32	i_gid;		/* Group Id */
	u32	i_generation;	/* File version (for NFS) */
	u16	i_links_count;	/* Links count */
	u16	i_mode;		/* File mode */
	u64	trans_id;
};

/*
 * DRAM state for inodes
 */
struct bytefs_inode_info {
	struct bytefs_inode_info_header header;
	struct inode vfs_inode;
};


static inline struct bytefs_inode_info *BYTEFS_I(struct inode *inode)
{
	return container_of(inode, struct bytefs_inode_info, vfs_inode);
}

static inline struct bytefs_inode_info_header *BYTEFS_IH(struct inode *inode)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	return &si->header;
}

static inline struct bytefs_inode *bytefs_get_alter_inode(struct super_block *sb,
	struct inode *inode)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode fake_pi;
	void *addr;
	int rc;

	if (metadata_csum == 0)
		return NULL;

	addr = bytefs_get_block(sb, sih->alter_pi_addr);


	void* sim_ptr;

	BYTEFS_CACHE_BYTE_ALLOC_LEN(addr, sim_ptr, sizeof(struct bytefs_inode), BYTEFS_INODE_ISSUE);
	BYTEFS_DECACHE_END_BYTE_ISSUE(addr, sim_ptr);

	rc = memcpy_mcsafe(&fake_pi, addr, sizeof(struct bytefs_inode));
	if (rc)
		return NULL;

	return (struct bytefs_inode *)addr;
}

static inline int bytefs_update_alter_inode(struct super_block *sb,
	struct inode *inode, struct bytefs_inode *pi)
{
	struct bytefs_inode *alter_pi;

	if (metadata_csum == 0)
		return 0;

	alter_pi = bytefs_get_alter_inode(sb, inode);
	if (!alter_pi)
		return -EINVAL;

	memcpy_to_pmem_nocache(alter_pi, pi, sizeof(struct bytefs_inode));
	return 0;
}


static inline int bytefs_update_inode_checksum(struct bytefs_inode *pi)
{
	u32 crc = 0;

	if (metadata_csum == 0)
		goto persist;

	crc = bytefs_crc32c(~0, (__u8 *)pi,
			(sizeof(struct bytefs_inode) - sizeof(__le32)));

	pi->csum = crc;
persist:
	bytefs_flush_buffer(pi, sizeof(struct bytefs_inode), 1);
	return 0;
}

static inline int bytefs_check_inode_checksum(struct bytefs_inode *pi)
{
	u32 crc = 0;

	if (metadata_csum == 0)
		return 0;

	crc = bytefs_crc32c(~0, (__u8 *)pi,
			(sizeof(struct bytefs_inode) - sizeof(__le32)));

	if (pi->csum == cpu_to_le32(crc))
		return 0;
	else
		return 1;
}



static inline void bytefs_update_tail(struct bytefs_inode *pi, u64 new_tail)
{
	INIT_TIMING(update_time);

	BYTEFS_START_TIMING(update_tail_t, update_time);

	PERSISTENT_BARRIER();
	pi->log_tail = new_tail;
	bytefs_flush_buffer(&pi->log_tail, CACHELINE_SIZE, 1);

	BYTEFS_END_TIMING(update_tail_t, update_time);
}

static inline void bytefs_update_alter_tail(struct bytefs_inode *pi, u64 new_tail)
{
	INIT_TIMING(update_time);

	if (metadata_csum == 0)
		return;

	BYTEFS_START_TIMING(update_tail_t, update_time);

	PERSISTENT_BARRIER();
	pi->alter_log_tail = new_tail;
	bytefs_flush_buffer(&pi->alter_log_tail, CACHELINE_SIZE, 1);

	BYTEFS_END_TIMING(update_tail_t, update_time);
}



/* Update inode tails and checksums */
static inline void bytefs_update_inode(struct super_block *sb,
	struct inode *inode, struct bytefs_inode *pi,
	struct bytefs_inode_update *update, int update_alter)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;

	sih->log_tail = update->tail;
	sih->alter_log_tail = update->alter_tail;
	bytefs_update_tail(pi, update->tail);
	if (metadata_csum)
		bytefs_update_alter_tail(pi, update->alter_tail);

	bytefs_update_inode_checksum(pi);
	if (inode && update_alter)
		bytefs_update_alter_inode(sb, inode, pi);
}


static inline
struct inode_table *bytefs_get_inode_table(struct super_block *sb,
	int version, int cpu)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	int table_start;

	if (cpu >= sbi->cpus)
		return NULL;

	if ((version & 0x1) == 0)
		table_start = INODE_TABLE0_START;
	else
		table_start = INODE_TABLE1_START;

	return (struct inode_table *)((char *)bytefs_get_block(sb,
		BYTEFS_DEF_BLOCK_SIZE_4K * table_start) +
		cpu * CACHELINE_SIZE);
}

static inline unsigned int
bytefs_inode_blk_shift(struct bytefs_inode_info_header *sih)
{
	return blk_type_to_shift[sih->i_blk_type];
}

static inline uint32_t bytefs_inode_blk_size(struct bytefs_inode_info_header *sih)
{
	return blk_type_to_size[sih->i_blk_type];
}

static inline u64 bytefs_get_reserved_inode_addr(struct super_block *sb,
	u64 inode_number)
{
	return (BYTEFS_DEF_BLOCK_SIZE_4K * RESERVE_INODE_START) +
			inode_number * BYTEFS_INODE_SIZE;
}

static inline u64 bytefs_get_alter_reserved_inode_addr(struct super_block *sb,
	u64 inode_number)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);

	return bytefs_get_addr_off(sbi, sbi->replica_reserved_inodes_addr) +
			inode_number * BYTEFS_INODE_SIZE;
}

static inline struct bytefs_inode *bytefs_get_reserved_inode(struct super_block *sb,
	u64 inode_number)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	u64 addr;

	addr = bytefs_get_reserved_inode_addr(sb, inode_number);

	return (struct bytefs_inode *)(sbi->virt_addr + addr);
}

static inline struct bytefs_inode *
bytefs_get_alter_reserved_inode(struct super_block *sb,
	u64 inode_number)
{
	struct bytefs_sb_info *sbi = BYTEFS_SB(sb);
	u64 addr;

	addr = bytefs_get_alter_reserved_inode_addr(sb, inode_number);

	return (struct bytefs_inode *)(sbi->virt_addr + addr);
}

/* If this is part of a read-modify-write of the inode metadata,
 * bytefs_memunlock_inode() before calling!
 */
static inline struct bytefs_inode *bytefs_get_inode_by_ino(struct super_block *sb,
						  u64 ino)
{
	if (ino == 0 || ino >= BYTEFS_NORMAL_INODE_START)
		return NULL;

	return bytefs_get_reserved_inode(sb, ino);
}

// haor2 : for any caller, we will not add another overhead because we added it here
static inline struct bytefs_inode *bytefs_get_inode(struct super_block *sb,
	struct inode *inode)
{
	struct bytefs_inode_info *si = BYTEFS_I(inode);
	struct bytefs_inode_info_header *sih = &si->header;
	struct bytefs_inode fake_pi;
	void *addr;
	int rc;

	addr = bytefs_get_block(sb, sih->pi_addr);

	// void* sim_ptr;




	// rc = memcpy_mcsafe(&fake_pi, addr, sizeof(struct bytefs_inode));
	// if (rc)
	// 	return NULL;

	return (struct bytefs_inode *)addr;
}



extern const struct address_space_operations bytefs_aops_dax;
int bytefs_init_inode_inuse_list(struct super_block *sb);
extern int bytefs_init_inode_table(struct super_block *sb);
int bytefs_get_alter_inode_address(struct super_block *sb, u64 ino,
	u64 *alter_pi_addr);
unsigned long bytefs_get_last_blocknr(struct super_block *sb,
	struct bytefs_inode_info_header *sih);
int bytefs_get_inode_address(struct super_block *sb, u64 ino, int version,
	u64 *pi_addr, int extendable, int extend_alternate);
int bytefs_set_blocksize_hint(struct super_block *sb, struct inode *inode,
	struct bytefs_inode *pi, loff_t new_size);
extern struct inode *bytefs_iget(struct super_block *sb, unsigned long ino);
extern void bytefs_evict_inode(struct inode *inode);
extern int bytefs_write_inode(struct inode *inode, struct writeback_control *wbc);
extern void bytefs_dirty_inode(struct inode *inode, int flags);
extern int bytefs_notify_change(struct dentry *dentry, struct iattr *attr);
extern int bytefs_getattr(const struct path *path, struct kstat *stat,
			u32 request_mask, unsigned int query_flags);
extern void bytefs_set_inode_flags(struct inode *inode, struct bytefs_inode *pi,
	unsigned int flags);
extern unsigned long bytefs_find_region(struct inode *inode, loff_t *offset,
		int hole);
int bytefs_delete_file_tree(struct super_block *sb,
	struct bytefs_inode_info_header *sih, unsigned long start_blocknr,
	unsigned long last_blocknr, bool delete_nvmm,
	bool delete_dead, u64 trasn_id);
u64 bytefs_new_bytefs_inode(struct super_block *sb, u64 *pi_addr);
extern struct inode *bytefs_new_vfs_inode(enum bytefs_new_inode_type,
	struct inode *dir, u64 pi_addr, u64 ino, umode_t mode,
	size_t size, dev_t rdev, const struct qstr *qstr, u64 epoch_id);

#endif
