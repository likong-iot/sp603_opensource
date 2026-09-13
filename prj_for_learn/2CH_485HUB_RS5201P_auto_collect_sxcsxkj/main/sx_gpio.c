/*
 * @Author: Orion
 * @Date: 2024-02-03 11:29:35
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-21 14:18:52
 * @FilePath: \SERIIAL_SERVER_P\main\sx_gpio.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "sx_gpio.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 时间字符串缓冲区
EXT_RAM_BSS_ATTR char strftime_buf[64];

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

void init_gpio() {
  if (strftime_buf[0] == '\0') {
    strcpy(strftime_buf, "--:--:--");
  }

  esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
  esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
  esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
  esp_log_level_set("wifi", ESP_LOG_ERROR);

  // 配置按键GPIO
  gpio_reset_pin(KEY);
  gpio_set_direction(KEY, GPIO_MODE_INPUT);
  gpio_set_pull_mode(KEY, GPIO_PULLUP_ONLY);
  ESP_LOGI("GPIO", "按键GPIO1配置完成");
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
