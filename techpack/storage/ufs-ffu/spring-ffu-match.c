// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kernel.h>
#include <linux/string.h>

#include "spring-ffu-match.h"
#include "spring-ffu-table.h"

const struct spring_ffu_update *spring_ffu_find_update(const char *vendor,
						       const char *model,
						       const char *revision)
{
	size_t index;

	for (index = 0; index < ARRAY_SIZE(spring_ffu_updates); index++) {
		const struct spring_ffu_update *entry = &spring_ffu_updates[index];

		if (!strcmp(vendor, entry->vendor) &&
		    !strcmp(model, entry->model) &&
		    !strcmp(revision, entry->from_revision))
			return entry;
	}
	return NULL;
}
