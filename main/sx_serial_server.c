#include "sx_serial_server.h"

#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "async_uart.h"
#include "app_task_utils.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_tls_errors.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "sx_serial_port_manager.h"
#include "sx_network_manager.h"
#include "sx_web_server.h"

#define SERIAL_PORT_COUNT 3
#define SERIAL_SERVER_DEFAULT_PORT 8888
#define MODBUS_TCP_MAX_ADU 260
#define MODBUS_RTU_MAX_ADU 256

static const char *TAG = "serial_server";
static atomic_bool s_port_task_active[SERIAL_PORT_COUNT + 1];
static atomic_bool s_serial_server_running;
static SemaphoreHandle_t s_lifecycle_mutex;
static SemaphoreHandle_t s_config_mutex;
static atomic_int s_listen_fds[SERIAL_PORT_COUNT + 1] = {-1, -1, -1, -1};
static atomic_int s_client_fds[SERIAL_PORT_COUNT + 1] = {-1, -1, -1, -1};
static uint16_t s_tcp_ports[SERIAL_PORT_COUNT + 1];
static atomic_bool s_mqtt_connected[SERIAL_PORT_COUNT + 1];
static atomic_bool s_mqtt_error[SERIAL_PORT_COUNT + 1];
static sx_serial_port_config_t s_runtime_configs[SERIAL_PORT_COUNT + 1];
static uint8_t s_mqtt_rx_buffers[SERIAL_PORT_COUNT + 1][512];
static size_t s_mqtt_rx_lengths[SERIAL_PORT_COUNT + 1];
static size_t s_mqtt_rx_totals[SERIAL_PORT_COUNT + 1];

#define SERIAL_TX_QUEUE_DEPTH 8
#define SERIAL_TX_TIMEOUT_US (5LL * 1000000LL)
typedef struct {
    size_t len;
    size_t offset;
    int64_t deadline_us;
    uint8_t data[];
} serial_tx_frame_t;

/* Port tasks alone use and close TCP/MQTT handles. UART only copies into a
 * bounded queue; these locks/queues stay allocated across mode restarts. */
static QueueHandle_t s_tx_queues[SERIAL_PORT_COUNT + 1];
static SemaphoreHandle_t s_tx_locks[SERIAL_PORT_COUNT + 1];
static bool s_tx_accepting[SERIAL_PORT_COUNT + 1];
static atomic_bool s_tx_failed[SERIAL_PORT_COUNT + 1];
static uint32_t s_tx_generation[SERIAL_PORT_COUNT + 1];

static void set_tx_active(int port, bool active)
{
    xSemaphoreTake(s_tx_locks[port], portMAX_DELAY);
    s_tx_accepting[port] = active && atomic_load(&s_serial_server_running);
    ++s_tx_generation[port];
    serial_tx_frame_t *frame;
    while (xQueueReceive(s_tx_queues[port], &frame, 0) == pdTRUE) free(frame);
    s_tx_failed[port] = false;
    xSemaphoreGive(s_tx_locks[port]);
}

