/*
 * @Author: Orion
 * @Date: 2024-01-23 16:10:01
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-19 16:10:02
 * @FilePath: \SERIIAL_SERVER\main\include\sx_web_server.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
#include <stdbool.h>
#include "esp_http_server.h"
#include "driver/uart.h"
extern char dIp[20];
extern char dNetmask[20];
extern char dGateway[20];
extern uint8_t mac_addr[6];
extern char device_mac[30];
extern char th[20];
extern char device_mac_end[20];
extern char got_ip_addrs[16];
extern char got_ip_netmask[16];
extern char got_ip_gw[16];
extern int httpreason;
extern char device_sta_mac[24];

void http_server_init(void);

void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx, int channel);
void send_uart_to_websocket_from_port(const uint8_t *data, size_t len, bool is_tx, uart_port_t uart_num);
// #define VERSION "1.0.7-release"
#define VERSION "HW:2.0.0_SDK:1.0.3_main_develop"
#define DEVICE_TYPE "RS5201"
#define CLIENT_HEAD "RS5201"
#ifdef __cplusplus
}
#endif
