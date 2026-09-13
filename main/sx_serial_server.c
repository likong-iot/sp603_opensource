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
    if (s_serial_server_task != NULL) return ESP_OK;
    s_serial_server_running = true;
    if (create_app_task_psram(serial_server_task, "serial_server", 6144,
                              NULL, 6, &s_serial_server_task,
                              tskNO_AFFINITY) != pdPASS) {
        s_serial_server_running = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "SP603 multi-port serial_server mode started");
    return ESP_OK;
}

esp_err_t sx_serial_server_stop(void)
{
    s_serial_server_running = false;
    close_all_serial_sockets();
    for (int i = 0; i < 100 && s_serial_server_task != NULL; ++i)
        vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "serial_server mode stopped");
    return s_serial_server_task == NULL ? ESP_OK : ESP_ERR_TIMEOUT;
}

