#ifndef BYTEFS_DEBUG_H
#define BYTEFS_DEBUG_H

#include <linux/blk_types.h>

extern void byte_fs_print_submit_bio_info(struct bio *bio);

extern void byte_fs_debug_bio_init_ssd(void);

extern int32_t byte_fs_debug_bio_get_superfill_times(void);

extern uint8_t byte_fs_checkif_test_ssd(struct bio* bio);

extern void byte_fs_print_bio_disk(struct bio* bio);

extern void byte_fs_ext4_print_fillsuper_times(void);

extern int byte_fs_bio_issue(struct bio* bio);

extern uint32_t get_ssd_init(void);

#define BYTE_FS_NO_DEBUG_BIO     0     /* base of debugging code */
#define BYTE_FS_BASIC_DEBUG_BIO (1<<0) /* print out basic debugging */
#define BYTE_FS_VALIDATE_BIO    (1<<1) /* print out memory validation */
#define BYTE_FS_UNLIKELY        (1<<2) /* print out unlikely info */
#define BYTE_FS_BLOCK_BIO       (1<<3) /* bio interface still block */
#define BYTE_FS_DEBUG_BIO       (BYTE_FS_NO_DEBUG_BIO | BYTE_FS_BASIC_DEBUG_BIO  )

#endif

