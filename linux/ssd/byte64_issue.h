#ifndef _BYTE_ISSUE_H_
#define _BYTE_ISSUE_H_

#include "ftl.h"

#define LOG_SIZE    (BYTEFS_LOG_REGION_GRANDULARITY)

int32_t byte_issue_64_aligned(int is_write, uint64_t lpa, uint64_t size, void* buf);

#endif


