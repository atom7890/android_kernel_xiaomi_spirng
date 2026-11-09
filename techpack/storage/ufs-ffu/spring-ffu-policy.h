/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SPRING_FFU_POLICY_H
#define SPRING_FFU_POLICY_H

#include <linux/types.h>

#define SPRING_FFU_BLOCK_SIZE 4096

struct spring_ffu_partition_entry {
	u64 offset;
	u32 size;
	u32 crc;
};

int spring_ffu_check_boot(void);
int spring_ffu_find_partition_entry(const u8 *header, size_t header_size,
				    u64 partition_size, const char *vendor,
				    const char *model, const char *revision,
				    struct spring_ffu_partition_entry *entry);
int spring_ffu_check_partition_payload(const struct spring_ffu_partition_entry *entry,
				       const void *data, size_t size);

#endif
