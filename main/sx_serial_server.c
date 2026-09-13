#include "sx_serial_server.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "async_uart.h"
#include "app_task_utils.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sx_serial_port_manager.h"

#define SERIAL_CHANNEL_COUNT 3
#define SERIAL_SERVER_DEFAULT_PORT 8888

static const char *TAG = "serial_server";
static TaskHandle_t s_serial_server_task;
static TaskHandle_t s_channel_tasks[SERIAL_CHANNEL_COUNT + 1];
static volatile bool s_serial_server_running;
static volatile int s_listen_fds[SERIAL_CHANNEL_COUNT + 1] = {-1, -1, -1, -1};
static volatile int s_client_fds[SERIAL_CHANNEL_COUNT + 1] = {-1, -1, -1, -1};
static uint16_t s_tcp_ports[SERIAL_CHANNEL_COUNT + 1] = {0, 8888, 8889, 8890};

static void close_socket(volatile int *fd)
{
    int value = *fd;
    if (value >= 0) {
        shutdown(value, SHUT_RDWR);
        close(value);
        *fd = -1;
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
    for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel)
        s_tcp_ports[channel] = SERIAL_SERVER_DEFAULT_PORT + channel - 1;

    nvs_handle_t nvs = 0;
    if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) return;
    for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
        char key[16];
        snprintf(key, sizeof(key), "ss_ch%d_port", channel);
        if (!load_port_value(nvs, key, &s_tcp_ports[channel]) && channel == 1)
            load_port_value(nvs, "tcp_port", &s_tcp_ports[channel]);
    }
    nvs_close(nvs);
}

esp_err_t sx_serial_server_init(void)
{
    load_serial_server_ports();
    return ESP_OK;
}

int sx_serial_server_get_port(int channel)
{
    if (channel < 1 || channel > SERIAL_CHANNEL_COUNT) return -1;
    load_serial_server_ports();
    return s_tcp_ports[channel];
}

bool sx_serial_server_is_listening(int channel)
{
    return channel >= 1 && channel <= SERIAL_CHANNEL_COUNT &&
           s_serial_server_running && s_listen_fds[channel] >= 0;
}

bool sx_serial_server_client_connected(int channel)
{
    return channel >= 1 && channel <= SERIAL_CHANNEL_COUNT &&
           s_serial_server_running && s_client_fds[channel] >= 0;
}

esp_err_t sx_serial_server_save_ports(const uint16_t ports[3])
{
    if (ports == NULL) return ESP_ERR_INVALID_ARG;
    for (int i = 0; i < SERIAL_CHANNEL_COUNT; ++i) {
        if (ports[i] == 0) return ESP_ERR_INVALID_ARG;
        for (int j = i + 1; j < SERIAL_CHANNEL_COUNT; ++j) {
            if (ports[i] == ports[j]) return ESP_ERR_INVALID_ARG;
        }
    }

    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open("storage", NVS_READWRITE, &nvs),
                        TAG, "open NVS failed");
    esp_err_t err = ESP_OK;
    for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
        char key[16];
        snprintf(key, sizeof(key), "ss_ch%d_port", channel);
        err = nvs_set_u32(nvs, key, ports[channel - 1]);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static int create_listener(int channel)
{
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (fd < 0) return -1;
    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(s_tcp_ports[channel]),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(fd, 1) < 0) {
        ESP_LOGE(TAG, "%s TCP listen port %u failed: errno=%d",
                 sx_serial_port_manager_channel_label(channel),
                 s_tcp_ports[channel], errno);
        close(fd);
        return -1;
    }
    ESP_LOGI(TAG, "%s serial server listening on TCP %u",
             sx_serial_port_manager_channel_label(channel),
             s_tcp_ports[channel]);
    return fd;
}

void sx_serial_server_handle_uart_data(int channel,
                                       const uint8_t *data,
                                       size_t len)
{
    if (channel < 1 || channel > SERIAL_CHANNEL_COUNT ||
        data == NULL || len == 0 || !s_serial_server_running) {
        return;
    }
    int client = s_client_fds[channel];
    if (client < 0) return;
    int sent = send(client, data, len, MSG_DONTWAIT);
    if (sent < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
        ESP_LOGW(TAG, "%s TCP client send failed: errno=%d",
                 sx_serial_port_manager_channel_label(channel), errno);
    }
}

static void close_all_serial_sockets(void)
{
    for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
        close_socket(&s_client_fds[channel]);
        close_socket(&s_listen_fds[channel]);
    }
}

static bool open_available_listeners(void)
{
    bool opened = false;
    for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
        if (!sx_serial_port_manager_channel_available(channel)) {
            ESP_LOGW(TAG, "%s serial server disabled: port unavailable",
                     sx_serial_port_manager_channel_label(channel));
            continue;
        }
        s_listen_fds[channel] = create_listener(channel);
        if (s_listen_fds[channel] >= 0) opened = true;
    }
    return opened;
}

