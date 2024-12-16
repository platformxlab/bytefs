#include "bytefs_issue.h"
#include "../../../ssd/byte64_issue.h"
#include "../../../ssd/cow.h"
#include <linux/random.h>
#include <linux/kernel.h>
#include <linux/module.h>

void *bytefs_dummy_buffer = 0;

static int bytefs_ssd_init_num = 0;
static void bytefs_verification(void);
static void* base_off = 0;

static int _bytefs_byte_issue(int is_write, uint64_t lpa, uint64_t size, void* buf) {
#if (0 == BYTE_ISSUE_64_ALIGN)
	if(0 > byte_issue(is_write, lpa, size, buf)) {
		printk(KERN_ERR "BUG : issue failed");
		return -1;
	}
#else

	return byte_issue_64_aligned(is_write, lpa, size, buf);
	

#endif


	return 0;
}


/* This function can be called multiple times. It will only call ssd_init once. */
static int32_t bytefs_ssd_init(void) {
	if(bytefs_ssd_init_num) return 0;
	bytefs_ssd_init_num = 1;
	bytefs_dummy_buffer = vmalloc(BYTEFS_DUMMY_BUFFER_LEN);
	return ssd_init();
}


/** 
 * Naive layer of separation for byte block addressability : 
 * for PG_SIZE blocks (between edge block), block, otherwise byte
 * The function will be defined when BYTE_BLOCK_MIX_ENABLE is opened
*/
#ifdef BYTE_BLOCK_MIX_ENABLE

// if the edge block is contained within ONE PAGE
static int32_t __one_edge_block(uint64_t lpa, uint64_t size) {
	uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / PG_SIZE * PG_SIZE, S1 = C0 / PG_SIZE * PG_SIZE + PG_SIZE;
	uint64_t S2 = C1 / PG_SIZE * PG_SIZE, S3 = C1 / PG_SIZE * PG_SIZE + PG_SIZE;
	if(S0 == S2 && S1 == S3) {
		return 1;
	}
	return 0;
}

static int32_t __handle_one_edge_block(int is_write, uint64_t lpa, uint64_t size, void* buf) {
	uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / PG_SIZE * PG_SIZE;
	if(0 < _bytefs_byte_issue(is_write, C0, C1 - C0, buf)) {
		printk(KERN_ERR "%s : issue faield", __func__);
		return -1;
	}
	return 0;
}	

/* handle the front and back 64B chunks (if not aligned) */
static int32_t _bytefs_handle_edge_logs(int is_write, uint64_t lpa, uint64_t size, void* buf) {
	uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / PG_SIZE * PG_SIZE, S1 = C0 / PG_SIZE * PG_SIZE + PG_SIZE;
	uint64_t S2 = C1 / PG_SIZE * PG_SIZE, S3 = C1 / PG_SIZE * PG_SIZE + PG_SIZE;

	if(__one_edge_block(lpa, size)) {
		return __handle_one_edge_block(is_write, lpa, size, buf);
	}

	bytefs_bytefs_debug(KERN_ERR "%llx %llx S0~S3 : %llx %llx %llx %llx",C0,C1,S0,S1,S2,S3);

	// handle two write logs
	if((C0 % PG_SIZE)) { /* handle left */
		if(0 < _bytefs_byte_issue(is_write, C0, S1 - C0, buf)) {
			printk(KERN_ERR "%s : issue failed\n",__func__);
			return -1;
		}
	}
	if((C1 % PG_SIZE)) { /* handle right */
		if(0 < _bytefs_byte_issue(is_write, S2, C1 - S2, buf + S2 - C0)) {
			printk("%s : issue failed\n",__func__);
			return -1;
		}
	}
	return 0;
}

#endif

int32_t bytefs_data_rw(struct file* filp, int is_write, uint64_t pfn, uint64_t offset, uint64_t size, void* buf){
	uint64_t lpa;
	lpa = pfn * PG_SIZE + offset;
	if(-1 == (lpa = bytefs_debug_lpa_in_range(lpa))){
		dump_stack();
		return -1; // (*) : IMPORTANT, here we update lpa
	}
	if(is_write) {
		// SSD_STAT_ATOMIC_ADD(block_rissue_traffic, size);
		return bytefs_pgcache_wr(filp, lpa, lpa + size, buf);
	} else {
		return bytefs_pgcache_rd(filp, lpa, lpa + size, buf);
	}
	
}


