/*
 * @Author: Orion
 * @Date: 2024-01-11 11:20:48
 * @LastEditors: Orion
 * @LastEditTime: 2025-03-13 17:25:57
 * @FilePath: \SERIIAL_SERVER\main\include\sx_tcp_client.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
void start_tcp_client(void);
void tcpclient_message(char *message,int rxBytes);
extern int global_client_sock;
extern int global_tcp_server;
void tcp_client_send_device_info(uint8_t *u_data, int data_len, int template_index);
#ifdef __cplusplus
}
#endif
