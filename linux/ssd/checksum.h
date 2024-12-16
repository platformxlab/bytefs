#ifndef __SSD_CHECKSUM_H__
#define __SSD_CHECKSUM_H__

#include <linux/crc32c.h>
#include <linux/types.h>

static inline u32 ssd_crc32c(u32 crc, const u8 *data, size_t len)
{
	u8 *ptr = (u8*) data;

	u32 csum;
    csum = crc32c(crc, data, len);

	return csum;
};



#endif