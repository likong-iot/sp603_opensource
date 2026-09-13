/*
 * @Author: Orion
 * @Date: 2025-10-23 00:00:00
 * @LastEditors: Orion
 * @LastEditTime: 2025-10-23 00:00:00
 * @FilePath: \2CH_485HUB-V1.0\main\include\sx_time_manager.h
 * @Description: 统一时间管理模块 - 通过Web同步时间，提供全局时间服务
 *
 * Copyright (c) 2025 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

/**
 * @brief 时间管理器初始化
 */
void time_manager_init(void);

/**
 * @brief 从浏览器同步时间
 * @param browser_timestamp_ms 浏览器时间戳（毫秒，Unix时间）
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t time_manager_sync_from_browser(uint64_t browser_timestamp_ms);

/**
 * @brief 获取当前时间戳（毫秒）
 * @return 当前时间戳（毫秒，Unix时间）
 */
uint64_t time_manager_get_current_ms(void);

/**
 * @brief 获取当前时间戳（微秒）
 * @return 当前时间戳（微秒，Unix时间）
 */
uint64_t time_manager_get_current_us(void);

/**
 * @brief 检查时间是否已同步
 * @return true 已同步，false 未同步
 */
bool time_manager_is_synced(void);

/**
 * @brief 获取格式化的时间字符串
 * @param buffer 输出缓冲区
 * @param size 缓冲区大小
 * @param format 时间格式（strftime格式，如 "%Y-%m-%d %H:%M:%S"）
 * @return 实际写入的字符数，失败返回0
 * 
 * @example
 * char time_str[64];
 * time_manager_format_time(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S");
 * // 输出: "2025-10-23 14:35:26"
 */
int time_manager_format_time(char *buffer, size_t size, const char *format);

/**
 * @brief 将时间戳转换为结构化时间
 * @param timestamp_ms 时间戳（毫秒）
 * @param tm_info 输出的时间结构
 */
void time_manager_timestamp_to_tm(uint64_t timestamp_ms, struct tm *tm_info);

/**
 * @brief 获取上次同步时间
 * @return 上次同步的时间戳（毫秒），0表示从未同步
 */
uint64_t time_manager_get_last_sync_time(void);

#ifdef __cplusplus
}
#endif

