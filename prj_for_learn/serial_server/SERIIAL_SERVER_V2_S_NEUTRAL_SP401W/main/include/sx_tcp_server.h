/*
 * @Author: Orion
 * @Date: 2024-01-11 11:21:13
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-13 11:51:59
 * @FilePath: \SERIIAL_SERVER\main\include\sx_tcp_server.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include <stdint.h>
void start_tcp_server(void);
// void tcp_uart_send(int tcpSock, uint8_t *data, size_t length);
extern int global_sock;
extern int tcp_client_count;
void tcpserver_message(char *message,int rxBytes);

void tcp_server_send_device_info(uint8_t *u_data, int data_len, int template_index);
#ifdef __cplusplus
}
#endif
