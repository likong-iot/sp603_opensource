#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t sx_serial_server_init(void);
esp_err_t sx_serial_server_start(void);
esp_err_t sx_serial_server_stop(void);
void sx_serial_server_handle_uart_data(int channel, const uint8_t *data, size_t len);
int sx_serial_server_get_port(int channel);
bool sx_serial_server_is_listening(int channel);
bool sx_serial_server_client_connected(int channel);
esp_err_t sx_serial_server_save_ports(const uint16_t ports[3]);

typedef enum { SX_SERIAL_TCP_SERVER = 0, SX_SERIAL_TCP_CLIENT = 1 } sx_serial_tcp_mode_t;
typedef struct {
    int channel;
    sx_serial_tcp_mode_t tcp_mode;
    uint16_t local_port;
    char remote_ip[64];
    uint16_t remote_port;
    int baud_rate, data_bit, check_bit, stop_bit, frame_time, frame_len, timeout;
} sx_serial_channel_config_t;
esp_err_t sx_serial_server_get_config(int channel, sx_serial_channel_config_t *config);
esp_err_t sx_serial_server_save_config(const sx_serial_channel_config_t *config);

#ifdef __cplusplus
}
#endif