static bool queue_network_tx(int port, const uint8_t *data, size_t len,
                             uint32_t generation)
{
    if (len == 0 || len > ASYNC_UART_BUF_SIZE ||
        xSemaphoreTake(s_tx_locks[port], pdMS_TO_TICKS(10)) != pdTRUE) {
        atomic_store(&s_tx_failed[port], true);
        return false;
    }
    bool accepted = false;
    if (s_tx_accepting[port] && atomic_load(&s_serial_server_running) &&
        (generation == 0 || generation == s_tx_generation[port])) {
        serial_tx_frame_t *frame = NULL;
        if (uxQueueSpacesAvailable(s_tx_queues[port]) > 0)
            frame = heap_caps_malloc(sizeof(*frame) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (frame != NULL) {
            frame->len = len;
            frame->offset = 0;
            frame->deadline_us = esp_timer_get_time() + SERIAL_TX_TIMEOUT_US;
            memcpy(frame->data, data, len);
            accepted = xQueueSend(s_tx_queues[port], &frame, 0) == pdTRUE;
            if (!accepted) free(frame);
        }
        if (!accepted) s_tx_failed[port] = true;
    }
    xSemaphoreGive(s_tx_locks[port]);
    return accepted;
}

static bool network_tx_failed(int port)
{
    xSemaphoreTake(s_tx_locks[port], portMAX_DELAY);
    bool failed = s_tx_failed[port];
    xSemaphoreGive(s_tx_locks[port]);
    return failed;
}

/* Keep the unsent suffix on short writes or temporary socket backpressure. */
static int flush_tcp_frame(int fd, serial_tx_frame_t *frame)
{
    if (esp_timer_get_time() >= frame->deadline_us) return -1;
    int sent = send(fd, frame->data + frame->offset,
                    frame->len - frame->offset, MSG_DONTWAIT);
    if (sent > 0) {
        frame->offset += (size_t)sent;
        return frame->offset == frame->len ? 1 : 0;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
    return -1;
}

static void serial_config_key(char *key, size_t key_size, int port,
                              const char *field)
{
    snprintf(key, key_size, "%s_%s", sx_serial_port_manager_port_key(port), field);
}

static uint16_t default_server_port(int port)
{
    if (sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422) {
        return (uint16_t)(SERIAL_SERVER_DEFAULT_PORT + port - 1);
    }
    if (port == 3) return SERIAL_SERVER_DEFAULT_PORT;
    if (port == 1) return SERIAL_SERVER_DEFAULT_PORT + 1;
    return SERIAL_SERVER_DEFAULT_PORT + 2;
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

static void close_socket(atomic_int *fd)
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

/* Accept only contiguous fragments from one bounded MQTT message. */
static size_t append_mqtt_fragment(int port, const char *data, int len, int total, int offset)
{
    if (data == NULL || total <= 0 || total > (int)sizeof(s_mqtt_rx_buffers[port]) ||
        offset < 0 || len <= 0 || offset > total || len > total - offset) goto invalid;
    if (offset == 0) {
        s_mqtt_rx_lengths[port] = 0;
        s_mqtt_rx_totals[port] = (size_t)total;
    }
    if ((size_t)total != s_mqtt_rx_totals[port] ||
        (size_t)offset != s_mqtt_rx_lengths[port]) goto invalid;
    memcpy(s_mqtt_rx_buffers[port] + offset, data, (size_t)len);
    s_mqtt_rx_lengths[port] += (size_t)len;
    if (s_mqtt_rx_lengths[port] == (size_t)total) {
        s_mqtt_rx_lengths[port] = 0;
        s_mqtt_rx_totals[port] = 0;
        return (size_t)total;
    }
    return 0;
invalid:
    s_mqtt_rx_lengths[port] = 0;
    s_mqtt_rx_totals[port] = 0;
    return 0;
}

static void mqtt_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    int port = (int)(intptr_t)arg;
    if (port < 1 || port > SERIAL_PORT_COUNT || event_data == NULL) return;
    esp_mqtt_event_handle_t event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        s_mqtt_rx_lengths[port] = 0;
        s_mqtt_rx_totals[port] = 0;
        set_tx_active(port, true);
        s_mqtt_connected[port] = true;
        s_mqtt_error[port] = false;
        const sx_serial_port_config_t *cfg = &s_runtime_configs[port];
        if (cfg->mqtt_subscribe_topic[0] != '\0' &&
            esp_mqtt_client_subscribe(event->client, cfg->mqtt_subscribe_topic, cfg->mqtt_qos) < 0) {
            atomic_store(&s_tx_failed[port], true);
            s_mqtt_error[port] = true;
        }
        ESP_LOGI(TAG, "%s MQTT connected",
                 sx_serial_port_manager_port_label(port));
        char connected_text[96];
        snprintf(connected_text, sizeof(connected_text), "%s MQTT connected",
                 sx_serial_port_manager_port_label(port));
        send_system_log("INFO", "mqtt", connected_text);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        set_tx_active(port, false);
        s_mqtt_rx_lengths[port] = 0;
        s_mqtt_rx_totals[port] = 0;
        s_mqtt_connected[port] = false;
        ESP_LOGW(TAG, "%s MQTT disconnected", sx_serial_port_manager_port_label(port));
        char disconnect_text[96];
        snprintf(disconnect_text, sizeof(disconnect_text), "%s MQTT disconnected",
                 sx_serial_port_manager_port_label(port));
        send_system_log("WARN", "mqtt", disconnect_text);
    } else if (event_id == MQTT_EVENT_ERROR) {
        set_tx_active(port, false);
        s_mqtt_rx_lengths[port] = 0;
        s_mqtt_rx_totals[port] = 0;
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
            if (error->esp_tls_last_esp_err == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOSTNAME) {
                snprintf(error_text, sizeof(error_text),
                         "%s MQTT domain resolution failed: %s",
                         sx_serial_port_manager_port_label(port),
                         s_runtime_configs[port].mqtt_uri);
            } else if (error->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
                snprintf(error_text, sizeof(error_text),
                         "%s MQTT broker refused connection (code=%d)",
                         sx_serial_port_manager_port_label(port),
                         error->connect_return_code);
            } else if (error->esp_transport_sock_errno > 0) {
                snprintf(error_text, sizeof(error_text),
                         "%s MQTT transport failed: tls=0x%x errno=%d (%s)",
                         sx_serial_port_manager_port_label(port),
                         (unsigned)error->esp_tls_last_esp_err,
                         error->esp_transport_sock_errno,
                         strerror(error->esp_transport_sock_errno));
            } else {
                snprintf(error_text, sizeof(error_text),
                         "%s MQTT transport failed: type=%d tls=0x%x",
                         sx_serial_port_manager_port_label(port), error->error_type,
                         (unsigned)error->esp_tls_last_esp_err);
            }
            send_system_log("ERROR", "mqtt", error_text);
        }
    } else if (event_id == MQTT_EVENT_DATA) {
        size_t length = append_mqtt_fragment(port, event->data, event->data_len,
                                             event->total_data_len, event->current_data_offset);
        if (length > 0 && atomic_load(&s_serial_server_running))
            send_data_to_port(port, (char *)s_mqtt_rx_buffers[port], length);
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
        s_tcp_ports[port] = default_server_port(port);

    nvs_handle_t nvs = 0;
    if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) return;
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        char key[16];
        serial_config_key(key, sizeof(key), port, "port");
        const int primary_port = sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422 ? 1 : 3;
        if (!load_port_value(nvs, key, &s_tcp_ports[port]) && port == primary_port)
            load_port_value(nvs, "tcp_port", &s_tcp_ports[port]);
    }
    nvs_close(nvs);
}

