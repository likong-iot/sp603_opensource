#include "sx_serial_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "async_uart.h"
#include "app_task_utils.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "sx_serial_port_manager.h"
#include "sx_web_server.h"

#define SERIAL_PORT_COUNT 3
#define SERIAL_SERVER_DEFAULT_PORT 8888
#define MODBUS_TCP_MAX_ADU 260
#define MODBUS_RTU_MAX_ADU 256

static const char *TAG = "serial_server";
static TaskHandle_t s_port_tasks[SERIAL_PORT_COUNT + 1];
static volatile bool s_serial_server_running;
static volatile int s_listen_fds[SERIAL_PORT_COUNT + 1] = {-1, -1, -1, -1};
static volatile int s_client_fds[SERIAL_PORT_COUNT + 1] = {-1, -1, -1, -1};
static uint16_t s_tcp_ports[SERIAL_PORT_COUNT + 1] = {0, 8888, 8889, 8890};
static esp_mqtt_client_handle_t s_mqtt_clients[SERIAL_PORT_COUNT + 1];
static bool s_mqtt_connected[SERIAL_PORT_COUNT + 1];
static bool s_mqtt_error[SERIAL_PORT_COUNT + 1];
static sx_serial_port_config_t s_runtime_configs[SERIAL_PORT_COUNT + 1];
static uint8_t s_mqtt_rx_buffers[SERIAL_PORT_COUNT + 1][512];
static size_t s_mqtt_rx_lengths[SERIAL_PORT_COUNT + 1];

static void serial_config_key(char *key, size_t key_size, int port,
                              const char *field)
{
    snprintf(key, key_size, "%s_%s", sx_serial_port_manager_port_key(port), field);
}

typedef struct {
    SemaphoreHandle_t lock;
    bool waiting;
    uint16_t transaction_id;
    uint8_t unit_id;
    uint8_t function_code;
    int socket_fd;
    int64_t deadline_us;
} modbus_bridge_state_t;

static modbus_bridge_state_t s_modbus_states[SERIAL_PORT_COUNT + 1];

static const char *serial_mqtt_bus_name(int port)
{
    return sx_serial_port_manager_port_key(port);
}

static void set_serial_mqtt_defaults(int port, sx_serial_port_config_t *config)
{
    uint8_t mac[6] = {0};
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_text[13];
    snprintf(mac_text, sizeof(mac_text), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const char *bus = serial_mqtt_bus_name(port);

    snprintf(config->mqtt_uri, sizeof(config->mqtt_uri),
             "mqtt://mqtt.likong-iot.com:1883");
    snprintf(config->mqtt_username, sizeof(config->mqtt_username), "public");
    snprintf(config->mqtt_password, sizeof(config->mqtt_password), "Aa123456");
    snprintf(config->mqtt_client_id, sizeof(config->mqtt_client_id),
             "ST200_%s_%s", mac_text, bus);
    /* Topic names follow the platform perspective used by the reference service. */
    snprintf(config->mqtt_publish_topic, sizeof(config->mqtt_publish_topic),
             "/public/%s/subscribe/%s", mac_text, bus);
    snprintf(config->mqtt_subscribe_topic, sizeof(config->mqtt_subscribe_topic),
             "/public/%s/publish/%s", mac_text, bus);
    config->mqtt_qos = 0;
    config->mqtt_retain = false;
}

static void close_socket(volatile int *fd)
{
    int value = *fd;
    if (value >= 0) {
        shutdown(value, SHUT_RDWR);
        close(value);
        *fd = -1;
    }
}

const char *sx_serial_protocol_name(sx_serial_tcp_mode_t mode)
{
    switch (mode) {
    case SX_SERIAL_TCP_CLIENT: return "tcp_client";
    case SX_SERIAL_MQTT: return "mqtt";
    case SX_SERIAL_MODBUS_TCP_RTU: return "modbus_tcp_rtu";
    default: return "tcp_server";
    }
}

bool sx_serial_protocol_from_name(const char *name, sx_serial_tcp_mode_t *mode)
{
    if (name == NULL || mode == NULL) return false;
    if (strcmp(name, "tcp_server") == 0 || strcmp(name, "server") == 0)
        *mode = SX_SERIAL_TCP_SERVER;
    else if (strcmp(name, "tcp_client") == 0 || strcmp(name, "client") == 0)
        *mode = SX_SERIAL_TCP_CLIENT;
    else if (strcmp(name, "mqtt") == 0)
        *mode = SX_SERIAL_MQTT;
    else if (strcmp(name, "modbus_tcp_rtu") == 0)
        *mode = SX_SERIAL_MODBUS_TCP_RTU;
    else
        return false;
    return true;
}

static uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc & 1U) ? (uint16_t)((crc >> 1U) ^ 0xa001U) : (uint16_t)(crc >> 1U);
    }
    return crc;
}

