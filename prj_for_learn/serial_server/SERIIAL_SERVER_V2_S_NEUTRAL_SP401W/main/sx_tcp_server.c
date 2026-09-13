/*
 * @Author: Orion
 * @Date: 2024-01-23 16:08:36
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 19:07:21
 * @FilePath: \SERIIAL_SERVER\main\sx_tcp_server.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "cJSON.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"
#include "sx_modbus.h"
#include "sx_utils.h"
#include <lwip/netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>  // For close() function

#define KEEPALIVE_IDLE 5
#define KEEPALIVE_INTERVAL 5
#define KEEPALIVE_COUNT 3
int global_sock = -1;

int tcp_client_count = 0;

static const char *TAG = "TCPSERVER";
// 最后的Transaction ID
static uint16_t last_transaction_id = 0;
// 添加互斥锁
static SemaphoreHandle_t tcp_send_mutex = NULL;
// 心跳包任务句柄
TaskHandle_t xHandleTaskTCPServerHeartbeat = NULL;
static bool s_tcp_server_modbus_mode = false;

// Socket清理队列
#define CLEANUP_QUEUE_SIZE 10
static QueueHandle_t socket_cleanup_queue = NULL;
static TaskHandle_t socket_cleanup_task_handle = NULL;

typedef struct {
    int socket_fd;
} cleanup_item_t;

// Socket清理任务
static void socket_cleanup_task(void *pvParameters) {
    cleanup_item_t cleanup_item;

    while (1) {
        // 等待需要清理的socket
        if (xQueueReceive(socket_cleanup_queue, &cleanup_item, portMAX_DELAY) == pdTRUE) {
            if (cleanup_item.socket_fd >= 0) {
                ESP_LOGI(TAG, "Cleaning up socket %d", cleanup_item.socket_fd);
                shutdown(cleanup_item.socket_fd, SHUT_RDWR);
                vTaskDelay(pdMS_TO_TICKS(50)); // 给TCP栈时间处理
                close(cleanup_item.socket_fd);
                ESP_LOGI(TAG, "Socket %d cleaned up", cleanup_item.socket_fd);
            }
        }
    }
}

// 安全关闭socket的函数
static void safe_close_socket(int socket_fd) {
    if (socket_fd < 0) {
        return;
    }

    if (socket_cleanup_queue == NULL) {
        // 如果队列不存在，直接关闭
        shutdown(socket_fd, SHUT_RDWR);
        vTaskDelay(pdMS_TO_TICKS(10));
        close(socket_fd);
        return;
    }

    cleanup_item_t cleanup_item = { .socket_fd = socket_fd };

    // 尝试将socket添加到清理队列
    if (xQueueSend(socket_cleanup_queue, &cleanup_item, 0) != pdTRUE) {
        // 如果队列满了，直接关闭
        ESP_LOGW(TAG, "Cleanup queue full, closing socket directly");
        shutdown(socket_fd, SHUT_RDWR);
        vTaskDelay(pdMS_TO_TICKS(10));
        close(socket_fd);
    }
}

#define MAX_CLIENTS 10  // 最大客户端连接数
// 客户端连接结构体
typedef struct {
    int sock;
    TaskHandle_t task_handle;
    bool in_use;
} client_conn_t;

// 客户端连接数组
static client_conn_t clients[MAX_CLIENTS];
static SemaphoreHandle_t clients_mutex = NULL;

// 初始化客户端数组
static void init_clients(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].sock = -1;
        clients[i].task_handle = NULL;
        clients[i].in_use = false;
    }
}

static int get_tcp_mode_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return TCP_MODE_SERVER;
    }

    size_t tcpconn_len = 0;
    int mode = TCP_MODE_SERVER;
    err = nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn_len);
    if (err == ESP_OK && tcpconn_len > 0) {
        char *mode_buf = malloc(tcpconn_len);
        if (mode_buf) {
            if (nvs_get_str(nvs_handle, "tcpconn", mode_buf, &tcpconn_len) == ESP_OK) {
                mode = atoi(mode_buf);
            }
            free(mode_buf);
        }
    }

    nvs_close(nvs_handle);
    return mode;
}

// 添加客户端连接
static int add_client(int sock, TaskHandle_t handle) {
    if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!clients[i].in_use) {
                clients[i].sock = sock;
                clients[i].task_handle = handle;
                clients[i].in_use = true;
                tcp_client_count++;
                xSemaphoreGive(clients_mutex);
                return i;
            }
        }
        xSemaphoreGive(clients_mutex);
    }
    return -1;
}

// 移除客户端连接
static void remove_client(int index) {
    if (index < 0 || index >= MAX_CLIENTS) {
        return;
    }

    if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
        if (clients[index].in_use) {
            int sock_to_close = -1;

            // 先标记为不可用，避免其他线程访问
            clients[index].in_use = false;
            sock_to_close = clients[index].sock;
            clients[index].sock = -1;
            clients[index].task_handle = NULL;
            tcp_client_count--;

            xSemaphoreGive(clients_mutex);

            // 使用安全关闭函数
            safe_close_socket(sock_to_close);
        } else {
            xSemaphoreGive(clients_mutex);
        }
    }
}

// 将十六进制字符串转换为二进制数据
static int hex_to_bin(const char *hex_str, uint8_t *bin_data, int max_len) {
    int hex_len = strlen(hex_str);
    int bin_len = 0;

    // 跳过可能的0x前缀
    if (hex_len >= 2 && hex_str[0] == '0' && (hex_str[1] == 'x' || hex_str[1] == 'X')) {
        hex_str += 2;
        hex_len -= 2;
    }

    // 确保长度是偶数
    if (hex_len % 2 != 0) {
        ESP_LOGE(TAG, "Hex string length must be even");
        return 0;
    }

    bin_len = hex_len / 2;
    if (bin_len > max_len) {
        ESP_LOGE(TAG, "Hex string too long");
        return 0;
    }

    for (int i = 0; i < bin_len; i++) {
        char high = hex_str[i * 2];
        char low = hex_str[i * 2 + 1];

        // 转换高位
        if (high >= '0' && high <= '9') {
            bin_data[i] = (high - '0') << 4;
        } else if (high >= 'A' && high <= 'F') {
            bin_data[i] = (high - 'A' + 10) << 4;
        } else if (high >= 'a' && high <= 'f') {
            bin_data[i] = (high - 'a' + 10) << 4;
        } else {
            ESP_LOGE(TAG, "Invalid hex character: %c", high);
            return 0;
        }

        // 转换低位
        if (low >= '0' && low <= '9') {
            bin_data[i] |= (low - '0');
        } else if (low >= 'A' && low <= 'F') {
            bin_data[i] |= (low - 'A' + 10);
        } else if (low >= 'a' && low <= 'f') {
            bin_data[i] |= (low - 'a' + 10);
        } else {
            ESP_LOGE(TAG, "Invalid hex character: %c", low);
            return 0;
        }
    }

    return bin_len;
}

// 获取MAC地址字符串（不带冒号）
static void get_mac_str(char *mac_str, size_t max_len) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(mac_str, max_len, "%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// 心跳包任务
void tcp_server_heartbeat_task(void *pvParameters) {
    if (s_tcp_server_modbus_mode) {
        xHandleTaskTCPServerHeartbeat = NULL;
        vTaskDelete(NULL);
        return;
    }

    nvs_handle_t nvs_handle;
    esp_err_t err;

    // 打开 NVS
    err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    // 获取心跳包配置
    size_t heart_packet_size = 0;
    size_t heart_format_size = 0;
    size_t heart_interval_size = 0;

    err = nvs_get_str(nvs_handle, "heart_packet", NULL, &heart_packet_size);
    if (err != ESP_OK || heart_packet_size == 0) {
        ESP_LOGI(TAG, "No heartbeat packet configured");
    }

    err = nvs_get_str(nvs_handle, "heart_format", NULL, &heart_format_size);
    if (err != ESP_OK || heart_format_size == 0) {
        ESP_LOGI(TAG, "No heartbeat format configured");
    }

    err = nvs_get_str(nvs_handle, "heart_interval", NULL, &heart_interval_size);
    if (err != ESP_OK || heart_interval_size == 0) {
        ESP_LOGI(TAG, "No heartbeat interval configured");
    }

    // 分配内存
    char *heart_packet = heart_packet_size > 0 ? malloc(heart_packet_size) : NULL;
    char *heart_format = heart_format_size > 0 ? malloc(heart_format_size) : NULL;
    char *heart_interval_str = heart_interval_size > 0 ? malloc(heart_interval_size) : NULL;
    bool heartbeat_disabled = false;

    // 读取配置
    if (heart_packet) {
        err = nvs_get_str(nvs_handle, "heart_packet", heart_packet, &heart_packet_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read heartbeat packet");
        }
    }

    if (heart_format) {
        err = nvs_get_str(nvs_handle, "heart_format", heart_format, &heart_format_size);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read heartbeat format");
        } else if (strcmp(heart_format, "none") == 0) {
            heartbeat_disabled = true;
        }
    }

    int heart_interval = 30; // 默认30秒
    if (heart_interval_str) {
        err = nvs_get_str(nvs_handle, "heart_interval", heart_interval_str, &heart_interval_size);
        if (err == ESP_OK) {
            heart_interval = atoi(heart_interval_str);
            if (heart_interval <= 0) {
                heart_interval = 30;
            }
        }
    }

    if (heartbeat_disabled) {
        ESP_LOGI(TAG, "Heartbeat disabled for TCP Server, stopping heartbeat task");
        if (heart_packet) {
            free(heart_packet);
            heart_packet = NULL;
        }
        if (heart_format) {
            free(heart_format);
            heart_format = NULL;
        }
        if (heart_interval_str) {
            free(heart_interval_str);
            heart_interval_str = NULL;
        }
        nvs_close(nvs_handle);
        xHandleTaskTCPServerHeartbeat = NULL;
        vTaskDelete(NULL);
        return;
    }

    // 只有在没有读取到heart_packet配置时才使用MAC地址作为默认值
    // 如果用户设置了空字符串，应该尊重用户选择，不发送心跳包
    if (heart_packet == NULL) {
        // 首次使用，没有配置，使用MAC地址作为默认值
        heart_packet = malloc(13);
        if (heart_packet) {
            get_mac_str(heart_packet, 13);
        }
    }

    // 默认ASCII格式
    if (heart_format == NULL) {
        heart_format = malloc(6);
        if (heart_format) {
            strcpy(heart_format, "ascii");
        }
    }

    nvs_close(nvs_handle);

    // 心跳包循环
    while (1) {
        // 获取互斥锁
        if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
            // 遍历所有客户端连接
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i].in_use && clients[i].sock >= 0) {
                    // 只有当heart_packet不为空字符串时才发送心跳包
                    if (heart_packet && heart_format && strlen(heart_packet) > 0) {
                        int send_result = -1;
                        if (strcmp(heart_format, "hex") == 0) {
                            // 十六进制格式
                            uint8_t bin_data[128] = {0};
                            int bin_len = hex_to_bin(heart_packet, bin_data, sizeof(bin_data));

                            if (bin_len > 0) {
                                ESP_LOGI(TAG, "Sending HEX heartbeat packet to client %d, length: %d", i, bin_len);
                                send_result = send(clients[i].sock, bin_data, bin_len, MSG_NOSIGNAL);
                            } else {
                                ESP_LOGE(TAG, "Failed to convert HEX heartbeat packet");
                            }
                        } else {
                            // ASCII格式
                            ESP_LOGI(TAG, "Sending ASCII heartbeat packet to client %d: %s", i, heart_packet);
                            send_result = send(clients[i].sock, heart_packet, strlen(heart_packet), MSG_NOSIGNAL);
                        }

                        // 检查发送结果
                        if (send_result < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                                ESP_LOGE(TAG, "Heartbeat send failed to client %d: errno %d", i, errno);
                                // 标记客户端为失效，但不在此处关闭socket
                                clients[i].in_use = false;
                                int socket_to_close = clients[i].sock;
                                clients[i].sock = -1;
                                tcp_client_count--;

                                // 在释放锁后安全关闭socket
                                xSemaphoreGive(clients_mutex);
                                safe_close_socket(socket_to_close);
                                if (xSemaphoreTake(clients_mutex, portMAX_DELAY) == pdFALSE) {
                                    break; // 如果无法重新获取锁，退出循环
                                }
                            }
                        }
                    }
                }
            }
            xSemaphoreGive(clients_mutex);
        }

        // 等待指定的间隔时间
        vTaskDelay(heart_interval * 1000 / portTICK_PERIOD_MS);
    }

    // 清理
    if (heart_packet) free(heart_packet);
    if (heart_format) free(heart_format);
    if (heart_interval_str) free(heart_interval_str);

    xHandleTaskTCPServerHeartbeat = NULL;
    vTaskDelete(NULL);
}

// 在文件开始初始化互斥锁
void init_tcp_server(void) {
    if (tcp_send_mutex == NULL) {
        tcp_send_mutex = xSemaphoreCreateMutex();
    }
    if (clients_mutex == NULL) {
        clients_mutex = xSemaphoreCreateMutex();
    }
    if (tcp_send_mutex == NULL || clients_mutex == NULL) {
        ESP_LOGE("TCP_SERVER", "Failed to create mutexes");
        return;
    }

    // 创建socket清理队列和任务
    if (socket_cleanup_queue == NULL) {
        socket_cleanup_queue = xQueueCreate(CLEANUP_QUEUE_SIZE, sizeof(cleanup_item_t));
        if (socket_cleanup_queue == NULL) {
            ESP_LOGE("TCP_SERVER", "Failed to create socket cleanup queue");
            return;
        }
    }

    if (socket_cleanup_task_handle == NULL) {
        xTaskCreate(socket_cleanup_task, "socket_cleanup", 2048, NULL, 5, &socket_cleanup_task_handle);
        if (socket_cleanup_task_handle == NULL) {
            ESP_LOGE("TCP_SERVER", "Failed to create socket cleanup task");
        }
    }

    init_clients();

    int tcp_mode = get_tcp_mode_from_nvs();
    s_tcp_server_modbus_mode = TCP_MODE_IS_MODBUS(tcp_mode);

    // 启动心跳包任务（ModbusTCP模式不启动）
    if (!s_tcp_server_modbus_mode && xHandleTaskTCPServerHeartbeat == NULL) {
        xTaskCreate(tcp_server_heartbeat_task, "tcp_server_heartbeat", 4096, NULL, 5, &xHandleTaskTCPServerHeartbeat);
    }
}
/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/



static void do_retransmit(const int sock) {
  int len;
  uint8_t rx_buffer[512];

  do {
    len = recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
    if (len < 0) {
      ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
    } else if (len == 0) {
      ESP_LOGW(TAG, "Connection closed");
    } else {
      ESP_LOGI(TAG, "Received TCP data, length: %d", len);
      ESP_LOG_BUFFER_HEXDUMP(TAG, rx_buffer, len, ESP_LOG_INFO);

      nvs_handle_t nvs_handle;
      char work_mode[32] = "mqtt_tcp"; // 设置默认值

      if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        size_t size = sizeof(work_mode);
        esp_err_t err = nvs_get_str(nvs_handle, "w_mode", work_mode, &size);

        ESP_LOGI(TAG, "Current work mode: %s (err: %s)", work_mode,
                 esp_err_to_name(err));

        if (strcmp(work_mode, "modbus_tcp") == 0) {
          // 只在modbus_tcp模式下检查Modbus TCP数据包
          if (len >= 7 && rx_buffer[2] == 0x00 && rx_buffer[3] == 0x00) {
            ESP_LOGI(TAG, "Detected Modbus TCP packet");

            // 保存Transaction ID用于响应
            last_transaction_id = (rx_buffer[0] << 8) | rx_buffer[1];
            ESP_LOGI(TAG, "Updated Transaction ID: 0x%04x", last_transaction_id);

            // 提取 Modbus RTU 部分 (跳过MBAP头)
            uint8_t modbus_rtu[256];
            int rtu_len = len - 6; // 减去MBAP头长度
            memcpy(modbus_rtu, rx_buffer + 6, rtu_len);

            // modbus_tcp模式：发送RTU部分到串口，并添加CRC校验
            uint8_t modbus_rtu_with_crc[256];
            memcpy(modbus_rtu_with_crc, modbus_rtu, rtu_len);

            // 计算CRC
            uint16_t crc = calculate_crc(modbus_rtu_with_crc, rtu_len);

            // 添加CRC到数据末尾（低字节在前，高字节在后）
            modbus_rtu_with_crc[rtu_len] = crc & 0xFF;
            modbus_rtu_with_crc[rtu_len + 1] = (crc >> 8) & 0xFF;

            // 打印调试信息
            ESP_LOGI(TAG, "Sending Modbus RTU with CRC:");
            ESP_LOG_BUFFER_HEXDUMP(TAG, modbus_rtu_with_crc, rtu_len + 2,
                                 ESP_LOG_INFO);

            // 发送带CRC的完整RTU数据
            tx_task(modbus_rtu_with_crc, rtu_len + 2);
          } else {
            // 非标准Modbus TCP包，直接转发
            tx_task(rx_buffer, len);
          }
        } else {
          // 非Modbus TCP模式，直接转发数据到串口
          tx_task(rx_buffer, len);
        }

        nvs_close(nvs_handle);
      } else {
        // 无法打开NVS，直接转发数据
        tx_task(rx_buffer, len);
      }
    }
  } while (len > 0);
}

// 客户端处理任务
static void handle_client_task(void *pvParameters) {
    int client_id = *((int *)pvParameters);
    free(pvParameters); // 释放参数内存

    int sock = -1;

    // 获取套接字
    if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
        if (client_id >= 0 && client_id < MAX_CLIENTS && clients[client_id].in_use) {
            sock = clients[client_id].sock;
        }
        xSemaphoreGive(clients_mutex);
    }

    if (sock < 0) {
        ESP_LOGE(TAG, "Invalid socket in client task");
        remove_client(client_id);
        vTaskDelete(NULL);
        return;
    }

    do_retransmit(sock);

    // 任务完成，移除客户端
    remove_client(client_id);
    vTaskDelete(NULL);
}

void tcpserver_message(char *data, int len) {
    static const char *TAG = "TCP_SERVER";
    nvs_handle_t nvs_handle;
    char work_mode[32] = {0};

    // 检查互斥锁是否已初始化
    if (tcp_send_mutex == NULL) {
        ESP_LOGE(TAG, "TCP send mutex not initialized");
        return;
    }

    // 尝试获取互斥锁，设置超时时间
    if (xSemaphoreTake(tcp_send_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle) == ESP_OK) {
            size_t size = sizeof(work_mode);
            if (nvs_get_str(nvs_handle, "w_mode", work_mode, &size) == ESP_OK) {
                // 获取客户端互斥锁
                if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                    bool any_client_connected = false;

                    // 遍历所有客户端
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].in_use && clients[i].sock != -1) {
                            any_client_connected = true;

                            // 只在 modbus_tcp 模式下修改数据格式
                            if (strcmp(work_mode, "modbus_tcp") == 0 && len >= 2) {
                                // 创建新的缓冲区，包含MBAP头
                                uint8_t *tcp_data = malloc(len + 6);
                                if (tcp_data != NULL) {
                                    // 添加MBAP头
                                    tcp_data[0] = (last_transaction_id >> 8) & 0xFF;  // Transaction ID高字节
                                    tcp_data[1] = last_transaction_id & 0xFF;         // Transaction ID低字节
                                    tcp_data[2] = 0x00;  // Protocol ID (Modbus = 0)高字节
                                    tcp_data[3] = 0x00;  // Protocol ID (Modbus = 0)低字节

                                    // PDU长度 = 数据长度(原始RTU响应没有CRC)
                                    tcp_data[4] = ((len) >> 8) & 0xFF;  // Length high byte
                                    tcp_data[5] = (len) & 0xFF;         // Length low byte

                                    // 复制原始数据
                                    memcpy(tcp_data + 6, (uint8_t *)data, len);

                                    ESP_LOGI(TAG, "Sending Modbus TCP response to client %d with Transaction ID: 0x%04x, length: %d",
                                             i, last_transaction_id, len + 6);
                                    ESP_LOG_BUFFER_HEXDUMP(TAG, tcp_data, len + 6, ESP_LOG_INFO);

                                    // 发送完整的Modbus TCP数据
                                    int total_sent = 0;
                                    bool send_failed = false;
                                    while (total_sent < len + 6 && !send_failed) {
                                        int written = send(clients[i].sock, tcp_data + total_sent,
                                                        len + 6 - total_sent, MSG_NOSIGNAL);
                                        if (written < 0) {
                                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                                vTaskDelay(pdMS_TO_TICKS(10));
                                                continue;
                                            }
                                            ESP_LOGE(TAG, "Error occurred during sending to client %d: errno %d", i, errno);
                                            // 保存socket用于关闭，并标记客户端失效
                                            send_failed = true;
                                            int socket_to_close = clients[i].sock;
                                            clients[i].in_use = false;
                                            clients[i].sock = -1;
                                            tcp_client_count--;
                                            // 释放锁后关闭socket
                                            xSemaphoreGive(clients_mutex);
                                            safe_close_socket(socket_to_close);
                                            // 重新获取锁，如果失败则退出
                                            if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
                                                ESP_LOGE(TAG, "Failed to reacquire clients mutex");
                                                xSemaphoreGive(tcp_send_mutex);
                                                return;
                                            }
                                            break;
                                        }
                                        total_sent += written;
                                    }
                                    free(tcp_data);
                                }
                            } else {
                                // 非modbus_tcp模式，直接发送原始数据
                                int total_sent = 0;
                                bool send_failed = false;
                                while (total_sent < len && !send_failed) {
                                    int written = send(clients[i].sock, data + total_sent,
                                                    len - total_sent, MSG_NOSIGNAL);
                                    if (written < 0) {
                                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                            vTaskDelay(pdMS_TO_TICKS(10));
                                            continue;
                                        }
                                        ESP_LOGE(TAG, "Error occurred during sending to client %d: errno %d", i, errno);
                                        // 保存socket用于关闭，并标记客户端失效
                                        send_failed = true;
                                        int socket_to_close = clients[i].sock;
                                        clients[i].in_use = false;
                                        clients[i].sock = -1;
                                        tcp_client_count--;
                                        // 释放锁后关闭socket
                                        xSemaphoreGive(clients_mutex);
                                        safe_close_socket(socket_to_close);
                                        // 重新获取锁，如果失败则退出
                                        if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
                                            ESP_LOGE(TAG, "Failed to reacquire clients mutex");
                                            xSemaphoreGive(tcp_send_mutex);
                                            return;
                                        }
                                        break;
                                    }
                                    total_sent += written;
                                }
                            }
                        }
                    }

                    if (!any_client_connected) {
                        ESP_LOGE(TAG, "No TCP clients connected");
                    }

                    xSemaphoreGive(clients_mutex);
                } else {
                    ESP_LOGE(TAG, "Failed to acquire clients mutex");
                }
            }
            nvs_close(nvs_handle);
        }
        xSemaphoreGive(tcp_send_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex for TCP send");
    }
}
void tcp_server_send_device_info(uint8_t *u_data, int data_len, int template_index) {
    static const char *TAG = "TCP_SERVER_REPORT";
    const int display_index = template_index + 1;
    ESP_LOGI(TAG, "Processing TCP Server report for template %d", display_index);

    // 创建 JSON 根对象
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return;
    }

    // 添加基本字段
    cJSON_AddBoolToObject(root, "enabled", true);
    cJSON_AddNumberToObject(root, "command_index", display_index);

    // 从 nvs_namespace读取指定模板的配置
    nvs_handle_t nvs_handle_storage;
    if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle_storage) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open nvs_namespace");
        cJSON_Delete(root);
        return;
    }

    // 读取并添加所有配置字段
    const struct {
        const char *nvs_suffix;
        const char *json_key;
    } fields[] = {
        {"s_addr", "slave_addr"},
        {"f_code", "function_code"},
        {"r_addr", "register_addr"},
        {"r_num", "register_num"},
        {"timeout", "timeout"},
        {"d_fmt", "data_format"},
        {"i_time", "interval_time"},
        {"r_fmt", "report_format"},
        {"baud_rate", "baud_rate"},
        {"data_bit", "data_bit"},
        {"stop_bit", "stop_bit"},
        {"check_bit", "check_bit"}
    };

    char key[32];
    char value[32];
    size_t size;

    // 读取所有配置字段
    for (int j = 0; j < sizeof(fields)/sizeof(fields[0]); j++) {
        size = sizeof(value);
        snprintf(key, sizeof(key), "m%d%s", template_index, fields[j].nvs_suffix);
        if (nvs_get_str(nvs_handle_storage, key, value, &size) == ESP_OK) {
            cJSON_AddStringToObject(root, fields[j].json_key, value);
        }
    }

    // 处理响应数据
    if (!(u_data[1] & 0x80)) {  // 检查是否为错误响应
        cJSON *response_array = cJSON_CreateArray();
        cJSON *data_obj = cJSON_CreateObject();

        if (response_array && data_obj) {
            // 获取数据格式
            char data_format[16];
            size = sizeof(data_format);
            snprintf(key, sizeof(key), "m%dd_fmt", template_index);
            if (nvs_get_str(nvs_handle_storage, key, data_format, &size) != ESP_OK) {
                strcpy(data_format, "HEX");
            }

            // 解析数据
            int data_start = 3;
            int data_length = u_data[2];
            int reg_count = data_length / 2;
            if (reg_count > MAX_MODBUS_REGISTERS_PER_RESPONSE) {
                ESP_LOGW(TAG, "Response registers (%d) exceed buffer, truncating to %d",
                         reg_count, MAX_MODBUS_REGISTERS_PER_RESPONSE);
                reg_count = MAX_MODBUS_REGISTERS_PER_RESPONSE;
            }

            // 存储寄存器值（固定缓冲，避免频繁分配）
            uint16_t registers[MAX_MODBUS_REGISTERS_PER_RESPONSE];
            for (int j = 0; j < reg_count; j++) {
                registers[j] = (u_data[data_start + j*2] << 8) | u_data[data_start + j*2 + 1];
            }

            // 确定每个值需要的寄存器数量
            int regs_per_value = 1;
            if (strstr(data_format, "Double") != NULL) {
                regs_per_value = 4;
            } else if (strstr(data_format, "Long") != NULL ||
                      strstr(data_format, "Float") != NULL) {
                regs_per_value = 2;
            }

            // 转换数据
            for (int j = 0; j < reg_count; j += regs_per_value) {
                if (j + regs_per_value <= reg_count) {
                    char data_key[16];
                    snprintf(data_key, sizeof(data_key), "data%d", (j/regs_per_value) + 1);
                    char *formatted_value = convert_multi_register_data(
                        &registers[j], regs_per_value, data_format);
                    cJSON_AddStringToObject(data_obj, data_key, formatted_value);
                }
            }

            cJSON_AddItemToArray(response_array, data_obj);
            cJSON_AddItemToObject(root, "response_data", response_array);
        } else {
            if (response_array) cJSON_Delete(response_array);
            if (data_obj) cJSON_Delete(data_obj);
        }
    }

    // 发送TCP消息
    char *json_string = cJSON_Print(root);
    if (json_string) {
        // 尝试获取互斥锁
        if (xSemaphoreTake(tcp_send_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                bool any_client_connected = false;

                // 遍历所有客户端连接
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].in_use && clients[i].sock != -1) {
                        any_client_connected = true;

                        // 添加换行符以便接收端解析
                        char *send_buffer = malloc(strlen(json_string) + 3);
                        if (send_buffer) {
                            sprintf(send_buffer, "%s\r\n", json_string);
                            size_t total_len = strlen(send_buffer);
                            size_t sent = 0;

                            while (sent < total_len) {
                                int written = send(clients[i].sock,
                                                 send_buffer + sent,
                                                 total_len - sent,
                                                 MSG_NOSIGNAL);
                                if (written < 0) {
                                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                        vTaskDelay(pdMS_TO_TICKS(10));
                                        continue;
                                    }
                                    ESP_LOGE(TAG, "Error sending JSON data to client %d: errno %d", i, errno);
                                    // 如果发送失败，标记客户端断开
                                    clients[i].in_use = false;
                                    clients[i].sock = -1;
                                    tcp_client_count--;
                                    break;
                                }
                                sent += written;
                            }

                            if (sent == total_len) {
                                ESP_LOGI(TAG, "Successfully sent JSON data to client %d", i);
                                ESP_LOGI(TAG, "Sent data: %s", send_buffer);
                            }
                            free(send_buffer);
                        }
                    }
                }

                if (!any_client_connected) {
                    ESP_LOGE(TAG, "No TCP clients connected");
                }
                xSemaphoreGive(clients_mutex);
            } else {
                ESP_LOGE(TAG, "Failed to acquire clients mutex");
            }
            xSemaphoreGive(tcp_send_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to acquire mutex for TCP send");
        }
        free(json_string);
    }

    nvs_close(nvs_handle_storage);
    cJSON_Delete(root);
}
static void tcp_server_task(void *pvParameters) {
  char addr_str[128];
  int addr_family = (int)pvParameters;
  int ip_protocol = 0;
  int keepAlive = 1;
  int keepIdle = KEEPALIVE_IDLE;
  int keepInterval = KEEPALIVE_INTERVAL;
  int keepCount = KEEPALIVE_COUNT;
  struct sockaddr_storage dest_addr;

  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  size_t use_tcp, tcpconn, tcp_server, tcp_port;
  ret = nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp);
  nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn);
  nvs_get_str(nvs_handle, "tcp_server", NULL, &tcp_server);
  nvs_get_str(nvs_handle, "tcp_port", NULL, &tcp_port);
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_use_tcp = malloc(use_tcp);
    char *nvs_tcpconn = malloc(tcpconn);
    char *nvs_tcp_server = malloc(tcp_server);
    char *nvs_tcp_port = malloc(tcp_port);

    if (nvs_use_tcp == NULL || nvs_tcpconn == NULL || nvs_tcp_server == NULL ||
        nvs_tcp_port == NULL) {
      printf("Memory allocation failed\n");
      free(nvs_use_tcp);
      free(nvs_tcpconn);
      free(nvs_tcp_server);
      free(nvs_tcp_port);
    } else {
      ret = nvs_get_str(nvs_handle, "use_tcp", nvs_use_tcp, &use_tcp);
      nvs_get_str(nvs_handle, "tcpconn", nvs_tcpconn, &tcpconn);
      nvs_get_str(nvs_handle, "tcp_server", nvs_tcp_server, &tcp_server);
      nvs_get_str(nvs_handle, "tcp_port", nvs_tcp_port, &tcp_port);
      int tcp_mode = (nvs_tcpconn && nvs_tcpconn[0] != '\0')
                         ? atoi(nvs_tcpconn)
                         : TCP_MODE_SERVER;
      s_tcp_server_modbus_mode = TCP_MODE_IS_MODBUS(tcp_mode);
      if (ret == ESP_OK) {
        if (addr_family == AF_INET) {
          struct sockaddr_in *dest_addr_ip4 = (struct sockaddr_in *)&dest_addr;
          dest_addr_ip4->sin_addr.s_addr = htonl(INADDR_ANY);
          dest_addr_ip4->sin_family = AF_INET;
          const char *port_str =
              (nvs_tcp_port && nvs_tcp_port[0] != '\0')
                  ? nvs_tcp_port
                  : (TCP_MODE_IS_MODBUS(tcp_mode) ? "502" : "8888");
          int port_num = atoi(port_str);
          dest_addr_ip4->sin_port = htons(port_num);
          ip_protocol = IPPROTO_IP;
          ESP_LOGI(TAG, "Socket bound, port %d", port_num);
        }
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_tcp);
      free(nvs_tcpconn);
      free(nvs_tcp_server);
      free(nvs_tcp_port);
    }
  }
  nvs_close(nvs_handle);

  int listen_sock = socket(addr_family, SOCK_STREAM, ip_protocol);
  if (listen_sock < 0) {
    ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
    vTaskDelete(NULL);
    return;
  }

  int opt = 1;
  setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  if (addr_family == AF_INET6) {
    setsockopt(listen_sock, IPPROTO_IPV6, IPV6_V6ONLY, &opt, sizeof(opt));
  }

  ESP_LOGI(TAG, "Socket created");

  int err = bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0) {
    ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
    ESP_LOGE(TAG, "IPPROTO: %d", addr_family);
    goto CLEAN_UP;
  }

  err = listen(listen_sock, 1);
  if (err != 0) {
    ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
    goto CLEAN_UP;
  }

      while (1) {
        ESP_LOGI(TAG, "Socket listening");
        struct sockaddr_storage source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
            continue;
        }

        if (tcp_client_count >= MAX_CLIENTS) {
            ESP_LOGE(TAG, "Max clients reached, rejecting connection");
            safe_close_socket(sock);
            continue;
        }

        // 设置socket选项
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

        int *client_id = malloc(sizeof(int));
        if (client_id == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for client ID");
            safe_close_socket(sock);
            continue;
        }
        *client_id = add_client(sock, NULL);

        // 创建客户端处理任务（增加栈大小到8KB以支持大数据传输）
        TaskHandle_t task_handle = NULL;
        xTaskCreate(handle_client_task, "tcp_client", 8192, client_id, 5, &task_handle);

        // 更新任务句柄
        if (xSemaphoreTake(clients_mutex, portMAX_DELAY)) {
            if (*client_id >= 0 && *client_id < MAX_CLIENTS && clients[*client_id].in_use) {
                clients[*client_id].task_handle = task_handle;
            }
            xSemaphoreGive(clients_mutex);
        }
    }

CLEAN_UP:
  safe_close_socket(listen_sock);
  vTaskDelete(NULL);
}

void start_tcp_server(void) {
    init_tcp_server();
    xTaskCreate(tcp_server_task, "tcp_server", 4096, (void *)AF_INET, 5, NULL);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
