/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef SPRING_NOVATEK_TOUCH_H
#define SPRING_NOVATEK_TOUCH_H

#include <linux/types.h>

struct device;

int nvt_spring_touch_register(struct device *dev);
void nvt_spring_touch_unregister(void);
int nvt_spring_set_palm(int enabled);
int nvt_spring_send_mode(u8 command, u8 value);

#endif
