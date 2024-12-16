// /* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
// /*
//  *	include/linux/bytefs_fs.h - BYTEFS data structures on disk.
//  *	Copyright (C) 1999 Tigran Aivazian <tigran@veritas.com>
//  */

// #ifndef _LINUX_BYTEFS_FS_H
// #define _LINUX_BYTEFS_FS_H

// #include <linux/types.h>

// #define BYTEFS_BSIZE_BITS		9
// #define BYTEFS_BSIZE		(1<<BYTEFS_BSIZE_BITS)

// #define BYTEFS_MAGIC		0x1BADFACE
// #define BYTEFS_ROOT_INO		2
// #define BYTEFS_INODES_PER_BLOCK	8

// /* SVR4 vnode type values (bytefs_inode->i_vtype) */
// #define BYTEFS_VDIR 2L
// #define BYTEFS_VREG 1L

// /* BYTEFS inode layout on disk */
// struct bytefs_inode {
// 	__le16 i_ino;
// 	__u16 i_unused;
// 	__le32 i_sblock;
// 	__le32 i_eblock;
// 	__le32 i_eoffset;
// 	__le32 i_vtype;
// 	__le32 i_mode;
// 	__le32 i_uid;
// 	__le32 i_gid;
// 	__le32 i_nlink;
// 	__le32 i_atime;
// 	__le32 i_mtime;
// 	__le32 i_ctime;
// 	__u32 i_padding[4];
// };

// #define BYTEFS_NAMELEN		14	
// #define BYTEFS_DIRENT_SIZE		16
// #define BYTEFS_DIRS_PER_BLOCK	32

// struct bytefs_dirent {
// 	__le16 ino;
// 	char name[BYTEFS_NAMELEN];
// };

// /* BYTEFS superblock layout on disk */
// struct bytefs_super_block {
// 	__le32 s_magic;
// 	__le32 s_start;
// 	__le32 s_end;
// 	__le32 s_from;
// 	__le32 s_to;
// 	__s32 s_bfrom;
// 	__s32 s_bto;
// 	char  s_fsname[6];
// 	char  s_volume[6];
// 	__u32 s_padding[118];
// };


// #define BYTEFS_OFF2INO(offset) \
//         ((((offset) - BYTEFS_BSIZE) / sizeof(struct bytefs_inode)) + BYTEFS_ROOT_INO)

// #define BYTEFS_INO2OFF(ino) \
// 	((__u32)(((ino) - BYTEFS_ROOT_INO) * sizeof(struct bytefs_inode)) + BYTEFS_BSIZE)
// #define BYTEFS_NZFILESIZE(ip) \
//         ((le32_to_cpu((ip)->i_eoffset) + 1) -  le32_to_cpu((ip)->i_sblock) * BYTEFS_BSIZE)

// #define BYTEFS_FILESIZE(ip) \
//         ((ip)->i_sblock == 0 ? 0 : BYTEFS_NZFILESIZE(ip))

// #define BYTEFS_FILEBLOCKS(ip) \
//         ((ip)->i_sblock == 0 ? 0 : (le32_to_cpu((ip)->i_eblock) + 1) -  le32_to_cpu((ip)->i_sblock))
// #define BYTEFS_UNCLEAN(bytefs_sb, sb)	\
// 	((le32_to_cpu(bytefs_sb->s_from) != -1) && (le32_to_cpu(bytefs_sb->s_to) != -1) && !(sb->s_flags & SB_RDONLY))




// #endif	/* _LINUX_BYTEFS_FS_H */

   
//         ︿
//        / >》，———＜^｝
//       /┄┄/≠≠ ┄┄┄┄┄┄ヽ.
//      /┄┄//┄┄┄┄ ／｝┄┄ハ
//     /┄┄┄||┄┄／  ﾉ／  }┄}
//    /┄┄┄┄瓜亻 ＞ ´＜ ,'┄ﾉ
//   /┄┄┄┄|ノ\､  (フ_ノ 亻
//  │┄┄┄┄┄|  ╱ ｝｀ｽ/￣￣￣￣ /
// │┄┄┄┄┄┄┄|じ::つ / McBook /________
// ￣￣￣￣￣￣\___/_______ /