static bool modbus_tcp_adu_to_rtu(const uint8_t *tcp, size_t tcp_len,
                                  uint8_t *rtu, size_t rtu_capacity,
                                  size_t *rtu_len, uint16_t *transaction_id,
                                  uint8_t *unit_id, uint8_t *function_code)
{
    if (tcp == NULL || rtu == NULL || rtu_len == NULL || transaction_id == NULL ||
        unit_id == NULL || function_code == NULL || tcp_len < 8 ||
        tcp_len > MODBUS_TCP_MAX_ADU || tcp[2] != 0 || tcp[3] != 0) {
        return false;
    }
    uint16_t mbap_len = ((uint16_t)tcp[4] << 8U) | tcp[5];
    size_t payload_len = tcp_len - 6U;
    if (mbap_len < 2 || payload_len != mbap_len || payload_len + 2U > rtu_capacity) {
        return false;
    }
    memcpy(rtu, tcp + 6, payload_len);
    uint16_t crc = modbus_crc16(rtu, payload_len);
    rtu[payload_len] = (uint8_t)crc;
    rtu[payload_len + 1U] = (uint8_t)(crc >> 8U);
    *rtu_len = payload_len + 2U;
    *transaction_id = ((uint16_t)tcp[0] << 8U) | tcp[1];
    *unit_id = tcp[6];
    *function_code = tcp[7];
    return true;
}

static bool modbus_rtu_adu_to_tcp(const uint8_t *rtu, size_t rtu_len,
                                  uint16_t transaction_id, uint8_t expected_unit_id,
                                  uint8_t expected_function_code,
                                  uint8_t *tcp, size_t tcp_capacity, size_t *tcp_len)
{
    if (rtu == NULL || tcp == NULL || tcp_len == NULL || rtu_len < 5 ||
        rtu_len > MODBUS_RTU_MAX_ADU || tcp_capacity < rtu_len + 4U ||
        rtu[0] != expected_unit_id ||
        (rtu[1] != expected_function_code &&
         rtu[1] != (uint8_t)(expected_function_code | 0x80U))) {
        return false;
    }
    uint16_t received_crc = (uint16_t)rtu[rtu_len - 2U] |
                            ((uint16_t)rtu[rtu_len - 1U] << 8U);
    if (modbus_crc16(rtu, rtu_len - 2U) != received_crc) return false;

    tcp[0] = (uint8_t)(transaction_id >> 8U);
    tcp[1] = (uint8_t)transaction_id;
    tcp[2] = tcp[3] = 0;
    uint16_t mbap_len = (uint16_t)(rtu_len - 2U);
    tcp[4] = (uint8_t)(mbap_len >> 8U);
    tcp[5] = (uint8_t)mbap_len;
    memcpy(tcp + 6, rtu, mbap_len);
    *tcp_len = 6U + mbap_len;
    return true;
}

static bool config_changed(int port, const sx_serial_port_config_t *running)
{
    sx_serial_port_config_t latest;
    return sx_serial_server_get_config(port, &latest) != ESP_OK ||
           memcmp(&latest, running, sizeof(latest)) != 0;
}

static bool wait_for_retry_or_config_change(int port,
                                            const sx_serial_port_config_t *running,
                                            uint32_t delay_ms)
{
    const uint32_t interval_ms = 100;
    uint32_t waited_ms = 0;
    while (s_serial_server_running && waited_ms < delay_ms) {
        if (config_changed(port, running)) return true;
        uint32_t remaining_ms = delay_ms - waited_ms;
        uint32_t sleep_ms = remaining_ms < interval_ms ? remaining_ms : interval_ms;
        vTaskDelay(pdMS_TO_TICKS(sleep_ms));
        waited_ms += sleep_ms;
    }
    return config_changed(port, running);
}

