/*
 * @Author: Orion
 * @Date: 2024-01-11 11:20:29
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-11 13:53:45
 * @FilePath: \SERIIAL_SERVER\main\include\sx_mqtt_client.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
void mqtt_app_start(void);
int aiot_platform_publish_topic(const char *topic, const char *data, int len,
                                int qos, int retain);
void mqtt_public_send_device_info(uint8_t *u_data, int data_len, int template_index);
void mqtt_public_send_mqtt_tcp_data(uint8_t *data, int data_len);
void mqtt_public_send_modbus_tcp_data(uint8_t *data, int data_len);
#ifdef __cplusplus
}
#endif
