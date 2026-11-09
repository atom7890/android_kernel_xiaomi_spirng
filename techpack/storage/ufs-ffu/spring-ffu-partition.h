/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SPRING_FFU_PARTITION_H
#define SPRING_FFU_PARTITION_H

#include <linux/types.h>

struct block_device;
struct scsi_device;

struct spring_ffu_partition {
	struct scsi_device *sdev;
	u64 first_block;
	u64 block_count;
};

int spring_ffu_rescue_extent(struct spring_ffu_partition *partition,
			     struct scsi_device *sdev,
			     struct block_device *bdev);
int spring_ffu_read_partition(const struct spring_ffu_partition *partition,
			      u64 offset, void *data, size_t size);
int spring_ffu_read_header(const struct spring_ffu_partition *partition,
			   u8 *header);
int spring_ffu_invalidate_partition(const struct spring_ffu_partition *partition,
				    const u8 *expected_header);

#endif