static int connect_client_interruptible(int port,
                                        const sx_serial_port_config_t *running,
                                        const struct sockaddr_in *address,
                                        bool *config_was_changed)
{
    *config_was_changed = false;
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (const struct sockaddr *)address, sizeof(*address)) == 0)
        return fd;
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    for (int elapsed_ms = 0; s_serial_server_running && elapsed_ms < 5000;
         elapsed_ms += 100) {
        if (config_changed(port, running)) {
            *config_was_changed = true;
            break;
        }
        fd_set write_fds;
        fd_set error_fds;
        FD_ZERO(&write_fds);
        FD_ZERO(&error_fds);
        FD_SET(fd, &write_fds);
        FD_SET(fd, &error_fds);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 100000};
        int ready = select(fd + 1, NULL, &write_fds, &error_fds, &timeout);
        if (ready > 0) {
            int socket_error = 0;
            socklen_t error_length = sizeof(socket_error);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) == 0 &&
                socket_error == 0 && FD_ISSET(fd, &write_fds)) {
                return fd;
            }
            break;
        }
        if (ready < 0 && errno != EINTR) break;
    }
    close(fd);
    return -1;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    int port = (int)(intptr_t)arg;
    if (port < 1 || port > SERIAL_PORT_COUNT) return;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_mqtt_connected[port] = true;
        s_mqtt_error[port] = false;
        const sx_serial_port_config_t *cfg = &s_runtime_configs[port];
        if (cfg->mqtt_subscribe_topic[0] != '\0')
            esp_mqtt_client_subscribe(event->client, cfg->mqtt_subscribe_topic, cfg->mqtt_qos);
        ESP_LOGI(TAG, "%s MQTT connected",
                 sx_serial_port_manager_port_label(port));
        char connected_text[96];
        snprintf(connected_text, sizeof(connected_text), "%s MQTT connected",
                 sx_serial_port_manager_port_label(port));
        send_system_log("INFO", "mqtt", connected_text);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        s_mqtt_connected[port] = false;
        ESP_LOGW(TAG, "%s MQTT disconnected", sx_serial_port_manager_port_label(port));
        char disconnect_text[96];
        snprintf(disconnect_text, sizeof(disconnect_text), "%s MQTT disconnected",
                 sx_serial_port_manager_port_label(port));
        send_system_log("WARN", "mqtt", disconnect_text);
    } else if (event_id == MQTT_EVENT_ERROR) {
        s_mqtt_connected[port] = false;
        s_mqtt_error[port] = true;
        if (event->error_handle == NULL) {
            ESP_LOGE(TAG, "%s MQTT error without details",
                     sx_serial_port_manager_port_label(port));
            send_system_log("ERROR", "mqtt", "MQTT error without details");
        } else {
            const esp_mqtt_error_codes_t *error = event->error_handle;
            ESP_LOGE(TAG,
                     "%s MQTT error type=%d connect_return=%d tls_err=0x%x "
                     "tls_stack=%d cert_flags=0x%x sock_errno=%d (%s)",
                     sx_serial_port_manager_port_label(port), error->error_type,
                     error->connect_return_code, (unsigned)error->esp_tls_last_esp_err,
                     error->esp_tls_stack_err, error->esp_tls_cert_verify_flags,
                     error->esp_transport_sock_errno,
                     strerror(error->esp_transport_sock_errno));
            char error_text[192];
            snprintf(error_text, sizeof(error_text),
                     "%s MQTT error type=%d return=%d tls=0x%x errno=%d (%s)",
                     sx_serial_port_manager_port_label(port), error->error_type,
                     error->connect_return_code, (unsigned)error->esp_tls_last_esp_err,
                     error->esp_transport_sock_errno,
                     strerror(error->esp_transport_sock_errno));
            send_system_log("ERROR", "mqtt", error_text);
        }
    } else if (event_id == MQTT_EVENT_DATA) {
        if (event->total_data_len <= 0 || event->total_data_len > 512 ||
            event->current_data_offset < 0 || event->data_len < 0 ||
            event->current_data_offset + event->data_len > event->total_data_len) {
            s_mqtt_rx_lengths[port] = 0;
            return;
        }
        if (event->current_data_offset == 0) s_mqtt_rx_lengths[port] = 0;
        memcpy(s_mqtt_rx_buffers[port] + event->current_data_offset,
               event->data, (size_t)event->data_len);
        s_mqtt_rx_lengths[port] = (size_t)(event->current_data_offset + event->data_len);
        if (s_mqtt_rx_lengths[port] == (size_t)event->total_data_len) {
            send_data_to_port(port, (char *)s_mqtt_rx_buffers[port],
                                 s_mqtt_rx_lengths[port]);
            s_mqtt_rx_lengths[port] = 0;
        }
    }
}

static bool load_port_value(nvs_handle_t nvs, const char *key, uint16_t *port)
{
    uint32_t number = 0;
    if (nvs_get_u32(nvs, key, &number) == ESP_OK &&
        number >= 1 && number <= 65535) {
        *port = (uint16_t)number;
        return true;
    }
    char text[16] = {0};
    size_t length = sizeof(text);
    if (nvs_get_str(nvs, key, text, &length) == ESP_OK) {
        long value = strtol(text, NULL, 10);
        if (value >= 1 && value <= 65535) {
            *port = (uint16_t)value;
            return true;
        }
    }
    return false;
}

