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
void sx_serial_server_handle_uart_data(int port, const uint8_t *data, size_t len);
int sx_serial_server_get_port(int port);
bool sx_serial_server_is_listening(int port);
bool sx_serial_server_client_connected(int port);
esp_err_t sx_serial_server_save_ports(const uint16_t ports[3]);

typedef enum {
    SX_SERIAL_TCP_SERVER = 0,
    SX_SERIAL_TCP_CLIENT,
    SX_SERIAL_MQTT,
    SX_SERIAL_MODBUS_TCP_RTU,
} sx_serial_tcp_mode_t;
typedef enum {
    SX_SERIAL_RUNTIME_STOPPED = 0,
    SX_SERIAL_RUNTIME_CONNECTING,
    SX_SERIAL_RUNTIME_LISTENING,
    SX_SERIAL_RUNTIME_CONNECTED,
    SX_SERIAL_RUNTIME_ERROR,
} sx_serial_runtime_state_t;

typedef struct {
    bool service_running;
    bool listening;
    bool connected;
    bool mqtt_connected;
    sx_serial_runtime_state_t state;
    sx_serial_tcp_mode_t protocol;
    uint16_t local_port;
    char remote_ip[64];
    uint16_t remote_port;
    char mqtt_uri[128];
} sx_serial_runtime_status_t;

esp_err_t sx_serial_server_get_runtime_status(int port,
                                               sx_serial_runtime_status_t *status);
typedef struct {
    int port;
    sx_serial_tcp_mode_t tcp_mode;
    uint16_t local_port;
    char remote_ip[64];
    uint16_t remote_port;
    char mqtt_uri[128];
    char mqtt_username[64];
    char mqtt_password[64];
    char mqtt_client_id[64];
    char mqtt_publish_topic[128];
    char mqtt_subscribe_topic[128];
    uint8_t mqtt_qos;
    bool mqtt_retain;
    int baud_rate, data_bit, check_bit, stop_bit, frame_time, frame_len, timeout;
} sx_serial_port_config_t;
esp_err_t sx_serial_server_get_config(int port, sx_serial_port_config_t *config);
esp_err_t sx_serial_server_save_config(const sx_serial_port_config_t *config);
const char *sx_serial_protocol_name(sx_serial_tcp_mode_t mode);
bool sx_serial_protocol_from_name(const char *name, sx_serial_tcp_mode_t *mode);

#ifdef __cplusplus
}
#endif
