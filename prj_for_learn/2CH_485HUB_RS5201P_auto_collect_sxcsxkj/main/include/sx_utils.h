/*
 * @Author: Orion
 * @Date: 2024-01-11 11:21:24
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-14 16:58:55
 * @FilePath: \SERIIAL_SERVER\main\include\sx_utils.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "cJSON.h"
#include <stdint.h>

#define SX_NETWORK_CORE_ID 0
#define SX_WORK_CORE_ID 1
#define SX_AUX_CORE_ID SX_WORK_CORE_ID

static inline TickType_t sx_ms_to_ticks(uint32_t delay_ms) {
  TickType_t ticks = pdMS_TO_TICKS(delay_ms);
  return (ticks > 0) ? ticks : 1;
}

void urlDecode(char *src, char *dest);
void save_form_data(const char *query);
char *reset_reason_to_string(esp_reset_reason_t reason);
esp_err_t nvs_init();
esp_err_t save_to_nvs(const char *json_string);
double pressure_to_altitude(double pressure);

extern char messageid[48];
// 添加安全重置 NVS 的函数声明
esp_err_t nvs_safe_reset(void);
esp_err_t nvs_reset_clear(void);

char *send_device_info(uint8_t *u_data, int idx);
char *send_device_info_err(void);
char *send_device_ip(void);

esp_err_t perform_factory_reset(const char *work_mode_to_restore);
void protocol_select();

void simple_task();
char* convert_multi_register_data(uint16_t *registers, int reg_count, const char* format);

// 新增函数声明
bool verify_credentials(const char *username, const char *password);
bool update_device_settings(cJSON *parm);

char *heartbeat_config_json(void);

char *get_ip_addr(void);
char *get_gateway(void);
char *get_netmask(void);

char *get_wifi_mac(void);
char *get_version(void);
char *get_device_type(void);
char *get_device_name(void);
char *get_is_dhcp_value(void);
char *get_primary_dns(void);
char *get_secondary_dns(void);

// Modbus CRC计算函数
uint16_t ModbusCRC16(uint8_t *data, uint16_t length);

// PSRAM 定向工具：应用任务的栈放在PSRAM，系统任务保持默认
BaseType_t create_app_task_psram(TaskFunction_t task_fn, const char *name,
                                 uint32_t stack_size_bytes, void *params,
                                 UBaseType_t priority, TaskHandle_t *handle,
                                 BaseType_t core_id);
void delete_app_task_with_caps(TaskHandle_t handle);
void delete_self_app_task_with_caps(void);

// 系统资源打印
void print_system_resource_usage(void);

#ifdef __cplusplus
}
#endif
