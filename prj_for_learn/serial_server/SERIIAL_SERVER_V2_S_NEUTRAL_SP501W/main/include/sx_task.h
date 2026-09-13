/*
 * @Author: Orion
 * @Date: 2024-02-03 11:27:12
 * @LastEditors: Orion
 * @LastEditTime: 2024-05-08 15:33:33
 * @FilePath: \SERIIAL_SERVER\main\include\sx_task.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

void led_task(void *param);
void key_task(void *param);
void i2cRead(void *arg);
void free_task(void *param);
void close_ap_task(void *pvParameter);
#ifdef __cplusplus
}
#endif
