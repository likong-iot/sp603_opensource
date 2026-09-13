/*
 * @Author: AI Assistant
 * @Date: 2024-07-18
 * @Description: 使用ESP32定时器替代FreeRTOS任务，减少内存占用
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#include "esp_timer.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 定时器任务初始化和停止函数
esp_err_t init_timer_tasks(void);
esp_err_t stop_timer_tasks(void);


esp_err_t init_key_timer(void);

// AP超时配置重新加载函数
void reload_ap_timeout_config(void);


// LED闪烁定时器回调函数声明
void time_sync_led_callback(void *arg);

// 导出LED闪烁定时器句柄，用于在其他文件中访问
extern esp_timer_handle_t time_sync_led_timer;
// 导出其他定时器句柄
extern esp_timer_handle_t heartbeat_timer;
extern esp_timer_handle_t time_sync_timer;
extern esp_timer_handle_t key_timer;

// 按键状态标志声明
extern volatile bool key_short_press_detected;
extern volatile bool key_long_press_detected;


#ifdef __cplusplus
}
#endif
