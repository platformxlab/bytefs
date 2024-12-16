#include <linux/random.h>

#include "bytefs_utils.h"

/* a series of macro for debug : reduce printk number in pre-compilation level */
#define TEST_SSD_NODEBUG        0
#define TEST_SSD_RANDRW_VERIFY  1
#define TEST_SSD_DEBUG          TEST_SSD_RANDRW_VERIFY

#define TEST_SSD_RANDW_LOC_NUM  1280
#define TEST_SSD_RAND_ROUNDS    50

extern struct ssd gdev;

/**
 * verify 2B-SSD buffer byte addressible buffer read  
 * INPUT :
 * buf1,buf2     - one is read, one is to compare with read
 * size 	- buffer size
 * RETURN :
*/
uint32_t verify(void *buf1, void *buf2, uint64_t size) {
	int i;
	uint64_t pg_offset;
	uint8_t val1, val2;
	int retval = 0;
	
	for (pg_offset = 0; pg_offset < size; pg_offset += PG_SIZE) {
		for (i = 0; i < PG_SIZE; i++) {
			val1 = ((uint8_t *) buf1)[pg_offset + i];
			val2 = ((uint8_t *) buf2)[pg_offset + i];
			bytefs_expect_msg(val1 == val2, "VERIFY on %llX <-> %llX (Sz: %lld) "
						"BUG : Verify X Offset: %013X: BUF1: %02X, BUF2: %02X\n", 
						(uint64_t) buf1 + pg_offset, (uint64_t) buf2 + pg_offset, 
						size, i, val1, val2);
			if (val1 != val2) {
				retval = 1;
				break;
			}
		}
	}
	bytefs_log("Verify over w/Sz: %lld =============", size);
	return retval;
}

/** 
 * ssd block&byte emulation in-kernel testing function
 * INPUT : 
 * a - potentially can be used, discarded in current version
 * RETURN :
 * always 0
 * COVERAGE :
 * STAGE 1 - block I/O rw and byte I/O rw, each time writng/reading a page
 * STAGE 2 - random 128 times r/w in 8 pages, each time writing a "log" (with log granularity 64)
 * STAGE 3 - write random bytes to the rest of buffer that hasn't been covered by STAGE 2
 * STAGE 4 - read the 8 page buffer to check STAGE 2 and STAGE 3
 * TEST  1,2,4 - r/w verifcation at different location of the page
 * TEST  3 - read at places never covered in previous tests
 * SIDE-EFFECT - turning on all printk in ftl.c will cause termination of mounting. Reasons are to be cleared out.
*/
int ssd_test(void) {
	const unsigned buf_start = 32 * PG_SIZE; // lpa starting address, in "disk"
	const unsigned buf_size = 64 * PG_SIZE;  
	void* buf1 = kzalloc(buf_size, GFP_KERNEL);
	void* buf2 = kzalloc(buf_size, GFP_KERNEL);
	int i;
	uint64_t buf_offset, offseted_start;
	uint8_t rand_byte[8];
	unsigned access_size;
	uint64_t access_start, access_offset;

	bytefs_log("DRAM cache size: %ldB", DRAM_END);

	bytefs_assert(buf1);
	bytefs_assert(buf2);

	for (buf_offset = 0; buf_offset < buf_size; buf_offset += PG_SIZE) {
		offseted_start = buf_start + buf_offset;
		get_random_bytes(buf1 + buf_offset, PG_SIZE);
		byte_issue(1, offseted_start, PG_SIZE, buf1 + buf_offset);
		byte_issue(0, offseted_start, PG_SIZE, buf2 + buf_offset);
		if (verify(buf1 + buf_offset, buf2 + buf_offset, PG_SIZE)) return 1;
	}
	bytefs_log("STAGE 1 COMPLETE");

	byte_issue(0, buf_start, buf_size, buf2);
	if (verify(buf1, buf2, buf_size)) return 1;
	bytefs_log("STAGE 2 COMPLETE");

	byte_issue(0, buf_start, buf_size, buf1);
	for (i = 0; i < TEST_SSD_RANDW_LOC_NUM; i++) {
		get_random_bytes(rand_byte, 1);
		access_size = 2 * (PG_SIZE * rand_byte[0] / 256);

		get_random_bytes(rand_byte, 8);
		access_offset = *((uint64_t *) rand_byte) % (buf_size - access_size);
		access_start = access_offset + buf_start;
		bytefs_log("I: %d S: %lld Off: %lld Sz: %d", i, access_start, access_offset, access_size);
		get_random_bytes(buf2 + access_offset, access_size);
		// memcpy(&buf2[access_offset], &buf1[access_offset], access_size);
		byte_issue(1, access_start, access_size, buf2 + access_offset);
	}
	bytefs_log("STAGE 3 COMPLETE");

	byte_issue(0, buf_start, buf_size, buf1);
	bytefs_log("STAGE 4 COMPLETE");
	
	if (verify(buf1, buf2, buf_size)) return 1;
	bytefs_log("STAGE 5 COMPLETE");

	kfree(buf1);
	kfree(buf2);
	return 0;
}

int bytefs_stop_threads(void) {
    struct ssd *ssd = &gdev;

    // kill the thread first
    bytefs_log("ByteFS stopping threads");
    if (ssd->thread_id && !kthread_stop(ssd->thread_id)) {
        ssd->thread_id = 0;
        bytefs_log("ftl thread stopped");
    } else {
        bytefs_err("ftl thread stop failed");
    }
        
    if (ssd->polling_thread_id && !kthread_stop(ssd->polling_thread_id)) {
        ssd->polling_thread_id = 0;
        bytefs_log("polling thread stopped");
    } else {
        bytefs_err("polling thread stop failed");
    }
    return 0;
}

int bytefs_start_threads(void) {
    struct ssd *ssd = &gdev;

    bytefs_log("ByteFS start threads");
    if (ssd->thread_id == 0) {
        ssd->thread_id = kzalloc(sizeof(struct task_struct), GFP_KERNEL);
        ssd->thread_id = kthread_create(ftl_thread, ssd->thread_args, "ftl_thread");
        kthread_bind(ssd->thread_id, SSD_THREAD_CPU);
        if (ssd->thread_id == 0)
            bytefs_err("Failed to create FTL thread\n");
        else
            wake_up_process(ssd->thread_id);
    } else {
        bytefs_err("ByteFS FTL thread already exists");
    }

    if (ssd->polling_thread_id == 0) {
        ssd->polling_thread_id = kzalloc(sizeof(struct task_struct), GFP_KERNEL);
        ssd->polling_thread_id = kthread_create(request_poller_thread, ssd->thread_args, "ftl_poller_thread");
        kthread_bind(ssd->polling_thread_id, SSD_POLLING_CPU);
        if (ssd->polling_thread_id == 0)
            bytefs_err("Failed to create request poller thread\n");
        else
            wake_up_process(ssd->polling_thread_id);
    } else {
        bytefs_err("ByteFS polling thread already exists");
    }
    return 0;
}