static void load_serial_server_ports(void)
{
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port)
        s_tcp_ports[port] = SERIAL_SERVER_DEFAULT_PORT + port - 1;

    nvs_handle_t nvs = 0;
    if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) return;
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        char key[16];
        serial_config_key(key, sizeof(key), port, "port");
        if (!load_port_value(nvs, key, &s_tcp_ports[port]) && port == 1)
            load_port_value(nvs, "tcp_port", &s_tcp_ports[port]);
    }
    nvs_close(nvs);
}

esp_err_t sx_serial_server_init(void)
{
    load_serial_server_ports();
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        if (s_modbus_states[port].lock == NULL) {
            s_modbus_states[port].lock = xSemaphoreCreateMutex();
            if (s_modbus_states[port].lock == NULL) return ESP_ERR_NO_MEM;
        }
        s_modbus_states[port].socket_fd = -1;
    }
    return ESP_OK;
}

int sx_serial_server_get_port(int port)
{
    if (port < 1 || port > SERIAL_PORT_COUNT) return -1;
    load_serial_server_ports();
    return s_tcp_ports[port];
}

bool sx_serial_server_is_listening(int port)
{
    return port >= 1 && port <= SERIAL_PORT_COUNT &&
           s_serial_server_running && s_listen_fds[port] >= 0;
}

bool sx_serial_server_client_connected(int port)
{
    return port >= 1 && port <= SERIAL_PORT_COUNT &&
           s_serial_server_running && s_client_fds[port] >= 0;
}

esp_err_t sx_serial_server_get_runtime_status(int port,
                                               sx_serial_runtime_status_t *status)
{
    if (status == NULL || port < 1 || port > SERIAL_PORT_COUNT)
        return ESP_ERR_INVALID_ARG;
    memset(status, 0, sizeof(*status));
    status->service_running = s_serial_server_running &&
                              sx_serial_port_manager_port_available(port);
    sx_serial_port_config_t config;
    esp_err_t err = sx_serial_server_get_config(port, &config);
    if (err != ESP_OK) return err;
    status->protocol = config.tcp_mode;
    status->local_port = config.local_port;
    status->remote_port = config.remote_port;
    snprintf(status->remote_ip, sizeof(status->remote_ip), "%s", config.remote_ip);
    snprintf(status->mqtt_uri, sizeof(status->mqtt_uri), "%s", config.mqtt_uri);
    status->listening = s_listen_fds[port] >= 0;
    status->connected = s_client_fds[port] >= 0;
    status->mqtt_connected = s_mqtt_connected[port];
    if (!status->service_running) {
        status->state = SX_SERIAL_RUNTIME_STOPPED;
    } else if (status->mqtt_connected || status->connected) {
        status->state = SX_SERIAL_RUNTIME_CONNECTED;
    } else if (status->protocol == SX_SERIAL_MQTT && s_mqtt_error[port]) {
        status->state = SX_SERIAL_RUNTIME_ERROR;
    } else if (status->listening) {
        status->state = SX_SERIAL_RUNTIME_LISTENING;
    } else {
        status->state = SX_SERIAL_RUNTIME_CONNECTING;
    }
    return ESP_OK;
}

esp_err_t sx_serial_server_save_ports(const uint16_t ports[3])
{
    if (ports == NULL) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < SERIAL_PORT_COUNT; ++i) {
        if (ports[i] == 0) return ESP_ERR_INVALID_ARG;
        for (int j = i + 1; j < SERIAL_PORT_COUNT; ++j) {
            if (ports[i] == ports[j]) return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open("storage", NVS_READWRITE, &nvs),
                        TAG, "open NVS failed");
    esp_err_t err = ESP_OK;
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        char key[16];
        serial_config_key(key, sizeof(key), port, "port");
        err = nvs_set_u32(nvs, key, ports[port - 1]);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static int create_listener(int serial_port, uint16_t tcp_port)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(tcp_port),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "%s TCP listen port %u failed: errno=%d",
                 sx_serial_port_manager_port_label(serial_port),
                 tcp_port, errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "%s serial server listening on TCP %u",
             sx_serial_port_manager_port_label(serial_port),
             tcp_port);
    return fd;
}

