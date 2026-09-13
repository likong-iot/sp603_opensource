#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VERSION "HW:1.0.0_SDK:1.0.1_main_develop"
#define DEVICE_TYPE "SP603"
#define CLIENT_HEAD "SP603"

esp_err_t sx_web_server_init_storage_defaults(void);
void http_server_init(void);

void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx, int channel);
void send_uart_to_websocket_async(const uint8_t *data, size_t len, bool is_tx, int channel);
void send_uart_to_websocket_from_port(const uint8_t *data, size_t len, bool is_tx, uart_port_t uart_num);
void send_uart_event_to_websocket(int channel, const char *level, const char *source, const char *text);

#ifdef __cplusplus
}
#endif
