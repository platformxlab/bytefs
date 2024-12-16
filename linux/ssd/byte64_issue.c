#include "byte64_issue.h"


// if the edge block is contained within ONE LOG
static int32_t __one_edge_block(uint64_t lpa, uint64_t size) {
	uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / LOG_SIZE * LOG_SIZE, S1 = C0 / LOG_SIZE * LOG_SIZE + LOG_SIZE;
	uint64_t S2 = C1 / LOG_SIZE * LOG_SIZE, S3 = C1 / LOG_SIZE * LOG_SIZE + LOG_SIZE;
	if(S0 == S2 && S1 == S3) {
		return 1;
	}
	return 0;
}

static int32_t __handle_one_edge_block(int is_write, uint64_t lpa, uint64_t size, void* buf) {
	uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / LOG_SIZE * LOG_SIZE;
	char* temp_buf;
	temp_buf = kmalloc(LOG_SIZE, GFP_KERNEL); /* front and end temp buffer */
	if(temp_buf == NULL) {
		printk("%s buffer allocation failed",__func__);
		return -1;
	}
	if(is_write) {
		if(0 > byte_issue(0, S0, LOG_SIZE, temp_buf)) {
			printk(KERN_ERR "%s : issue faield", __func__);
			return -1;
		}
		memcpy(temp_buf + C0 - S0, buf, C1 - C0);
		if(0 > byte_issue(1, S0, LOG_SIZE, temp_buf)) {
			printk(KERN_ERR "%s : issue faield", __func__);
			return -1;
		}
	}else{
		if(0 > byte_issue(0, S0, LOG_SIZE, temp_buf)) {
			printk(KERN_ERR "%s : issue faield", __func__);
			return -1;
		}
		memcpy(buf, temp_buf + C0 - S0, C1 - C0);
	}
	return 0;
}	

/* handle the front and back 64B chunks (if not aligned) */
static int32_t _handle_edge_logs(int is_write, uint64_t lpa, uint64_t size, void* buf) {
	uint64_t C0 = lpa, C1 = lpa + size;
	uint64_t S0 = C0 / LOG_SIZE * LOG_SIZE, S1 = C0 / LOG_SIZE * LOG_SIZE + LOG_SIZE;
	uint64_t S2 = C1 / LOG_SIZE * LOG_SIZE, S3 = C1 / LOG_SIZE * LOG_SIZE + LOG_SIZE;
	char* temp_buf;
	if(__one_edge_block(lpa, size)) {
		return __handle_one_edge_block(is_write, lpa, size, buf);
	}

	temp_buf = kmalloc(LOG_SIZE, GFP_KERNEL); /* front and end temp buffer */
	if(temp_buf == NULL) {
		printk(KERN_ERR "%s buffer allocation failed",__func__);
		return -1;
	}
	// handle two write logs
	if(is_write && (C0 % LOG_SIZE)) { /* handle left */
		if(0 > byte_issue(0, S0, LOG_SIZE, temp_buf)) {
			printk(KERN_ERR "%s : issue failed\n",__func__);
			return -1;
		}
		/* overwrite read from C0 to S1 */
		memcpy(temp_buf + C0 - S0, buf, S1 - C0);
		/* write the 64B to disk */
		if(0 > byte_issue(1, S0, LOG_SIZE, temp_buf)) {
			printk(KERN_ERR "%s : issue failed\n",__func__);
			return -1;
		}
	}
	if(is_write && (C1 % LOG_SIZE)) { /* handle right */
		if(0 > byte_issue(0, S2, LOG_SIZE, temp_buf)) {
			printk("%s : issue failed\n",__func__);
			return -1;
		}
		/* overwrite read from S2 to C1 */
		memcpy(temp_buf, buf + S2 - C0, C1 - S2);
		/* write the 64B to disk */
		if(0 > byte_issue(1, S2, LOG_SIZE, temp_buf)) {
			printk("%s : issue failed\n",__func__);
			return -1;
		}
	}

	// handle two read logs
	if((!is_write) && (C0 % LOG_SIZE)) { /* handle left */
		if(0 > byte_issue(0, S0, LOG_SIZE, temp_buf)) {
			printk("%s : issue failed\n",__func__);
			return -1;
		}
		memcpy(buf, temp_buf + C0 - S0, S1 - C0);
	}
	if((!is_write) && (C1 % LOG_SIZE)) {
	#if (PMFS_DEBUG_TYPE & PMFS_DATA_ISSUE_DEBUG)
		printk(KERN_ERR "type 2 cornor write");
	#endif
		if(0 > byte_issue(0, S2, LOG_SIZE, temp_buf)) {
			printk("%s : issue failed\n",__func__);
			return -1;
		}
		memcpy(buf + S2 - C0, temp_buf, C1 - S2);
	}
	kfree(temp_buf);
	return 0;
}

/**
* Same as _byte_issue in pmfs_bytefs.c, difference is that we align the r/w requests to 
* 64 B. Meaning if you initiate a 13 bytes r/w, it will either :
* read 64 B, modify, write
* or split it into two 64 B, modify two, write 2 64 B back.
* Meaning alignment is absolute to virtual address.
*/
int32_t byte_issue_64_aligned(int is_write, uint64_t lpa, uint64_t size, void* buf) {
	uint64_t C0,C1;
	uint64_t S0, S1, S2;

	if(0 > _handle_edge_logs(is_write, lpa, size, buf)) {
        printk(KERN_ERR "bad edge block handling in %s", __func__);
		return -1;
    }

	// the position here is essential because of (*)
	C0 = lpa, C1 = lpa + size;
	S0 = C0 / LOG_SIZE * LOG_SIZE;
	S1 = C0 / LOG_SIZE * LOG_SIZE + LOG_SIZE;
	S2 = C1 / LOG_SIZE * LOG_SIZE;
	

	if(S1 % LOG_SIZE || (S2 - S1) % LOG_SIZE) {
		printk(KERN_ERR "not 64 B aligned : issued failed at %s", __func__);
		return -1;
	}

	// for edge blocks, we use byte issue, for central blocks, we use block issue
	
	if(S2 >= S1) {
		if(C0 == S0) // if left edge block is aligned 
			return byte_issue(is_write, S0, S2 - S0, buf);
		else if(S2 > S1) 
			return byte_issue(is_write, S1, S2 - S1, buf + S1 - C0);
	}
	return 0;
}
EXPORT_SYMBOL(byte_issue_64_aligned);