void sx_serial_server_handle_uart_data(int port,
                                       const uint8_t *data,
                                       size_t len)
{
    if (port < 1 || port > SERIAL_PORT_COUNT ||
        data == NULL || len == 0 || !s_serial_server_running) {
        return;
    }
    sx_serial_port_config_t *cfg = &s_runtime_configs[port];
    if (cfg->tcp_mode == SX_SERIAL_MQTT) {
        esp_mqtt_client_handle_t client = s_mqtt_clients[port];
        if (client != NULL && s_mqtt_connected[port] &&
            cfg->mqtt_publish_topic[0] != '\0') {
            esp_mqtt_client_publish(client, cfg->mqtt_publish_topic,
                                    (const char *)data, (int)len,
                                    cfg->mqtt_qos, cfg->mqtt_retain ? 1 : 0);
        }
        return;
    }
    if (cfg->tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) {
        modbus_bridge_state_t *state = &s_modbus_states[port];
        if (state->lock == NULL || len < 5 ||
            xSemaphoreTake(state->lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
        if (state->waiting && state->socket_fd >= 0) {
            uint8_t response[MODBUS_TCP_MAX_ADU];
            size_t response_len = 0;
            if (modbus_rtu_adu_to_tcp(data, len, state->transaction_id,
                                      state->unit_id, state->function_code,
                                      response, sizeof(response), &response_len)) {
                send(state->socket_fd, response, response_len, MSG_DONTWAIT);
                state->waiting = false;
            }
        }
        xSemaphoreGive(state->lock);
        return;
    }
    int client = s_client_fds[port];
    if (client < 0) return;
    int sent = send(client, data, len, MSG_DONTWAIT);
    if (sent < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        ESP_LOGW(TAG, "%s TCP client send failed: errno=%d",
                 sx_serial_port_manager_port_label(port), errno);
    }
}

static void close_all_serial_sockets(void)
{
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        close_socket(&s_client_fds[port]);
        close_socket(&s_listen_fds[port]);
    }
}

static void send_modbus_exception(int fd, uint16_t transaction_id, uint8_t unit_id,
                                  uint8_t function_code, uint8_t exception_code)
{
    uint8_t response[9] = {
        (uint8_t)(transaction_id >> 8U), (uint8_t)transaction_id,
        0, 0, 0, 3, unit_id, (uint8_t)(function_code | 0x80U), exception_code,
    };
    if (fd >= 0) send(fd, response, sizeof(response), MSG_DONTWAIT);
}

static void handle_modbus_tcp_adu(int port, int fd, const uint8_t *adu, size_t len,
                                  const sx_serial_port_config_t *cfg)
{
    uint8_t rtu[MODBUS_RTU_MAX_ADU];
    size_t rtu_len = 0;
    uint16_t transaction_id = 0;
    uint8_t unit_id = 0;
    uint8_t function_code = 0;
    if (!modbus_tcp_adu_to_rtu(adu, len, rtu, sizeof(rtu), &rtu_len,
                               &transaction_id, &unit_id, &function_code)) {
        return;
    }
    modbus_bridge_state_t *state = &s_modbus_states[port];
    if (xSemaphoreTake(state->lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        send_modbus_exception(fd, transaction_id, unit_id, function_code, 0x06);
        return;
    }
    if (state->waiting) {
        xSemaphoreGive(state->lock);
        send_modbus_exception(fd, transaction_id, unit_id, function_code, 0x06);
        return;
    }
    state->waiting = true;
    state->transaction_id = transaction_id;
    state->unit_id = unit_id;
    state->function_code = function_code;
    state->socket_fd = fd;
    state->deadline_us = esp_timer_get_time() + (int64_t)cfg->timeout * 1000LL;
    xSemaphoreGive(state->lock);
    send_data_to_port(port, (char *)rtu, rtu_len);
}

static void check_modbus_timeout(int port)
{
    modbus_bridge_state_t *state = &s_modbus_states[port];
    if (xSemaphoreTake(state->lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
    if (state->waiting && esp_timer_get_time() >= state->deadline_us) {
        send_modbus_exception(state->socket_fd, state->transaction_id, state->unit_id,
                              state->function_code, 0x0b);
        state->waiting = false;
    }
    xSemaphoreGive(state->lock);
}

static void clear_modbus_request(int port, int fd)
{
    modbus_bridge_state_t *state = &s_modbus_states[port];
    if (xSemaphoreTake(state->lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
    if (state->socket_fd == fd) {
        state->waiting = false;
        state->socket_fd = -1;
    }
    xSemaphoreGive(state->lock);
}

static void run_mqtt_port(int port, const sx_serial_port_config_t *cfg)
{
    s_runtime_configs[port] = *cfg;
    const sx_serial_port_config_t *runtime = &s_runtime_configs[port];
    ESP_LOGI(TAG, "%s MQTT starting uri=%s client_id=%s user=%s",
             sx_serial_port_manager_port_label(port), runtime->mqtt_uri,
             runtime->mqtt_client_id[0] ? runtime->mqtt_client_id : "<auto>",
             runtime->mqtt_username[0] ? runtime->mqtt_username : "<none>");
    char start_text[192];
    snprintf(start_text, sizeof(start_text), "%s MQTT starting uri=%.64s client_id=%.32s user=%.16s",
             sx_serial_port_manager_port_label(port), runtime->mqtt_uri,
             runtime->mqtt_client_id[0] ? runtime->mqtt_client_id : "<auto>",
             runtime->mqtt_username[0] ? runtime->mqtt_username : "<none>");
    send_system_log("INFO", "mqtt", start_text);
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = runtime->mqtt_uri,
        .credentials.username = runtime->mqtt_username[0] ? runtime->mqtt_username : NULL,
        .credentials.client_id = runtime->mqtt_client_id[0] ? runtime->mqtt_client_id : NULL,
        .credentials.authentication.password = runtime->mqtt_password[0] ? runtime->mqtt_password : NULL,
        .network.reconnect_timeout_ms = 3000,
        .network.timeout_ms = 5000,
        .task.stack_size = 4096,
        .buffer.size = 1024,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        s_mqtt_error[port] = true;
        ESP_LOGE(TAG, "%s MQTT client init failed",
                 sx_serial_port_manager_port_label(port));
        send_system_log("ERROR", "mqtt", "MQTT client init failed");
        return;
    }
    s_mqtt_clients[port] = client;
    s_mqtt_connected[port] = false;
    s_mqtt_error[port] = false;
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler,
                                   (void *)(intptr_t)port);
    if (esp_mqtt_client_start(client) != ESP_OK) {
        s_mqtt_error[port] = true;
        ESP_LOGE(TAG, "%s MQTT client start failed",
                 sx_serial_port_manager_port_label(port));
        send_system_log("ERROR", "mqtt", "MQTT client start failed");
        esp_mqtt_client_destroy(client);
        s_mqtt_clients[port] = NULL;
        return;
    }
    while (s_serial_server_running && !config_changed(port, runtime))
        vTaskDelay(pdMS_TO_TICKS(250));
    s_mqtt_connected[port] = false;
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    s_mqtt_clients[port] = NULL;
}

static void serial_port_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    uint8_t buffer[1024];
    while (s_serial_server_running) {
        sx_serial_port_config_t cfg;
        sx_serial_server_get_config(port, &cfg);
        s_runtime_configs[port] = cfg;
        if (cfg.tcp_mode == SX_SERIAL_MQTT) {
            run_mqtt_port(port, &cfg);
            if (s_serial_server_running) vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        int endpoint = -1;
        if (cfg.tcp_mode == SX_SERIAL_TCP_SERVER ||
            cfg.tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) {
            s_listen_fds[port] = create_listener(port, cfg.local_port);
            if (s_listen_fds[port] < 0) {
                wait_for_retry_or_config_change(port, &cfg, 2000);
                continue;
            }
            while (s_serial_server_running && !config_changed(port, &cfg)) {
                fd_set accept_fds;
                FD_ZERO(&accept_fds);
                FD_SET(s_listen_fds[port], &accept_fds);
                struct timeval accept_timeout = {.tv_sec = 0, .tv_usec = 250000};
                int ready = select(s_listen_fds[port] + 1, &accept_fds, NULL, NULL,
                                   &accept_timeout);
                if (ready > 0) {
                    endpoint = accept(s_listen_fds[port], NULL, NULL);
                    break;
                }
            }
            if (endpoint >= 0) s_client_fds[port] = endpoint;
        } else {
            struct sockaddr_in addr = {
                .sin_family = AF_INET,
                .sin_port = htons(cfg.remote_port),
            };
            if (!inet_aton(cfg.remote_ip, &addr.sin_addr)) {
                wait_for_retry_or_config_change(port, &cfg, 2000);
                continue;
            }
            bool config_was_changed = false;
            endpoint = connect_client_interruptible(port, &cfg, &addr,
                                                    &config_was_changed);
            if (endpoint < 0) {
                if (!config_was_changed)
                    wait_for_retry_or_config_change(port, &cfg, 2000);
                continue;
            }
            s_client_fds[port] = endpoint;
        }

        size_t buffered = 0;
        while (s_serial_server_running && endpoint >= 0 && !config_changed(port, &cfg)) {
            fd_set rfds;
            FD_ZERO(&rfds);
            FD_SET(endpoint, &rfds);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 250000};
            int ready = select(endpoint + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0 && FD_ISSET(endpoint, &rfds)) {
                int received = recv(endpoint, buffer + buffered, sizeof(buffer) - buffered, 0);
                if (received <= 0) break;
                if (cfg.tcp_mode != SX_SERIAL_MODBUS_TCP_RTU) {
                    send_data_to_port(port, (char *)buffer, (size_t)received);
                } else {
                    buffered += (size_t)received;
                    while (buffered >= 6) {
                        uint16_t mbap_len = ((uint16_t)buffer[4] << 8U) | buffer[5];
                        size_t adu_len = 6U + mbap_len;
                        if (mbap_len < 2 || adu_len > MODBUS_TCP_MAX_ADU) {
                            buffered = 0;
                            break;
                        }
                        if (buffered < adu_len) break;
                        handle_modbus_tcp_adu(port, endpoint, buffer, adu_len, &cfg);
                        memmove(buffer, buffer + adu_len, buffered - adu_len);
                        buffered -= adu_len;
                    }
                }
            }
            if (cfg.tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) check_modbus_timeout(port);
        }
        if (cfg.tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) clear_modbus_request(port, endpoint);
        close_socket(&s_client_fds[port]);
        close_socket(&s_listen_fds[port]);
        if (s_serial_server_running) vTaskDelay(pdMS_TO_TICKS(250));
    }
    s_port_tasks[port] = NULL;
    delete_self_app_task_with_caps();
}

esp_err_t sx_serial_server_start(void)
{
    for (int i = 1; i <= SERIAL_PORT_COUNT; ++i) if (s_port_tasks[i] != NULL) return ESP_OK;
    s_serial_server_running = true;
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        if (!sx_serial_port_manager_port_available(port)) continue;
        char name[20]; snprintf(name, sizeof(name), "serial_port%d", port);
        if (create_app_task_psram(serial_port_task, name, 6144, (void *)(intptr_t)port, 6,
                                  &s_port_tasks[port], tskNO_AFFINITY) != pdPASS) {
            s_serial_server_running = false; close_all_serial_sockets(); return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "SP603 per-port serial server tasks started");
    return ESP_OK;
}

esp_err_t sx_serial_server_stop(void)
{
    s_serial_server_running = false;
    close_all_serial_sockets();
    for (int i = 0; i < 100; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bool running = false; for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) running |= s_port_tasks[port] != NULL;
        if (!running) break;
    }
    ESP_LOGI(TAG, "serial_server mode stopped");
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) if (s_port_tasks[port] != NULL) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

esp_err_t sx_serial_server_get_config(int port, sx_serial_port_config_t *config)
{
    if (!config || port < 1 || port > SERIAL_PORT_COUNT) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    config->port = port;
    config->tcp_mode = SX_SERIAL_TCP_SERVER;
    config->local_port = SERIAL_SERVER_DEFAULT_PORT + port - 1;
    config->remote_port = config->local_port;
    set_serial_mqtt_defaults(port, config);
    config->baud_rate = 9600;
    config->data_bit = 8;
    config->check_bit = 0;
    config->stop_bit = 1;
    config->frame_time = 50;
    config->frame_len = 512;
    config->timeout = 500;
    nvs_handle_t nvs;
    if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) return ESP_OK;
    char key[32], text[160];
    size_t len;
    uint32_t value;
    uint8_t u8;
    serial_config_key(key, sizeof(key), port, "port");
    if (nvs_get_u32(nvs, key, &value) == ESP_OK) config->local_port = (uint16_t)value;
    serial_config_key(key, sizeof(key), port, "protocol");
    len = sizeof(text);
    if (nvs_get_str(nvs, key, text, &len) == ESP_OK) {
        (void)sx_serial_protocol_from_name(text, &config->tcp_mode);
    } else {
        serial_config_key(key, sizeof(key), port, "mode");
        len = sizeof(text);
        if (nvs_get_str(nvs, key, text, &len) == ESP_OK)
            (void)sx_serial_protocol_from_name(text, &config->tcp_mode);
    }
    serial_config_key(key, sizeof(key), port, "rip");
    len = sizeof(config->remote_ip);
    (void)nvs_get_str(nvs, key, config->remote_ip, &len);
    serial_config_key(key, sizeof(key), port, "rport");
    if (nvs_get_u32(nvs, key, &value) == ESP_OK) config->remote_port = (uint16_t)value;
#define LOAD_MQTT_STRING(suffix, field) do { \
        serial_config_key(key, sizeof(key), port, suffix); \
        len = sizeof(config->field); \
        (void)nvs_get_str(nvs, key, config->field, &len); \
    } while (0)
    LOAD_MQTT_STRING("mquri", mqtt_uri);
    LOAD_MQTT_STRING("mqusr", mqtt_username);
    LOAD_MQTT_STRING("mqpwd", mqtt_password);
    LOAD_MQTT_STRING("mqcid", mqtt_client_id);
    LOAD_MQTT_STRING("mqpub", mqtt_publish_topic);
    LOAD_MQTT_STRING("mqsub", mqtt_subscribe_topic);
#undef LOAD_MQTT_STRING
    serial_config_key(key, sizeof(key), port, "mqqos");
    if (nvs_get_u8(nvs, key, &u8) == ESP_OK) config->mqtt_qos = u8;
    serial_config_key(key, sizeof(key), port, "mqret");
    if (nvs_get_u8(nvs, key, &u8) == ESP_OK) config->mqtt_retain = u8 != 0;
    const char *names[] = {"baud","data","parity","stop","ftime","flen","timeout"};
    int *values[] = {&config->baud_rate,&config->data_bit,&config->check_bit,&config->stop_bit,&config->frame_time,&config->frame_len,&config->timeout};
    for (size_t i = 0; i < 7; ++i) {
        serial_config_key(key, sizeof(key), port, names[i]);
        len = sizeof(text);
        if (nvs_get_str(nvs, key, text, &len) == ESP_OK)
            *values[i] = (i == 3 && strcmp(text, "1.5") == 0) ? 15 : atoi(text);
    }
    nvs_close(nvs);
    return ESP_OK;
}