esp_err_t sx_serial_server_init(void)
{
    if (s_lifecycle_mutex != NULL) return ESP_OK;
    if (s_config_mutex == NULL) s_config_mutex = xSemaphoreCreateMutex();
    if (s_config_mutex == NULL) return ESP_ERR_NO_MEM;
    load_serial_server_ports();
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        if (s_modbus_states[port].lock == NULL) {
            s_modbus_states[port].lock = xSemaphoreCreateMutex();
            if (s_modbus_states[port].lock == NULL) return ESP_ERR_NO_MEM;
        }
        s_modbus_states[port].socket_fd = -1;
        if (s_tx_locks[port] == NULL) s_tx_locks[port] = xSemaphoreCreateMutex();
        if (s_tx_queues[port] == NULL)
            s_tx_queues[port] = xQueueCreate(SERIAL_TX_QUEUE_DEPTH, sizeof(serial_tx_frame_t *));
        if (s_tx_locks[port] == NULL || s_tx_queues[port] == NULL) return ESP_ERR_NO_MEM;
    }
    s_lifecycle_mutex = xSemaphoreCreateMutex();
    return s_lifecycle_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

int sx_serial_server_get_port(int port)
{
    if (port < 1 || port > SERIAL_PORT_COUNT) return -1;
    if (s_config_mutex != NULL) xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    load_serial_server_ports();
    int value = s_tcp_ports[port];
    if (s_config_mutex != NULL) xSemaphoreGive(s_config_mutex);
    return value;
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

static esp_err_t save_ports_locked(const uint16_t ports[3])
{
    if (ports == NULL) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < SERIAL_PORT_COUNT; ++i) {
        if (!sx_serial_port_manager_port_available(i + 1)) continue;
        if (ports[i] == 0) return ESP_ERR_INVALID_ARG;
        for (int j = i + 1; j < SERIAL_PORT_COUNT; ++j) {
            if (sx_serial_port_manager_port_available(j + 1) && ports[i] == ports[j]) return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open("storage", NVS_READWRITE, &nvs),
                        TAG, "open NVS failed");
    esp_err_t err = ESP_OK;
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        if (!sx_serial_port_manager_port_available(port)) continue;
        char key[16];
        serial_config_key(key, sizeof(key), port, "port");
        err = nvs_set_u32(nvs, key, ports[port - 1]);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

esp_err_t sx_serial_server_save_ports(const uint16_t ports[3])
{
    if (s_config_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    esp_err_t err = save_ports_locked(ports);
    xSemaphoreGive(s_config_mutex);
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
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "%s serial server listening on TCP %u",
             sx_serial_port_manager_port_label(serial_port),
             tcp_port);
    return fd;
}

void sx_serial_server_handle_uart_data(int port, const uint8_t *data, size_t len)
{
    if (port < 1 || port > SERIAL_PORT_COUNT || data == NULL || len == 0 ||
        !atomic_load(&s_serial_server_running) || s_tx_locks[port] == NULL) return;
    if (xSemaphoreTake(s_tx_locks[port], pdMS_TO_TICKS(10)) != pdTRUE) {
        atomic_store(&s_tx_failed[port], true);
        return;
    }
    bool active = s_tx_accepting[port];
    uint32_t generation = s_tx_generation[port];
    sx_serial_tcp_mode_t mode = s_runtime_configs[port].tcp_mode;
    xSemaphoreGive(s_tx_locks[port]);
    if (!active) return;
    if (mode == SX_SERIAL_MODBUS_TCP_RTU) {
        modbus_bridge_state_t *state = &s_modbus_states[port];
        if (state->lock == NULL || len < 5 ||
            xSemaphoreTake(state->lock, pdMS_TO_TICKS(50)) != pdTRUE) return;
        if (state->waiting && state->socket_fd >= 0) {
            uint8_t response[MODBUS_TCP_MAX_ADU];
            size_t response_len = 0;
            if (modbus_rtu_adu_to_tcp(data, len, state->transaction_id,
                                      state->unit_id, state->function_code,
                                      response, sizeof(response), &response_len) &&
                queue_network_tx(port, response, response_len, generation))
                state->waiting = false;
        }
        xSemaphoreGive(state->lock);
        return;
    }
    (void)queue_network_tx(port, data, len, generation);
}

static void send_modbus_exception(int port, uint16_t transaction_id, uint8_t unit_id,
                                  uint8_t function_code, uint8_t exception_code)
{
    uint8_t response[9] = {
        (uint8_t)(transaction_id >> 8U), (uint8_t)transaction_id,
        0, 0, 0, 3, unit_id, (uint8_t)(function_code | 0x80U), exception_code,
    };
    (void)queue_network_tx(port, response, sizeof(response), 0);
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
        send_modbus_exception(port, transaction_id, unit_id, function_code, 0x06);
        return;
    }
    if (state->waiting) {
        xSemaphoreGive(state->lock);
        send_modbus_exception(port, transaction_id, unit_id, function_code, 0x06);
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
        send_modbus_exception(port, state->transaction_id, state->unit_id,
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
    const sx_serial_port_config_t *runtime = cfg;
    bool waiting_logged = false;
    while (s_serial_server_running && !config_changed(port, runtime)) {
        sx_network_status_t network = {0};
        sx_network_manager_get_status(&network);
        if (network.active_interface != SX_NETWORK_IF_NONE && network.dns_ready) break;
        if (!waiting_logged) {
            char waiting_text[96];
            snprintf(waiting_text, sizeof(waiting_text),
                     "%s MQTT waiting for a usable uplink IP and DNS",
                     sx_serial_port_manager_port_label(port));
            send_system_log("INFO", "mqtt", waiting_text);
            waiting_logged = true;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (!s_serial_server_running || config_changed(port, runtime)) return;

    sx_network_status_t mqtt_network = {0};
    sx_network_manager_get_status(&mqtt_network);
    const uint32_t mqtt_uplink_generation = mqtt_network.uplink_generation;

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
        .outbox.limit = 16 * 1024,
    };
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    if (client == NULL) {
        s_mqtt_error[port] = true;
        ESP_LOGE(TAG, "%s MQTT client init failed",
                 sx_serial_port_manager_port_label(port));
        send_system_log("ERROR", "mqtt", "MQTT client init failed");
        return;
    }
    s_mqtt_connected[port] = false;
    s_mqtt_error[port] = false;
    esp_err_t mqtt_err = esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID,
                                                        mqtt_event_handler, (void *)(intptr_t)port);
    if (mqtt_err == ESP_OK) mqtt_err = esp_mqtt_client_start(client);
    if (mqtt_err != ESP_OK) {
        s_mqtt_error[port] = true;
        ESP_LOGE(TAG, "%s MQTT client start failed",
                 sx_serial_port_manager_port_label(port));
        send_system_log("ERROR", "mqtt", "MQTT client start failed");
        esp_mqtt_client_destroy(client);
        return;
    }
    while (s_serial_server_running && !config_changed(port, runtime)) {
        sx_network_status_t network = {0};
        sx_network_manager_get_status(&network);
        if (network.active_interface == SX_NETWORK_IF_NONE ||
            !network.dns_ready || network.uplink_generation != mqtt_uplink_generation) {
            ESP_LOGW(TAG, "%s MQTT network changed, rebuilding client",
                     sx_serial_port_manager_port_label(port));
            char network_text[112];
            snprintf(network_text, sizeof(network_text),
                     "%s MQTT network changed; reconnecting",
                     sx_serial_port_manager_port_label(port));
            send_system_log("WARN", "mqtt", network_text);
            break;
        }
        if (network_tx_failed(port)) {
            ESP_LOGW(TAG, "%s MQTT TX queue full; rebuilding client",
                     sx_serial_port_manager_port_label(port));
            break;
        }
        serial_tx_frame_t *frame;
        if (xQueueReceive(s_tx_queues[port], &frame, 0) == pdTRUE) {
            int queued = esp_mqtt_client_enqueue(client, runtime->mqtt_publish_topic,
                                                 (const char *)frame->data, (int)frame->len,
                                                 runtime->mqtt_qos, runtime->mqtt_retain, true);
            free(frame);
            if (queued < 0) {
                ESP_LOGW(TAG, "%s MQTT outbox rejected data; rebuilding client",
                         sx_serial_port_manager_port_label(port));
                break;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    set_tx_active(port, false);
    s_mqtt_connected[port] = false;
    esp_mqtt_client_stop(client);
    esp_mqtt_client_destroy(client);
    set_tx_active(port, false);
    s_mqtt_connected[port] = false;
}

static void serial_port_task(void *arg)
{
    int port = (int)(intptr_t)arg;
    uint8_t buffer[1024];
    while (s_serial_server_running) {
        sx_serial_port_config_t cfg;
        if (sx_serial_server_get_config(port, &cfg) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        set_tx_active(port, false);
        xSemaphoreTake(s_tx_locks[port], portMAX_DELAY);
        s_runtime_configs[port] = cfg;
        xSemaphoreGive(s_tx_locks[port]);
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
                struct timeval accept_timeout = {.tv_sec = 0, .tv_usec = 25000};
                int ready = select(s_listen_fds[port] + 1, &accept_fds, NULL, NULL,
                                   &accept_timeout);
                if (ready > 0) {
                    endpoint = accept(s_listen_fds[port], NULL, NULL);
                    if (endpoint < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
                    break;
                }
                if (ready < 0 && errno != EINTR) break;
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

        if (endpoint >= 0) {
            int flags = fcntl(endpoint, F_GETFL, 0);
            if (flags < 0 || fcntl(endpoint, F_SETFL, flags | O_NONBLOCK) < 0) {
                close_socket(&s_client_fds[port]);
                endpoint = -1;
            } else {
                set_tx_active(port, true);
            }
        }
        size_t buffered = 0;
        serial_tx_frame_t *pending = NULL;
        while (s_serial_server_running && endpoint >= 0 && !config_changed(port, &cfg)) {
            if (network_tx_failed(port)) {
                ESP_LOGW(TAG, "%s TCP TX queue full; closing connection",
                         sx_serial_port_manager_port_label(port));
                break;
            }
            if (pending == NULL) (void)xQueueReceive(s_tx_queues[port], &pending, 0);
            if (pending != NULL && esp_timer_get_time() >= pending->deadline_us) {
                ESP_LOGW(TAG, "%s TCP TX timed out; closing connection",
                         sx_serial_port_manager_port_label(port));
                break;
            }
            fd_set rfds, wfds;
            FD_ZERO(&wfds);
            if (pending != NULL) FD_SET(endpoint, &wfds);
            FD_ZERO(&rfds);
            FD_SET(endpoint, &rfds);
            struct timeval tv = {.tv_sec = 0, .tv_usec = 20000};
            int ready = select(endpoint + 1, &rfds, &wfds, NULL, &tv);
            if (ready < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ready > 0 && pending != NULL && FD_ISSET(endpoint, &wfds)) {
                int result = flush_tcp_frame(endpoint, pending);
                if (result < 0) break;
                if (result > 0) { free(pending); pending = NULL; }
            }
            if (ready > 0 && FD_ISSET(endpoint, &rfds)) {
                int received = recv(endpoint, buffer + buffered, sizeof(buffer) - buffered, 0);
                if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
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
        set_tx_active(port, false);
        free(pending);
        if (cfg.tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) clear_modbus_request(port, endpoint);
        close_socket(&s_client_fds[port]);
        close_socket(&s_listen_fds[port]);
        if (s_serial_server_running) vTaskDelay(pdMS_TO_TICKS(250));
    }
    set_tx_active(port, false);
    atomic_store(&s_port_task_active[port], false);
    delete_self_app_task_with_caps();
}

static esp_err_t stop_serial_tasks_locked(void)
{
    atomic_store(&s_serial_server_running, false);
    /* Never close a descriptor another task is selecting or sending on. Each
     * owner checks running at least every 100 ms outside bounded SDK calls. */
    for (int waited = 0; waited < 15000; waited += 10) {
        bool active = false;
        for (int port = 1; port <= SERIAL_PORT_COUNT; ++port)
            active |= atomic_load(&s_port_task_active[port]);
        if (!active) return ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t sx_serial_server_start(void)
{
    if (s_lifecycle_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    if (atomic_load(&s_serial_server_running)) {
        xSemaphoreGive(s_lifecycle_mutex);
        return ESP_OK;
    }
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        if (atomic_load(&s_port_task_active[port])) {
            xSemaphoreGive(s_lifecycle_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    atomic_store(&s_serial_server_running, true);
    esp_err_t err = ESP_OK;
    for (int port = 1; port <= SERIAL_PORT_COUNT; ++port) {
        if (!sx_serial_port_manager_port_available(port)) continue;
        char name[20]; snprintf(name, sizeof(name), "serial_port%d", port);
        atomic_store(&s_port_task_active[port], true);
        if (create_app_task_psram(serial_port_task, name, 6144, (void *)(intptr_t)port, 6,
                                  NULL, tskNO_AFFINITY) != pdPASS) {
            atomic_store(&s_port_task_active[port], false);
            (void)stop_serial_tasks_locked();
            err = ESP_ERR_NO_MEM;
            break;
        }
    }
    xSemaphoreGive(s_lifecycle_mutex);
    return err;
}

esp_err_t sx_serial_server_stop(void)
{
    if (s_lifecycle_mutex == NULL) return ESP_OK;
    xSemaphoreTake(s_lifecycle_mutex, portMAX_DELAY);
    esp_err_t err = stop_serial_tasks_locked();
    xSemaphoreGive(s_lifecycle_mutex);
    if (err == ESP_OK) ESP_LOGI(TAG, "serial_server mode stopped");
    else ESP_LOGE(TAG, "serial_server stop timed out; task ownership retained");
    return err;
}

static esp_err_t get_config_locked(int port, sx_serial_port_config_t *config)
{
    if (!config || port < 1 || port > SERIAL_PORT_COUNT) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    config->port = port;
    config->tcp_mode = SX_SERIAL_TCP_SERVER;
    config->local_port = default_server_port(port);
    config->remote_port = config->local_port;
    set_serial_mqtt_defaults(port, config);
    config->baud_rate = 9600;
    config->data_bit = 8;
    config->check_bit = 0;
    config->stop_bit = 1;
    config->frame_time = 100;
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

esp_err_t sx_serial_server_get_config(int port, sx_serial_port_config_t *config)
{
    if (s_config_mutex != NULL) xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    esp_err_t err = get_config_locked(port, config);
    if (s_config_mutex != NULL) xSemaphoreGive(s_config_mutex);
    return err;
}

static esp_err_t save_config_locked(const sx_serial_port_config_t *config)
{
    if (!config || config->port < 1 || config->port > SERIAL_PORT_COUNT ||
        !sx_serial_port_manager_port_available(config->port) ||
        config->local_port < 1 || config->remote_port < 1 || config->baud_rate < 1200 ||
        config->data_bit < 5 || config->data_bit > 8 || config->check_bit < 0 ||
        config->check_bit > 2 || (config->stop_bit != 1 && config->stop_bit != 2 && config->stop_bit != 15) ||
        config->frame_time < 0 || config->frame_len < 1 ||
        config->frame_len > ASYNC_UART_BUF_SIZE || config->timeout < 1 ||
        config->tcp_mode < SX_SERIAL_TCP_SERVER || config->tcp_mode > SX_SERIAL_MODBUS_TCP_RTU)
        return ESP_ERR_INVALID_ARG;
#define CHECK_STRING(field) do { \
        if (memchr(config->field, '\0', sizeof(config->field)) == NULL) return ESP_ERR_INVALID_ARG; \
    } while (0)
    CHECK_STRING(remote_ip);
    CHECK_STRING(mqtt_uri);
    CHECK_STRING(mqtt_username);
    CHECK_STRING(mqtt_password);
    CHECK_STRING(mqtt_client_id);
    CHECK_STRING(mqtt_publish_topic);
    CHECK_STRING(mqtt_subscribe_topic);
#undef CHECK_STRING
    struct in_addr remote;
    if (config->tcp_mode == SX_SERIAL_TCP_CLIENT &&
        !inet_aton(config->remote_ip, &remote)) return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_TCP_CLIENT && config->remote_ip[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_MQTT &&
        ((strncmp(config->mqtt_uri, "mqtt://", 7) != 0 &&
          strncmp(config->mqtt_uri, "mqtts://", 8) != 0) ||
         config->mqtt_publish_topic[0] == '\0' || config->mqtt_subscribe_topic[0] == '\0' ||
         config->mqtt_qos > 2)) return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_TCP_SERVER ||
        config->tcp_mode == SX_SERIAL_MODBUS_TCP_RTU) {
        for (int other = 1; other <= SERIAL_PORT_COUNT; ++other) {
            if (other == config->port || !sx_serial_port_manager_port_available(other)) continue;
            sx_serial_port_config_t existing;
            if (get_config_locked(other, &existing) == ESP_OK &&
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
    for (size_t i = 0; err == ESP_OK && i < 7; ++i) {
        serial_config_key(key, sizeof(key), config->port, names[i]);
        if (i == 3 && values[i] == 15) snprintf(text, sizeof(text), "1.5");
        else snprintf(text, sizeof(text), "%d", values[i]);
        err = nvs_set_str(nvs, key, text);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        load_serial_server_ports();
    }
    return err;
}

esp_err_t sx_serial_server_save_config(const sx_serial_port_config_t *config)
{
    if (s_config_mutex == NULL) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_config_mutex, portMAX_DELAY);
    esp_err_t err = save_config_locked(config);
    xSemaphoreGive(s_config_mutex);
    return err;
}
