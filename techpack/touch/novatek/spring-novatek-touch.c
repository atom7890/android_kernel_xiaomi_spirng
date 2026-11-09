// SPDX-License-Identifier: GPL-2.0-only
#include "../xiaomi-tp/xiaomi_touch.h"
#include "spring-novatek-touch.h"

#define SPRING_NVT_PANEL_ID 0

static hardware_param_t nvt_panel_params;
static bool nvt_panel_registered;

static void nvt_spring_set_mode(int mode, int *value)
{
	static const u8 orientation[] = { 0xba, 0xbc, 0xba, 0xbb };
	u8 command, argument;
	int applied;

	if (!value)
		return;

	switch (mode) {
	case Touch_UP_THRESHOLD:
	case Touch_Tolerance:
		applied = clamp(*value, 0, 4);
		command = mode == Touch_UP_THRESHOLD ? 0x71 : 0x70;
		argument = applied;
		break;
	case Touch_Edge_Filter:
		applied = clamp(*value, 0, 3);
		command = 0x72;
		argument = applied;
		break;
	case Touch_Panel_Orientation:
		applied = clamp(*value, 0, 3);
		command = orientation[applied];
		argument = 0;
		break;
	default:
		return;
	}

	if (!nvt_spring_send_mode(command, argument))
		update_driver_cur_value(SPRING_NVT_PANEL_ID, mode, applied);
}

static void nvt_spring_dfs_test(int code)
{
	const char *fault;

	switch (code) {
	case 0x365c07e9:
		fault = "TpTransferErr";
		break;
	case 0x365c07ea:
		fault = "TpFirmwareLoadFail";
		break;
	case 0x365c07eb:
		xiaomi_touch_mievent_report_int(code, SPRING_NVT_PANEL_ID,
					       "TpParamParseFail", "novatek", 0);
		xiaomi_touch_mievent_report_int(code, SPRING_NVT_PANEL_ID,
					       "TpParamParseFail", "novatek", 1);
		return;
	case 0x365c0bd1:
		fault = "TpOpenTestFail";
		break;
	case 0x365c0bd2:
		fault = "TpShortTestFail";
		break;
	default:
		return;
	}

	xiaomi_touch_mievent_report_int(code, SPRING_NVT_PANEL_ID, fault,
				       "novatek", -1);
}

static hardware_operation_t nvt_panel_ops = {
	.set_mode_value = nvt_spring_set_mode,
	.palm_sensor_write = nvt_spring_set_palm,
	.touch_dfs_test = nvt_spring_dfs_test,
};

static_assert(offsetof(hardware_operation_t, set_mode_value) == 0x20);
static_assert(offsetof(hardware_operation_t, palm_sensor_write) == 0x60);
static_assert(offsetof(hardware_operation_t, touch_dfs_test) == 0xe8);
static_assert(sizeof(hardware_operation_t) == 0xf0);

int nvt_spring_touch_register(struct device *dev)
{
	int ret;

	ret = register_touch_panel(dev, SPRING_NVT_PANEL_ID,
				   &nvt_panel_params, &nvt_panel_ops);
	if (!ret)
		nvt_panel_registered = true;
	return ret;
}

void nvt_spring_touch_unregister(void)
{
	if (!nvt_panel_registered)
		return;

	unregister_touch_panel(SPRING_NVT_PANEL_ID);
	nvt_panel_registered = false;
}
