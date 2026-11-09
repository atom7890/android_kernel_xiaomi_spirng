// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/notifier.h>
#include <linux/reboot.h>

#include "spring-ffu-finish.h"
#include "spring-ffu-partition.h"

extern int ufs_ffu_reboot_reason_reboot(void *data);

/* Call only after a WRITE BUFFER command was attempted. */
int spring_ffu_finish_update(int write_status,
			     const struct spring_ffu_partition *source,
			     const u8 *expected_header)
{
	char success[] = "ffus";
	char failure[] = "ffuf";
	char *reason = write_status ? failure : success;
	int ret;

	if (write_status && source) {
		ret = spring_ffu_invalidate_partition(source, expected_header);
		if (ret)
			return ret;
	}

	msleep(20);
	if (ufs_ffu_reboot_reason_reboot(reason) != NOTIFY_OK)
		return -EIO;

	kernel_restart(reason);
	return -EIO;
}