static void serial_channel_task(void *arg)
{
    int channel = (int)(intptr_t)arg;
    uint8_t buffer[512];
    while (s_serial_server_running) {
        sx_serial_channel_config_t cfg;
        sx_serial_server_get_config(channel, &cfg);
        int endpoint = -1;
        if (cfg.tcp_mode == SX_SERIAL_TCP_SERVER) {
            s_listen_fds[channel] = create_listener(channel);
            if (s_listen_fds[channel] < 0) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            endpoint = accept(s_listen_fds[channel], NULL, NULL);
            if (endpoint >= 0) { s_client_fds[channel] = endpoint; }
        } else {
            struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(cfg.remote_port) };
            if (!inet_aton(cfg.remote_ip, &addr.sin_addr)) { vTaskDelay(pdMS_TO_TICKS(2000)); continue; }
            endpoint = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
            if (endpoint < 0 || connect(endpoint, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
                if (endpoint >= 0) {
                    close(endpoint);
                }
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }
            s_client_fds[channel] = endpoint;
        }
        while (s_serial_server_running && endpoint >= 0) {
            fd_set rfds; FD_ZERO(&rfds); FD_SET(endpoint, &rfds);
            struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
            int ready = select(endpoint + 1, &rfds, NULL, NULL, &tv);
            if (ready > 0 && FD_ISSET(endpoint, &rfds)) {
                int received = recv(endpoint, buffer, sizeof(buffer), 0);
                if (received <= 0) break;
                send_data_to_channel(channel, (char *)buffer, (size_t)received);
            }
        }
        close_socket(&s_client_fds[channel]); close_socket(&s_listen_fds[channel]);
        if (s_serial_server_running) vTaskDelay(pdMS_TO_TICKS(500));
    }
    s_channel_tasks[channel] = NULL;
    delete_self_app_task_with_caps();
}

static void serial_server_task(void *arg)
{
    (void)arg;
    uint8_t buffer[512];
    load_serial_server_ports();

    while (s_serial_server_running) {
        if (!open_available_listeners()) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        while (s_serial_server_running) {
            fd_set readfds;
            FD_ZERO(&readfds);
            int maxfd = -1;
            for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
                int listener = s_listen_fds[channel];
                int client = s_client_fds[channel];
                if (listener >= 0) {
                    FD_SET(listener, &readfds);
                    if (listener > maxfd) maxfd = listener;
                }
                if (client >= 0) {
                    FD_SET(client, &readfds);
                    if (client > maxfd) maxfd = client;
                }
            }
            if (maxfd < 0) break;

            struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
            int ready = select(maxfd + 1, &readfds, NULL, NULL, &timeout);
            if (ready < 0) {
                if (errno == EINTR) continue;
                ESP_LOGW(TAG, "serial server select failed: errno=%d", errno);
                break;
            }

            for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
                int listener = s_listen_fds[channel];
                if (listener >= 0 && FD_ISSET(listener, &readfds)) {
                    int accepted = accept(listener, NULL, NULL);
                    if (accepted >= 0) {
                        close_socket(&s_client_fds[channel]);
                        s_client_fds[channel] = accepted;
                        ESP_LOGI(TAG, "%s TCP client connected",
                                 sx_serial_port_manager_channel_label(channel));
                    }
                }

                int client = s_client_fds[channel];
                if (client >= 0 && FD_ISSET(client, &readfds)) {
                    int received = recv(client, buffer, sizeof(buffer), 0);
                    if (received > 0) {
                        send_data_to_channel(channel, (char *)buffer,
                                             (size_t)received);
                    } else {
                        close_socket(&s_client_fds[channel]);
                        ESP_LOGI(TAG, "%s TCP client disconnected",
                                 sx_serial_port_manager_channel_label(channel));
                    }
                }
            }
        }
        close_all_serial_sockets();
        if (s_serial_server_running) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_serial_server_task = NULL;
    delete_self_app_task_with_caps();
}

