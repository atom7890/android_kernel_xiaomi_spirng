// SPDX-License-Identifier: GPL-2.0-only
#include <linux/build_bug.h>
#include <linux/stddef.h>
#include "xiaomi_touch.h"

static_assert(sizeof(common_data_t) == 1032);
static_assert(offsetof(common_data_t, data_buf) == 8);
static_assert(sizeof(hardware_param_t) == 214);
static_assert(sizeof(hardware_operation_t) == 240);
static_assert(sizeof(xiaomi_touch_driver_param_t) == 584);
static_assert(offsetof(xiaomi_touch_driver_param_t, hardware_param) == 130);
static_assert(offsetof(xiaomi_touch_driver_param_t, hardware_operation) == 344);

#define CHECK_CALLBACK(member, offset) \
    static_assert(offsetof(hardware_operation_t, member) == (offset))

CHECK_CALLBACK(ic_self_test, 0);
CHECK_CALLBACK(ic_data_collect, 8);
CHECK_CALLBACK(ic_get_lockdown_info, 16);
CHECK_CALLBACK(ic_get_fw_version, 24);
CHECK_CALLBACK(set_mode_value, 32);
CHECK_CALLBACK(get_mode_value, 40);
CHECK_CALLBACK(get_mode_all, 48);
CHECK_CALLBACK(reset_mode, 56);
CHECK_CALLBACK(ic_switch_mode, 64);
CHECK_CALLBACK(ic_enable_irq, 72);
CHECK_CALLBACK(cmd_update_func, 80);
CHECK_CALLBACK(set_mode_long_value, 88);
CHECK_CALLBACK(palm_sensor_write, 96);
CHECK_CALLBACK(enable_touch_raw, 104);
CHECK_CALLBACK(panel_vendor_read, 112);
CHECK_CALLBACK(panel_color_read, 120);
CHECK_CALLBACK(panel_display_read, 128);
CHECK_CALLBACK(touch_vendor_read, 136);
CHECK_CALLBACK(get_touch_ic_buffer, 144);
CHECK_CALLBACK(touch_doze_analysis, 152);
CHECK_CALLBACK(touch_log_level_control, 160);
CHECK_CALLBACK(touch_log_level_control_v2, 168);
CHECK_CALLBACK(set_nfc_to_touch_event, 176);
CHECK_CALLBACK(htc_ic_setModeValue, 184);
CHECK_CALLBACK(htc_ic_getModeValue, 192);
CHECK_CALLBACK(ic_resume_suspend, 200);
CHECK_CALLBACK(ic_set_charge_state, 208);
CHECK_CALLBACK(ic_set_fod_value, 216);
CHECK_CALLBACK(set_thermal_temp, 224);
CHECK_CALLBACK(touch_dfs_test, 232);