esp_err_t sx_serial_server_save_config(const sx_serial_port_config_t *config)
{
    if (!config || config->port < 1 || config->port > SERIAL_PORT_COUNT ||
        config->local_port < 1 || config->remote_port < 1 || config->baud_rate < 1200 ||
        config->data_bit < 5 || config->data_bit > 8 || config->check_bit < 0 ||
        config->check_bit > 2 || (config->stop_bit != 1 && config->stop_bit != 2 && config->stop_bit != 15) ||
        config->frame_time < 0 || config->frame_len < 1 ||
        config->frame_len > ASYNC_UART_BUF_SIZE || config->timeout < 1 ||
        config->tcp_mode < SX_SERIAL_TCP_SERVER || config->tcp_mode > SX_SERIAL_MODBUS_TCP_RTU)
        return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_TCP_CLIENT && config->remote_ip[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_MQTT &&
        ((strncmp(config->mqtt_uri, "mqtt://", 7) != 0 &&
          strncmp(config->mqtt_uri, "mqtts://", 8) != 0) ||
         config->mqtt_publish_topic[0] == '\0' || config->mqtt_subscribe_topic[0] == '\0' ||
         config->mqtt_qos > 2)) return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_TCP_SERVER ||
        config->tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) {
        for (int other = 1; other <= SERIAL_PORT_COUNT; ++other) {
            if (other == config->port) continue;
            sx_serial_port_config_t existing;
            if (sx_serial_server_get_config(other, &existing) == ESP_OK &&
                (existing.tcp_mode == SX_SERIAL_TCP_SERVER ||
                 existing.tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) &&
                existing.local_port == config->local_port) return ESP_ERR_INVALID_ARG;
        }
    }
    nvs_handle_t nvs;
    ESP_RETURN_ON_ERROR(nvs_open("storage", NVS_READWRITE, &nvs), TAG, "open NVS failed");
    char key[32], text[24];
    esp_err_t err = ESP_OK;
    serial_config_key(key, sizeof(key), config->port, "port"); err = nvs_set_u32(nvs, key, config->local_port);
    if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, "mode"); err = nvs_set_str(nvs, key, config->tcp_mode == SX_SERIAL_TCP_CLIENT ? "client" : "server"); }
    if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, "protocol"); err = nvs_set_str(nvs, key, sx_serial_protocol_name(config->tcp_mode)); }
    if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, "rip"); err = nvs_set_str(nvs, key, config->remote_ip); }
    if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, "rport"); err = nvs_set_u32(nvs, key, config->remote_port); }
