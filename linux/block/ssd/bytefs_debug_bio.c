/**
 * bytefs file system ext4 & bio related testing, debugging, and performance evaluation functions.
 * Only disk /dev/pmem0 will be tested.
*/
#include "bytefs_debug_bio.h"

/* headers directly copied from blk-core.c */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/debugfs.h>
#include <linux/bpf.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/ctype.h>

#include "../../ssd/ftl.h"


#define BYTE_FS_BIO_INFO_PRINTID_MAX  20000
#define BYTE_FS_BIO_PG_SIZE           4096
#define PRINT_BUF_SIZE                4096
/**
 * statically allocated variables used for debugging &
 * latency printing.
*/
static uint32_t is_ssd_init = 0;
static int32_t ext4_fill_super_times = 0;
/** If we have two concurrent request, bio layer won't have any problems since every operations
 * is for private bio struct. However, in this file, print buffer, and nvme_issue uses public data
 * structures like cur, start, which possibly needs protection.
*/
static DEFINE_SPINLOCK(bytefs_bio_lock);



/**
 * EXT4 Level Functions
*/

#if (BYTE_FS_DEBUG_BIO & BYTE_FS_VALIDATE_BIO)

static void _printbuf_flush(char* buf, int32_t* _ptr) {
	if ((*_ptr) == 0) return ;
	buf[*_ptr] = '\0';
	printk(KERN_ERR "%s\n", buf);
	(*_ptr) = 0;

}

static void _printbuf_insert(char* buf, int32_t* _ptr, char c) {
	buf[(*_ptr)++] = (isalpha(c) || isdigit(c)) ? c : ' ';
	if ((*_ptr) == PRINT_BUF_SIZE) {
		_printbuf_flush(buf, _ptr);
		(*_ptr) = 0;
	}
}


/**
 * dump a page from virt_pg_addr to (virt_pg_addr + print_size)
 * INPUT:
 * virt_pg_addr - starting address of the passed buffer
 * print_size   - bytes to print in buffer
 * SIDE-EFFECT:
 * dereferencing virt_pg_addr for print_size bytes. Must ensure those addresses are valid and
 * has been mapped using page_address
*/
static void byte_fs_bio_debug_dump_page(char* virt_pg_addr, unsigned int print_size) {
	unsigned int i;
	int32_t buf_ptr = 0;
	char* print_buf;
	printk(KERN_INFO "start dumpping page printing %u bytes************\n", print_size);
	print_buf = kzalloc(PRINT_BUF_SIZE + 1, GFP_ATOMIC);
	if (print_buf == NULL) {
		printk(KERN_ERR "%s : print_buf allocation failed\n", __func__);
		return;
	}
	for (i = 0; i < print_size; i++) {
		// printk("%c",virt_pg_addr[i]);
		_printbuf_insert(print_buf, &buf_ptr, virt_pg_addr[i]);
	}
	_printbuf_flush(print_buf, &buf_ptr);



	printk(KERN_INFO "\n end dumpping page -----------------------------------\n");
	kfree(print_buf);
}
#endif

/**
 * print the time ext4 fs fills super
*/
void byte_fs_ext4_print_fillsuper_times(void) {
#if (BYTE_FS_DEBUG_BIO & BYTE_FS_BASIC_DEBUG_BIO)
	printk(KERN_DEBUG "EXT4: ext4 super is loaded %d times\n", ++ext4_fill_super_times);
#endif
}

/**
 * print the corresponding hardware name for current bio
*/
void byte_fs_print_bio_disk(struct bio* bio) {
#if (BYTE_FS_DEBUG_BIO & BYTE_FS_BASIC_DEBUG_BIO)
	printk(KERN_ERR"bio disk operation for disk : %s\n", bio->bi_disk->disk_name);
	if (op_is_write(bio_op(bio))) {
		printk(KERN_ERR "operation write\n");
	}
	else {
		printk(KERN_ERR "operation read\n");
	}
#endif
}

/**
 * return how many times super block has been filled
*/
int32_t byte_fs_debug_bio_get_superfill_times(void) {
	return ext4_fill_super_times;
}

