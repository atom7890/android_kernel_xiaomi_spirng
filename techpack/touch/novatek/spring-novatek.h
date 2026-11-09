/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SPRING_NOVATEK_H
#define SPRING_NOVATEK_H

struct spring_novatek_firmware {
	const char *vendor;
	const char *normal;
	const char *selftest;
};

int spring_novatek_select_firmware(struct spring_novatek_firmware *firmware);

#endif
