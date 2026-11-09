// SPDX-License-Identifier: GPL-2.0-only
#include <asm/unaligned.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/string.h>

#include "spring-ffu-policy.h"

#define SPRING_FFU_RECORD_OFFSET 0x100
#define SPRING_FFU_RECORD_STRIDE 0x100
#define SPRING_FFU_RECORD_COUNT_OFFSET 0x20
#define SPRING_FFU_MAX_PAYLOAD 0x1000000

int spring_ffu_check_boot(void)
{
	struct device_node *memory;
	u32 battery_status, boot_reason;
	int ret;

	memory = of_find_node_by_path("/memory");
	if (!memory)
		return -ENODEV;
	ret = of_property_read_u32(memory, "ufs_ffu_batterystatus", &battery_status);
	if (!ret)
		ret = of_property_read_u32(memory, "ufs_ffu_bootreason", &boot_reason);
	of_node_put(memory);
	if (ret)
		return ret;

	if (battery_status == 1 || boot_reason == 0x41)
		return -EPERM;
	return 0;
}

static bool spring_ffu_field_matches(const u8 *field, size_t width,
				     const char *value)
{
	size_t length = strnlen(field, width);

	return length && length < width && strlen(value) == length &&
	       !memcmp(field, value, length);
}

int spring_ffu_find_partition_entry(const u8 *header, size_t header_size,
				    u64 partition_size, const char *vendor,
				    const char *model, const char *revision,
				    struct spring_ffu_partition_entry *entry)
{
	u32 count, index;

	if (header_size < SPRING_FFU_BLOCK_SIZE ||
	    partition_size < SPRING_FFU_BLOCK_SIZE)
		return -EINVAL;
	if (memcmp(header, "ffu", sizeof("ffu")))
		return -ENOENT;

	count = get_unaligned_le32(header + SPRING_FFU_RECORD_COUNT_OFFSET);
	if (count > (SPRING_FFU_BLOCK_SIZE - SPRING_FFU_RECORD_OFFSET) /
		    SPRING_FFU_RECORD_STRIDE)
		return -EINVAL;

	for (index = 0; index < count; index++) {
		const u8 *record = header + SPRING_FFU_RECORD_OFFSET +
				   index * SPRING_FFU_RECORD_STRIDE;
		u64 offset, aligned_size;
		u32 size;

		if (!spring_ffu_field_matches(record, 9, vendor) ||
		    !spring_ffu_field_matches(record + 9, 17, model) ||
		    !spring_ffu_field_matches(record + 26, 5, revision))
			continue;
		if (spring_ffu_field_matches(record + 31, 5, revision))
			continue;

		offset = get_unaligned_le64(record + 36);
		size = get_unaligned_le32(record + 44);
		aligned_size = ALIGN((u64)size, SPRING_FFU_BLOCK_SIZE);
		if (!size || size > SPRING_FFU_MAX_PAYLOAD ||
		    offset < SPRING_FFU_BLOCK_SIZE ||
		    !IS_ALIGNED(offset, SPRING_FFU_BLOCK_SIZE) ||
		    offset >= partition_size || aligned_size > partition_size - offset)
			return -EINVAL;

		entry->offset = offset;
		entry->size = size;
		entry->crc = get_unaligned_le32(record + 48);
		return 0;
	}
	return -ENOENT;
}

int spring_ffu_check_partition_payload(const struct spring_ffu_partition_entry *entry,
				       const void *data, size_t size)
{
	if (size != entry->size)
		return -EINVAL;
	return crc32_le(0, data, size) == entry->crc ? 0 : -EBADMSG;
}