/**
 * bytefs byte issue for NVMM read/write.
 * In BYTEFS, the NVMM is accessed by its virtual address. Here we directly use pfn and offset to access our 2B-SSD interface.
 * @pfn is page frame number.
*/
int32_t bytefs_byte_issue(int is_write, uint64_t pfn, uint64_t offset, uint64_t size, void* buf, bytefs_byte_issue_t issue_type) {
	uint64_t lpa;
	// return 0;
#ifdef BYTE_BLOCK_MIX_ENABLE
	// locations for PG_SIZE alignment used for edge blocks
	uint64_t C0, C1;
	uint64_t S0, S1, S2;
#endif
#ifdef BYTEFS_BYTE_MEASURE_ENABLE
	static int issue_time = 0;
	static unsigned long r_issue = 0;
	static unsigned long w_issue = 0;
#endif
	bytefs_ssd_init();

#if (BYTEFS_DEBUG_TYPE != BYTEFS_NO_DEBUG)
	bytefs_verification();
#endif

	// calculate lpa, notice lpa is calculated both by 
	// the following formula and the bytefs_debug_lpa_in_range() function
	lpa = pfn * PG_SIZE + offset;
	if(-1 == (lpa = bytefs_debug_lpa_in_range(lpa))){
		dump_stack();
		return -1; // (*) : IMPORTANT, here we update lpa
	}
	

#ifdef BYTEFS_BYTE_MEASURE_ENABLE
	if(is_write) {
		w_issue += size;
	}else {
		r_issue += size;
	}
	if(++issue_time % 10000 == 0) {
		printk(KERN_ERR "issue times = %d r_size = %llu w_size = %llu", issue_time, r_issue, w_issue);
	}
#endif

	// all block interface : currently commented out 
#if (0 == BYTE_BLOCK_MIX)
	return _bytefs_byte_issue(is_write, lpa, size, buf);
#else

	if(0 > _bytefs_handle_edge_logs(is_write, lpa, size, buf)) {
		printk(KERN_ERR "bad edge block handling in %s", __func__);
		return -1;
	}

	bytefs_bytefs_debug(KERN_ERR "%s %d start = %llx size  = %llu", __func__, is_write, lpa, size);


	// the position here is essential because of (*)
	C0 = lpa, C1 = lpa + size;
	S0 = C0 / PG_SIZE * PG_SIZE;
	S1 = C0 / PG_SIZE * PG_SIZE + PG_SIZE;
	S2 = C1 / PG_SIZE * PG_SIZE;
	

	if(S1 % PG_SIZE || (S2 - S1) % PG_SIZE) {
		printk(KERN_ERR "not PG_SIZE aligned : issued failed at %s", __func__);
		return -1;
	}

	// for edge blocks, we use byte issue, for central blocks, we use block issue
	
	if(S2 >= S1) {
		if(C0 == S0) // if left edge block is aligned 
			return nvme_issue(is_write, S0 / PG_SIZE, (S2 - S0) / PG_SIZE, buf);
		else if(S2 > S1) 
			return nvme_issue(is_write, S1 / PG_SIZE, (S2 - S1) / PG_SIZE, buf + S1 - C0);
	}
	
	return 0;
#endif
}



#if 0
/**
 * verify if read and write buffer aligns with each other.
*/
static int _bytefs_byte_verify(char* s, char* d, uint64_t size, int32_t test_ind) {
	int i;
	for(i = 0; i < size; i++) {
		if(s[i] != d[i]){
			printk(KERN_ERR "test failed at test : %d\n", test_ind);
			return -1;
		}
	}
	return 0;
}


/**
 * BYTEFS byte interface debugging
 * TEST 1 ~ 2, test in-chunk and out-chunk small read and write without left edge log. 
 * 		in-chunk means the log to r/w (64B memory chunk) is within 64 B aligned memory region. 
 * 		edge log is the log that's causing the r/w region not 64B aligned. So they can be at left and right.
 * 
 * 
*/
static void bytefs_verification(void) {
	static int if_tested = 1;
	const int test_buf_size = 4096;
	int test_num;
	char* test_buf;
	char* probe_buf;
	// int i,j;

	if(if_tested++) return ;

	test_num = 0;
	test_buf = kmalloc(test_buf_size, GFP_KERNEL);
	probe_buf = kmalloc(test_buf_size, GFP_KERNEL);

	if(test_buf == NULL || probe_buf == NULL) {
		printk(KERN_ERR "%s allocation for buffer failed", __func__);
		return ;
	}

	get_random_bytes(test_buf, test_buf_size);

	memcpy(probe_buf, test_buf, test_buf_size);

	const int test_size_1 = 666;
	bytefs_byte_issue(1, PA_START / PG_SIZE, 0, test_size_1, test_buf);
	bytefs_byte_issue(1, PA_START / PG_SIZE, 0, test_size_1, probe_buf);
	_bytefs_byte_verify(test_buf, probe_buf, test_size_1, ++test_num);


	char *cur_w, *cur_r; // current write and read buffer
	cur_w = test_buf + test_size_1;
	cur_r = probe_buf + test_size_1;
	const int test_size_2 = 4088;
	bytefs_byte_issue(1, PA_START / PG_SIZE, 0, test_size_2, cur_w);
	bytefs_byte_issue(1, PA_START / PG_SIZE, 0, test_size_2, cur_r);
	_bytefs_byte_verify(cur_w, cur_r, test_size_2, ++test_num);

	uint8_t rand_offset;
	get_random_bytes(&rand_offset, 1); 
	printk(KERN_NOTICE "test %d, with starting offset : %u\n", test_num + 1, rand_offset);
	bytefs_byte_issue(1, PA_START / PG_SIZE, rand_offset, test_size_2, cur_w);
	bytefs_byte_issue(1, PA_START / PG_SIZE, rand_offset, test_size_2, cur_r);
	_bytefs_byte_verify(cur_w, cur_r, test_size_2, ++test_num);
}
#endif

