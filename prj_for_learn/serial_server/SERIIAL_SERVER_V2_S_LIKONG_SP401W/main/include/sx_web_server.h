/*
 * @Author: Orion
 * @Date: 2024-01-23 16:10:01
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 19:17:50
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
extern char device_eth_mac[24];
void http_server_init(void);
void send_log_to_websocket(const char* log_msg);
void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx);
// 版本配置 - 可通过编译选项覆盖
#ifndef DEVICE_MODEL
#define DEVICE_MODEL "SP501W"
#endif

#ifndef BRAND_TYPE
#define BRAND_TYPE "LIKONG"  // LIKONG 或 NEUTRAL
#endif

#ifndef BRAND_NAME_CN
#define BRAND_NAME_CN "立控电子"  // 立控电子 或 空字符串（中性版本）
#endif

#ifndef BRAND_NAME_EN
#define BRAND_NAME_EN "LIKONG"  // LIKONG 或 空字符串（中性版本）
#endif

#define VERSION "HW:1.0.0_SDK:2.2.1"
#define DEVICE_TYPE DEVICE_MODEL
#define CLIENT_HEAD "SP501"  // 保持不变，用于平台通信
#ifdef __cplusplus
}
#endif
