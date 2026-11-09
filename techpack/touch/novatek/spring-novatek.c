// SPDX-License-Identifier: GPL-2.0-only
#include <linux/errno.h>
#include <linux/of.h>
#include <linux/string.h>

#include "spring-novatek.h"

extern char erppanel_info[];

int spring_novatek_select_firmware(struct spring_novatek_firmware *firmware)
{
	struct device_node *chosen;
	const char *bootargs;
	int ret;

	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		return -ENODEV;

	ret = of_property_read_string(chosen, "bootargs", &bootargs);
	if (!ret && !strstr(bootargs, "dsi_panel_o19_42_02_0a_fhd_video"))
		ret = -ENODEV;
	of_node_put(chosen);
	if (ret)
		return ret;

	if (!strcmp(erppanel_info, "11")) {
		firmware->vendor = "csot_erp";
		firmware->normal = "novatek_ts_fw_csot_erp.bin";
		firmware->selftest = "novatek_ts_mp_csot_erp.bin";
	} else {
		firmware->vendor = "csot";
		firmware->normal = "novatek_ts_fw_csot.bin";
		firmware->selftest = "novatek_ts_mp_csot.bin";
	}
	return 0;
}
