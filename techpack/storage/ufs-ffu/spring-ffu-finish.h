/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SPRING_FFU_FINISH_H
#define SPRING_FFU_FINISH_H

#include <linux/types.h>

struct spring_ffu_partition;

int spring_ffu_finish_update(int write_status,
			     const struct spring_ffu_partition *source,
			     const u8 *expected_header);

#endif
