#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sx_work_mode_uart_handler_t)(int port,
                                             const uint8_t *data,
                                             size_t len);

esp_err_t sx_work_mode_init(void);
esp_err_t sx_work_mode_start_by_name(const char *name);
esp_err_t sx_work_mode_stop_current(void);
const char *sx_work_mode_get_current_name(void);
bool sx_work_mode_is_valid(const char *name);
size_t sx_work_mode_get_supported_names(const char **names, size_t capacity);
sx_work_mode_uart_handler_t sx_work_mode_get_uart_handler(void);
void sx_work_mode_handle_uart_data(int port, const uint8_t *data, size_t len);
int sx_work_mode_get_serial_server_port(int port);
bool sx_work_mode_serial_server_listening(int port);
bool sx_work_mode_serial_client_connected(int port);
esp_err_t sx_work_mode_save_serial_server_ports(const uint16_t ports[3]);

#ifdef __cplusplus
}
#endif