/**
 * check if the current written ssd is the ssd to test.
 * Only ext4 operations in /dev/pmem0 will be tested.
 * RETURN:
 * 0   - NOT test.
 * 1   - test. Current operation is in pmem0
*/
uint8_t byte_fs_checkif_test_ssd(struct bio* bio) {
#if (BYTE_FS_DEBUG_BIO & BYTE_FS_UNLIKELY)
	/* before /dev/pmem0 is mounted, if we receives writes to pmem0 : unlikely */
	if (!strncmp("pmem0", bio->bi_disk->disk_name, 3) && ext4_fill_super_times == 1 ) {
		// printk(KERN_ERR"undefined device operation before mounting %s\n", bio->bi_disk->disk_name);
		if (op_is_write(bio_op(bio))) {
			printk(KERN_ERR "operation write\n");
		}
		else {
			// printk(KERN_ERR "operation read\n");
		}
	}
#endif
	// return 0; // disable the block addressable. 
	// printk(KERN_INFO "device operation on %s\n", bio->bi_disk->disk_name);
	return !strncmp("pmem0", bio->bi_disk->disk_name, 3); /* 3 for pmem0, 1 for NUL */
}




/** Warning :
 * The following functions are NOT PROTECTED BY synchronization primitives.
 * The correctness isn't guaranteed in current version.
*/


/* A series of byte fs bio basic debug functions for ext4*/

/**
 * print information when accessing submit bio
 * SIDE-EFFECT : bio to submit must have valid bi_io_vec, bi_inline_vecs and bi_iter fields
*/
void byte_fs_print_submit_bio_info(struct bio *bio) {
#if (BYTE_FS_DEBUG_BIO&BYTE_FS_BASIC_DEBUG_BIO)
	int v_idx;
	char* virt_pg_addr;
	static int32_t print_rq_nr = 0;
#if (BYTE_FS_DEBUG_BIO & BYTE_FS_UNLIKELY)
	/* print out kernel page attribute (incomplete,\ see page-flags.h). It's unlikely you need following message */
	if (!print_rq_nr) {
		printk(KERN_INFO"print page->flags meaning:\n");
		printk(KERN_INFO"PG_locked : 0\n");
		printk(KERN_INFO"PG_referenced : 1\n");
		printk(KERN_INFO"PG_uptodate : 2\n");
		printk(KERN_INFO"PG_dirty : 3\n");
		printk(KERN_INFO"PG_lru : 4\n");
		printk(KERN_INFO"PG_active : 5\n");
		printk(KERN_INFO"PG_workingset : 6\n");
		printk(KERN_INFO"PG_waiters : 7\n");		/* Page has waiters, check its waitqueue. Must be bit #7 and in the same byte as "PG_locked" */
		printk(KERN_INFO"PG_error : 8\n");
		printk(KERN_INFO"PG_slab : 9\n");
	}
#endif

	printk(KERN_INFO "ext4 BIO:\n");
	for (v_idx = 0; v_idx < bio->bi_vcnt; v_idx++) {
		/**
		 * (linux/mm/mempoool.c, mempool_alloc(), see header for return value).
		 * Code in bounce.c:
		 * if (rw == WRITE) {
		 *		char *vto, *vfrom;
		 *
		 *		flush_dcache_page(page);
		 *
		 *		vto = page_address(to->bv_page) + to->bv_offset;
		 *		vfrom = kmap_atomic(page) + to->bv_offset;
		 *		memcpy(vto, vfrom, to->bv_len);
		 *		kunmap_atomic(vfrom);
		 *	}
		 * basically tells us the way to extract content from bio->bi_vcnt.
		*/

		printk(KERN_INFO "(/dev/pmem0 bio print request : %d)flag at %d : virtual address %llx\n", (print_rq_nr++) % BYTE_FS_BIO_INFO_PRINTID_MAX, v_idx,
		       (uint64_t)(virt_pg_addr = ((char*)page_address(bio->bi_io_vec[v_idx].bv_page) + bio->bi_io_vec[v_idx].bv_offset)) );

		/* This means each bi vector is not of the same size. Their sum of size is bio->bi_iter.bi_size */
		printk(KERN_ERR "size of each vec: %d total: %d num: %d\n", bio->bi_io_vec[v_idx].bv_len, bio->bi_iter.bi_size, bio->bi_vcnt);

#if (BYTE_FS_DEBUG_BIO & BYTE_FS_VALIDATE_BIO)
		/* validate the memory to issue using NVMe is accessible by printing all these memories out */
		byte_fs_bio_debug_dump_page(virt_pg_addr, bio->bi_io_vec[v_idx].bv_len);
#endif

	}

	printk(KERN_ERR "	Offset[inline]: %d\n", bio->bi_inline_vecs[0].bv_offset);
	printk(KERN_ERR "	Length[inline]: %d\n", bio->bi_inline_vecs[0].bv_len);

	printk(KERN_ERR "bi_sector: %ld\n", bio->bi_iter.bi_sector);
	printk(KERN_ERR "bi_size  : %d\n", bio->bi_iter.bi_size);
	printk(KERN_ERR "bi_idx   : %d\n", bio->bi_iter.bi_idx);
	printk(KERN_ERR "bi_done  : %d\n", bio->bi_iter.bi_bvec_done);
#endif
}


