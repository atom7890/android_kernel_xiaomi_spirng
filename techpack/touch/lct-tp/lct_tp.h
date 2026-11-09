/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SPRING_LCT_TP_H
#define SPRING_LCT_TP_H
#include <linux/types.h>

typedef int (*tp_selftest_callback_t)(unsigned char cmd);
typedef int (*tp_work_cb_t)(bool enable_tp);
typedef int (*tp_proximity_switch_cb_t)(bool enable_tp);
typedef int (*tp_gesture_cb_t)(bool enable_tp);
typedef int (*set_screen_angle_callback)(int angle);
typedef int (*get_screen_angle_callback)(void);

int init_lct_tp_selftest(tp_selftest_callback_t callback);
void uninit_lct_tp_selftest(void);
void set_lct_tp_info_callback(int (*pfun)(const char *));
void set_lct_tp_lockdown_info_callback(int (*pfun)(void));
void update_lct_tp_info(char *tp_info_buf);
void update_lct_tp_lockdown_info(char *tp_lockdown_info_buf);
int init_lct_tp_info(char *tp_info_buf);
int init_lct_tp_lockdown_info(void);
void uninit_lct_tp_info(void);
void uninit_lct_tp_lockdown_info(void);
int init_lct_tp_work(tp_work_cb_t callback);
void uninit_lct_tp_work(void);
void set_lct_tp_work_status(bool en);
bool get_lct_tp_work_status(void);
int init_lct_tp_proximity_data(void);
void uninit_lct_tp_proximity_data(void);
void set_lct_tp_proximity_data(int value);
bool get_lct_tp_proximity_data_data(void);
int init_lct_tp_proximity_switch(tp_proximity_switch_cb_t callback);
void uninit_lct_tp_proximity_switch(void);
void set_lct_tp_proximity_switch_status(bool en);
bool get_lct_tp_proximity_switch_status(void);
int init_lct_tp_grip_area(set_screen_angle_callback set_fun, get_screen_angle_callback get_fun);
void uninit_lct_tp_grip_area(void);
int set_tp_grip_area_angle(int screen_angle);
int get_tp_grip_area_angle(void);
int init_lct_tp_gesture(tp_gesture_cb_t callback);
void uninit_lct_tp_gesture(void);
void set_lct_tp_gesture_status(bool en);
bool get_lct_tp_gesture_status(void);
#endif
