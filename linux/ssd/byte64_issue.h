#ifndef _BYTE_ISSUE_H_
#define _BYTE_ISSUE_H_

#include "ftl.h"


int32_t byte_issue_64_aligned(int is_write, uint64_t lpa, uint64_t size, void* buf);

#endif


