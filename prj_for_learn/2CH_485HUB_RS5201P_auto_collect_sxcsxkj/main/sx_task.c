/*
 * @Author: Orion
 * @Date: 2024-02-03 11:29:54
 * @LastEditors: Orion
 * @LastEditTime: 2025-03-07 13:41:02
 * @FilePath: \SERIIAL_SERVER\main\sx_task.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sx_timer_tasks.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

void led_task(void *param) {
  // LED任务已移除，因为LED现在由UART控制
  vTaskDelete(NULL);
}

void key_task(void *param) {
  ESP_LOGI("KEY", "按键处理任务启动");
  while (1) {
    // 检查短按标志
    if (key_short_press_detected) {
      key_short_press_detected = false; // 清除标志
      ESP_LOGI("KEY", "执行短按重启操作");
      // 执行重启操作
      ESP_LOGI("KEY", "正在重启设备...");
      vTaskDelay(500 / portTICK_PERIOD_MS); // 短暂延时
      esp_restart();
    }

    // 检查长按标志
    if (key_long_press_detected) {
      key_long_press_detected = false; // 清除标志
      ESP_LOGI("KEY", "执行长按重置操作");
      esp_err_t err = perform_factory_reset(NULL);
      if (err != ESP_OK) {
        ESP_LOGE("KEY", "工厂重置失败: %s", esp_err_to_name(err));
      } else {
        ESP_LOGI("KEY", "工厂重置完成，准备重启...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
      }
    }
    vTaskDelay(50 / portTICK_PERIOD_MS); // 50ms检查周期
  }
}

void close_ap_task(void *pvParameter) {
  // 等待五分钟
  vTaskDelay(10 * 60 * 1000 / portTICK_PERIOD_MS);
  // vTaskDelay(5000);
  printf("close success\n");
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // 关闭AP模式，保持STA连接
  // esp_wifi_set_mode(WIFI_MODE_STA);

  // 删除这个任务
  vTaskDelete(NULL);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