#define SAVE_MQTT_STRING(suffix, field) do { \
        if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, suffix); err = nvs_set_str(nvs, key, config->field); } \
    } while (0)
    SAVE_MQTT_STRING("mquri", mqtt_uri);
    SAVE_MQTT_STRING("mqusr", mqtt_username);
    SAVE_MQTT_STRING("mqpwd", mqtt_password);
    SAVE_MQTT_STRING("mqcid", mqtt_client_id);
    SAVE_MQTT_STRING("mqpub", mqtt_publish_topic);
    SAVE_MQTT_STRING("mqsub", mqtt_subscribe_topic);
#undef SAVE_MQTT_STRING
    if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, "mqqos"); err = nvs_set_u8(nvs, key, config->mqtt_qos); }
    if (err == ESP_OK) { serial_config_key(key, sizeof(key), config->port, "mqret"); err = nvs_set_u8(nvs, key, config->mqtt_retain ? 1 : 0); }
    const char *names[] = {"baud","data","parity","stop","ftime","flen","timeout"};
    int values[] = {config->baud_rate,config->data_bit,config->check_bit,config->stop_bit,config->frame_time,config->frame_len,config->timeout};
    for (size_t i = 0; err == ESP_OK && i < 7; ++i) { serial_config_key(key, sizeof(key), config->port, names[i]); snprintf(text, sizeof(text), "%d", values[i]); err = nvs_set_str(nvs, key, text); }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        load_serial_server_ports();
    }
    return err;
}