esp_err_t sx_serial_server_start(void)
{
    for (int i = 1; i <= SERIAL_CHANNEL_COUNT; ++i) if (s_channel_tasks[i] != NULL) return ESP_OK;
    s_serial_server_running = true;
    for (int channel = 1; channel <= SERIAL_CHANNEL_COUNT; ++channel) {
        if (!sx_serial_port_manager_channel_available(channel)) continue;
        char name[20]; snprintf(name, sizeof(name), "serial_ch%d", channel);
        if (create_app_task_psram(serial_channel_task, name, 4096, (void *)(intptr_t)channel, 6,
                                  &s_channel_tasks[channel], tskNO_AFFINITY) != pdPASS) {
            s_serial_server_running = false; close_all_serial_sockets(); return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "SP603 per-channel serial server tasks started");
    return ESP_OK;
}

esp_err_t sx_serial_server_stop(void)
{
    s_serial_server_running = false;
    close_all_serial_sockets();
    for (int i = 0; i < 100; ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
        bool running = false; for (int ch = 1; ch <= SERIAL_CHANNEL_COUNT; ++ch) running |= s_channel_tasks[ch] != NULL;
        if (!running) break;
    }
    ESP_LOGI(TAG, "serial_server mode stopped");
    for (int ch = 1; ch <= SERIAL_CHANNEL_COUNT; ++ch) if (s_channel_tasks[ch] != NULL) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

esp_err_t sx_serial_server_get_config(int channel, sx_serial_channel_config_t *config)
{
    if (!config || channel < 1 || channel > SERIAL_CHANNEL_COUNT) return ESP_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    config->channel = channel; config->tcp_mode = SX_SERIAL_TCP_SERVER;
    config->local_port = SERIAL_SERVER_DEFAULT_PORT + channel - 1;
    config->remote_port = config->local_port; config->baud_rate = 9600;
    config->data_bit = 8; config->check_bit = 0; config->stop_bit = 1;
    config->frame_time = 50; config->frame_len = 512; config->timeout = 500;
    nvs_handle_t nvs; if (nvs_open("storage", NVS_READONLY, &nvs) != ESP_OK) return ESP_OK;
    char key[32], text[80]; size_t len; uint32_t value;
    snprintf(key, sizeof(key), "ss_ch%d_port", channel); if (nvs_get_u32(nvs, key, &value) == ESP_OK) config->local_port = value;
    snprintf(key, sizeof(key), "ch%d_tcp_mode", channel); len = sizeof(text); if (nvs_get_str(nvs, key, text, &len) == ESP_OK && strcmp(text, "client") == 0) config->tcp_mode = SX_SERIAL_TCP_CLIENT;
    snprintf(key, sizeof(key), "ch%d_remote_ip", channel); len = sizeof(config->remote_ip); nvs_get_str(nvs, key, config->remote_ip, &len);
    snprintf(key, sizeof(key), "ch%d_remote_port", channel); if (nvs_get_u32(nvs, key, &value) == ESP_OK) config->remote_port = value;
    const char *names[] = {"baud_rate","data_bit","check_bit","stop_bit","frame_time","frame_len","timeout"};
    int *values[] = {&config->baud_rate,&config->data_bit,&config->check_bit,&config->stop_bit,&config->frame_time,&config->frame_len,&config->timeout};
    for (size_t i = 0; i < 7; ++i) { snprintf(key, sizeof(key), "ch%d_%s", channel, names[i]); len = sizeof(text); if (nvs_get_str(nvs, key, text, &len) == ESP_OK) *values[i] = atoi(text); }
    nvs_close(nvs); return ESP_OK;
}

esp_err_t sx_serial_server_save_config(const sx_serial_channel_config_t *config)
{
    if (!config || config->channel < 1 || config->channel > SERIAL_CHANNEL_COUNT || config->local_port < 1 || config->remote_port < 1 || config->baud_rate < 1200 || config->data_bit < 5 || config->data_bit > 8 || config->check_bit < 0 || config->check_bit > 2 || config->stop_bit < 1 || config->stop_bit > 2 || config->frame_time < 0 || config->frame_len < 1 || config->frame_len > ASYNC_UART_BUF_SIZE || config->timeout < 1) return ESP_ERR_INVALID_ARG;
    if (config->tcp_mode == SX_SERIAL_TCP_CLIENT && config->remote_ip[0] == '\0') return ESP_ERR_INVALID_ARG;
    for (int other = 1; other <= SERIAL_CHANNEL_COUNT; ++other) {
        if (other == config->channel) continue;
        sx_serial_channel_config_t existing;
        if (sx_serial_server_get_config(other, &existing) == ESP_OK && existing.local_port == config->local_port) return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs; ESP_RETURN_ON_ERROR(nvs_open("storage", NVS_READWRITE, &nvs), TAG, "open NVS failed");
    char key[32], text[24]; esp_err_t err = ESP_OK;
    snprintf(key, sizeof(key), "ss_ch%d_port", config->channel); err = nvs_set_u32(nvs, key, config->local_port);
    if (err == ESP_OK) { snprintf(key, sizeof(key), "ch%d_tcp_mode", config->channel); err = nvs_set_str(nvs, key, config->tcp_mode == SX_SERIAL_TCP_CLIENT ? "client" : "server"); }
    if (err == ESP_OK) { snprintf(key, sizeof(key), "ch%d_remote_ip", config->channel); err = nvs_set_str(nvs, key, config->remote_ip); }
    if (err == ESP_OK) { snprintf(key, sizeof(key), "ch%d_remote_port", config->channel); err = nvs_set_u32(nvs, key, config->remote_port); }
    const char *names[] = {"baud_rate","data_bit","check_bit","stop_bit","frame_time","frame_len","timeout"};
    int values[] = {config->baud_rate,config->data_bit,config->check_bit,config->stop_bit,config->frame_time,config->frame_len,config->timeout};
    for (size_t i = 0; err == ESP_OK && i < 7; ++i) { snprintf(key, sizeof(key), "ch%d_%s", config->channel, names[i]); snprintf(text, sizeof(text), "%d", values[i]); err = nvs_set_str(nvs, key, text); }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        load_serial_server_ports();
    }
    return err;
}
