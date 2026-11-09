/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SPRING_FFU_MATCH_H
#define SPRING_FFU_MATCH_H

#include <crypto/sha2.h>
#include <linux/types.h>

struct firmware;
struct scsi_device;

struct spring_ffu_update {
	const char *vendor;
	const char *model;
	const char *from_revision;
	const char *to_revision;
	const char *firmware;
	size_t size;
	u8 digest[SHA256_DIGEST_SIZE];
};

const struct spring_ffu_update *spring_ffu_find_update(const char *vendor,
						       const char *model,
						       const char *revision);

int spring_ffu_write_payload(struct scsi_device *sdev,
			     const struct spring_ffu_update *update,
			     const struct firmware *firmware, bool *attempted);

#endif