/**
 * issue the NVMe operation to 2B-SSD from bio level.
 * Each bio vectors is treated as targeting different memory region, although from origin code implemented by lyq, 
 * it's not necessary.
 * 
 * RETURN:
 * number of 4KB pages issued through one call of byte_fs_bio_issue()
 * 
 * SIDE-EFFECT: 
 * spinlock might cause deadlock (not fully tested)
 * 
 * NOTE : 
 * The nvme issue is protected (in current version) by spinlock with irq save since the context is not clear.
 * The necessity of spinlock is unsure, and open to further testing.
 * 
*/
int byte_fs_bio_issue(struct bio* bio){
	int v_idx;
	char* virt_pg_addr;
	uint64_t lba=bio->bi_iter.bi_sector/8;
	uint64_t total_issued_bytes=0;
	uint64_t flags;
	unsigned long* if_end_io;
	unsigned short tot_vcnt = bio->bi_vcnt; // used to eliminate danger after bi_end_io
	int ret=0,fret;
	// static int32_t allow_exec_times=0;
	
	// printk_ratelimited(KERN_NOTICE "start sec addr %lu  sec size %lu\n",bio->bi_iter.bi_sector,bio->bi_iter.bi_size);
	/* if emulated device isn't ready : not issue */
	if(is_ssd_init==0){
		byte_fs_debug_bio_init_ssd();
	}
	// if(bio_flagged(bio, BIO_CHAIN)){
	// 	printk(KERN_ERR"Hi, Chaining\n");
	// }

	/* TODO : start temp code */
	
	// if(allow_exec_times++>20){
	// 	return 0;
	// }
	/* end temp code */

	if(op_is_discard(bio_op(bio)) || (bio_op(bio) == REQ_OP_SECURE_ERASE) ){
		// nvme_issue_delete();
		bio->bi_status = BLK_STS_NOTSUPP;
		bio_endio(bio);
		return -1;
	}

	// check sector addressable. If there is sector aligned request, block all io call for this bio, and return directly.
	for (v_idx = 0; v_idx < tot_vcnt; v_idx++) {
		// if sector aligned, re-start : TO DO , optimize this to not restart 
		if(bio->bi_io_vec[v_idx].bv_len % 4096){
			uint64_t lsa = bio->bi_iter.bi_sector;
			ret = 0;
			total_issued_bytes = 0;
			for(v_idx = 0; v_idx < bio->bi_vcnt; v_idx++) {
				spin_lock_irqsave(&bytefs_bio_lock,flags);
				if(0 > (fret = nvme_issue_sector_wait(op_is_write(bio_op(bio)), lsa, bio->bi_io_vec[v_idx].bv_len / 512,
				(virt_pg_addr = ((char*)page_address(bio->bi_io_vec[v_idx].bv_page) + bio->bi_io_vec[v_idx].bv_offset)), bio)
				)) {
					spin_unlock_irqrestore(&bytefs_bio_lock,flags);
					printk(KERN_ERR "NVMe sector issue failed\n");
					return fret;

				} else if (fret==-EBUSY){
					spin_unlock_irqrestore(&bytefs_bio_lock,flags);
					printk(KERN_ERR "NVMe sector issue busy\n");
					return fret;
				}
				spin_unlock_irqrestore(&bytefs_bio_lock,flags);
				ret += fret;
				lsa += bio->bi_io_vec[v_idx].bv_len / 512;
				total_issued_bytes += bio->bi_io_vec[v_idx].bv_len;
				if(total_issued_bytes != bio->bi_iter.bi_size){
					printk(KERN_ERR "sector NVMe issued size not aligned to information stored in bio\n");
					return -1;
				}
			}
			// bio->bi_end_io(bio);
			bio_endio(bio);
			return ret;
			// printk(KERN_ERR"%s: issue size not multiples of 4K page size current piece=%d total=%d\n",__func__,bio->bi_io_vec[v_idx].bv_len,bio->bi_iter.bi_size);
			// total_issued_bytes+=bio->bi_io_vec[v_idx].bv_len;
			// return -1;
		}
	}

	// allocate space for bio vector left, that is "unslept" (meaning not freed)

	if_end_io = kmalloc(sizeof(*if_end_io),GFP_ATOMIC);
	*if_end_io = tot_vcnt;

	if(tot_vcnt < 1){
		kfree(if_end_io);
		bio_endio(bio);
		printk("EMPTY BIO\n");
		return 0;
	}


	// 4K page aligned. Current version not blocking if BYTE_FS_BLOCK_BIO is NOT set

	for (v_idx = 0; v_idx < tot_vcnt; v_idx++) {
		
		
	#if (BYTE_FS_DEBUG_BIO & BYTE_FS_VALIDATE_BIO)
		/* locking for debugging */
		if(!spin_trylock(&bytefs_bio_lock)){
			printk(KERN_ERR "concurrency exists\n");
		}else{
			spin_unlock(&bytefs_bio_lock);
		}
	#endif

		spin_lock_irqsave(&bytefs_bio_lock,flags);
	#if ( BYTE_FS_DEBUG_BIO & BYTE_FS_BLOCK_BIO )
		if(0 >
			(fret = nvme_issue_wait(
				op_is_write(bio_op(bio)), lba, bio->bi_io_vec[v_idx].bv_len/4096, 
				(virt_pg_addr = ((char*)page_address(bio->bi_io_vec[v_idx].bv_page) + bio->bi_io_vec[v_idx].bv_offset))
			,bio))
		){
			spin_unlock_irqrestore(&bytefs_bio_lock, flags);
			printk(KERN_ERR "NVMe issue failed\n");
			// bio->bi_end_io(bio);
			bio_endio(bio);
			return -1;
		}
	#else
		if(0 >
			(fret = nvme_issue_nowait(
				op_is_write(bio_op(bio)), lba, bio->bi_io_vec[v_idx].bv_len/4096, 
				(virt_pg_addr = ((char*)page_address(bio->bi_io_vec[v_idx].bv_page) + bio->bi_io_vec[v_idx].bv_offset))
			,bio, if_end_io))
		){
			spin_unlock_irqrestore(&bytefs_bio_lock, flags);
			printk(KERN_ERR "NVMe issue failed\n");
			// bio->bi_end_io(bio);
			bio_endio(bio);
			return -1;
		}

	#endif


		ret += fret;

		if(v_idx + 1 != tot_vcnt){ // if not the last bio vector
			lba += bio->bi_io_vec[v_idx].bv_len / 4096;
		}

		spin_unlock_irqrestore(&bytefs_bio_lock, flags);
	#if (BYTE_FS_DEBUG_BIO & BYTE_FS_BLOCK_BIO)
		total_issued_bytes += bio->bi_io_vec[v_idx].bv_len;
	#endif
		

	#if (BYTE_FS_DEBUG_BIO & BYTE_FS_VALIDATE_BIO)
		/* validate the memory to issue using NVMe is accessible by printing all these memories out */
		byte_fs_bio_debug_dump_page(virt_pg_addr, bio->bi_io_vec[v_idx].bv_len);
	#endif


	}


	#if (BYTE_FS_DEBUG_BIO & BYTE_FS_BLOCK_BIO)
	if(total_issued_bytes!=bio->bi_iter.bi_size){
		printk(KERN_ERR "FATAL BUG : total NVMe issued operations length error\n");
		return -1;
	}
	// bio->bi_end_io(bio);
	bio_endio(bio);
	#endif

	return ret;


}	

/**
 * initialize ssd wrapper function. Only invoking ssd_init() once.
*/
void byte_fs_debug_bio_init_ssd(void) {
#if (BYTE_FS_DEBUG_BIO & BYTE_FS_BASIC_DEBUG_BIO)
	printk(KERN_INFO "start debugging init ssd\n");
#endif

	/* ssd_init will be initialized at most once. This function won't be called as long as we don't deliberately mount ext4 to a disk other than root directory */
	if (is_ssd_init) return;
#if (BYTE_FS_DEBUG_BIO & BYTE_FS_BASIC_DEBUG_BIO)
	printk(KERN_INFO "init ssd\n");
#endif
	is_ssd_init = 1;
	ssd_init();
}

uint32_t get_ssd_init(void){
	return is_ssd_init;
}







