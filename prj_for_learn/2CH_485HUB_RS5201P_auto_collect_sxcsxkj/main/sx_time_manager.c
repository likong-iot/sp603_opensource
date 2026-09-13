/*
 * @Author: Orion
 * @Date: 2025-10-23 00:00:00
 * @LastEditors: Orion
 * @LastEditTime: 2025-10-23 00:00:00
 * @FilePath: \2CH_485HUB-V1.0\main\sx_time_manager.c
 * @Description: 统一时间管理模块实现
 *
 * Copyright (c) 2025 by SX-IOT, All Rights Reserved.
 */

#include "sx_time_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/task.h"
#include <string.h>
#include <sys/time.h>

static const char *TAG = "TIME_MANAGER";

// 时间偏移量（毫秒）：浏览器时间 - ESP32启动时间
static int64_t g_time_offset_ms = 0;

// 时间是否已同步标志
static bool g_time_synced = false;

// 上次同步时间（ESP32相对时间，毫秒）
static uint64_t g_last_sync_time_ms = 0;

// 互斥锁
static portMUX_TYPE g_time_spinlock = portMUX_INITIALIZER_UNLOCKED;

/*==============================================================================
 * 公共接口实现
 *============================================================================*/

void time_manager_init(void) {
  ESP_LOGI(TAG, "====================================");
  ESP_LOGI(TAG, "时间管理器初始化");
  ESP_LOGI(TAG, "====================================");

  ESP_LOGW(TAG, "⚠️  时间未同步，等待Web端校准");
  ESP_LOGI(TAG, "提示: 打开Web界面后会自动同步时间");

  ESP_LOGI(TAG, "====================================");
}

esp_err_t time_manager_sync_from_browser(uint64_t browser_timestamp_ms) {
  if (browser_timestamp_ms == 0) {
    ESP_LOGE(TAG, "无效的浏览器时间戳: 0");
    return ESP_ERR_INVALID_ARG;
  }

  // 基本合理性检查：时间戳应该在 2020-01-01 到 2100-01-01 之间
  const uint64_t min_timestamp = 1577836800000ULL; // 2020-01-01
  const uint64_t max_timestamp = 4102444800000ULL; // 2100-01-01
  if (browser_timestamp_ms < min_timestamp ||
      browser_timestamp_ms > max_timestamp) {
    ESP_LOGE(TAG, "浏览器时间戳超出合理范围: %llu",
             (unsigned long long)browser_timestamp_ms);
    return ESP_ERR_INVALID_ARG;
  }

  // 获取ESP32启动以来的时间（微秒）
  uint64_t esp_time_us = esp_timer_get_time();
  uint64_t esp_time_ms = esp_time_us / 1000;

  // 计算时间偏移量
  int64_t new_offset = (int64_t)browser_timestamp_ms - (int64_t)esp_time_ms;

  portENTER_CRITICAL(&g_time_spinlock);
  int64_t old_offset = g_time_offset_ms;
  g_time_offset_ms = new_offset;
  g_time_synced = true;
  g_last_sync_time_ms = esp_time_ms;
  portEXIT_CRITICAL(&g_time_spinlock);

  // 同时更新系统时间（用于其他可能依赖系统时间的组件）
  struct timeval tv;
  tv.tv_sec = browser_timestamp_ms / 1000;
  tv.tv_usec = (browser_timestamp_ms % 1000) * 1000;
  settimeofday(&tv, NULL);

  // 格式化时间用于日志
  time_t seconds = browser_timestamp_ms / 1000;
  struct tm timeinfo;
  localtime_r(&seconds, &timeinfo);
  char time_str[64];
  strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeinfo);

  ESP_LOGI(TAG, "====================================");
  ESP_LOGI(TAG, "⏰ 时间同步完成");
  ESP_LOGI(TAG, "  浏览器时间: %s", time_str);
  ESP_LOGI(TAG, "  ESP32运行: %llu ms", (unsigned long long)esp_time_ms);
  ESP_LOGI(TAG, "  时间偏移: %lld ms", (long long)new_offset);
  if (old_offset != 0) {
    ESP_LOGI(TAG, "  偏移变化: %lld ms", (long long)(new_offset - old_offset));
  }
  ESP_LOGI(TAG, "====================================");

  return ESP_OK;
}

uint64_t time_manager_get_current_ms(void) {
  uint64_t esp_time_us = esp_timer_get_time();
  uint64_t esp_time_ms = esp_time_us / 1000;

  portENTER_CRITICAL(&g_time_spinlock);
  int64_t offset = g_time_offset_ms;
  portEXIT_CRITICAL(&g_time_spinlock);

  return esp_time_ms + offset;
}

uint64_t time_manager_get_current_us(void) {
  uint64_t esp_time_us = esp_timer_get_time();

  portENTER_CRITICAL(&g_time_spinlock);
  int64_t offset_us = g_time_offset_ms * 1000;
  portEXIT_CRITICAL(&g_time_spinlock);

  return esp_time_us + offset_us;
}

bool time_manager_is_synced(void) {
  portENTER_CRITICAL(&g_time_spinlock);
  bool synced = g_time_synced;
  portEXIT_CRITICAL(&g_time_spinlock);
  return synced;
}

int time_manager_format_time(char *buffer, size_t size, const char *format) {
  if (!buffer || size == 0 || !format) {
    ESP_LOGE(TAG, "无效的参数");
    return 0;
  }

  uint64_t timestamp_ms = time_manager_get_current_ms();
  struct tm tm_info;
  time_manager_timestamp_to_tm(timestamp_ms, &tm_info);

  return strftime(buffer, size, format, &tm_info);
}

void time_manager_timestamp_to_tm(uint64_t timestamp_ms, struct tm *tm_info) {
  if (!tm_info) {
    ESP_LOGE(TAG, "tm_info为空");
    return;
  }

  time_t seconds = timestamp_ms / 1000;
  localtime_r(&seconds, tm_info);
}

uint64_t time_manager_get_last_sync_time(void) {
  portENTER_CRITICAL(&g_time_spinlock);
  uint64_t last_sync = g_last_sync_time_ms;
  int64_t offset = g_time_offset_ms;
  portEXIT_CRITICAL(&g_time_spinlock);

  if (last_sync == 0) {
    return 0; // 从未同步
  }

  return last_sync + offset;
}
