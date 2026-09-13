/*
 * @Author: Orion
 * @Date: 2024-07-16 10:00:00
 * @LastEditors: Orion
 * @LastEditTime: 2025-03-19 16:00:39
 * @FilePath: \SERIIAL_SERVER\main\sx_udp_multicast.c
 * @Description: UDP组播功能实现，用于跨网段设备搜索
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/task_snapshot.h"
#include "nvs_flash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>

#include "cJSON.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/igmp.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include <lwip/netdb.h>

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

static const char *TAG = "UDPMULTICAST";
#define MULTICAST_PORT 37211
#define MULTICAST_TTL 32  // 增加TTL值，确保跨网段传输
#define MULTICAST_IPV4_ADDR "239.255.255.250"  // 标准组播地址

// 声明外部变量
extern char got_ip_addrs[16];
extern char got_ip_netmask[16];
extern char got_ip_gw[16];

// 设备名称存储
static char device_name[32] = "串口服务器";  // 默认名称
static char nvs_device_name[32] = {0};  // 从NVS读取的设备名称
static bool name_initialized = false;  // 初始化标志

TaskHandle_t xHandleUDPMulticastTask;

static void udp_multicast_task(void *pvParameters);
void start_udp_multicast(void);
void kill_udp_multicast(void);

/**
 * @brief 获取设备名称
 *
 * @return const char* 设备名称
 */
const char* get_device_name(void)
{
    static const char *TAG = "GET_DEVICE_NAME";

    // 如果已经初始化过，直接返回缓存的名称
    if (name_initialized) {
        return nvs_device_name;
    }

    // 从NVS中读取设备名称
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        // 使用默认名称
        strncpy(nvs_device_name, device_name, sizeof(nvs_device_name) - 1);
        name_initialized = true;
        return nvs_device_name;
    }

    // 获取设备名称
    size_t name_size = 0;
    err = nvs_get_str(nvs_handle, "host_names", NULL, &name_size);
    if (err == ESP_OK && name_size > 0 && name_size <= sizeof(nvs_device_name)) {
        // 读取设备名称
        err = nvs_get_str(nvs_handle, "host_names", nvs_device_name, &name_size);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "Read device name from NVS: %s", nvs_device_name);
            name_initialized = true;
        } else {
            ESP_LOGE(TAG, "Error reading device name: %s", esp_err_to_name(err));
            // 使用默认名称
            strncpy(nvs_device_name, device_name, sizeof(nvs_device_name) - 1);
            name_initialized = true;
        }
    } else {
        // 如果NVS中没有设备名称或出错，使用默认名称
        ESP_LOGI(TAG, "No device name in NVS, using default: %s", device_name);
        strncpy(nvs_device_name, device_name, sizeof(nvs_device_name) - 1);
        name_initialized = true;
    }

    nvs_close(nvs_handle);
    return nvs_device_name;
}

/**
 * @brief 重置设备名称缓存
 *
 * 当设备名称在NVS中更新后，调用此函数重置缓存状态，
 * 确保下次调用get_device_name时会重新从NVS读取最新的设备名称
 */
void reset_device_name_cache(void)
{
    static const char *TAG = "RESET_DEVICE_NAME";
    ESP_LOGI(TAG, "Resetting device name cache");
    // 重置初始化标志，强制下次调用get_device_name时重新读取
    name_initialized = false;
}

static void udp_multicast_task(void *pvParameters) {
    char rx_buffer[512];
    char addr_str[128];
    int addr_family = AF_INET;
    int ip_protocol = 0;
    struct sockaddr_in dest_addr;
    struct sockaddr_in source_addr;
    struct ip_mreq imr;
    int sock;

    // 添加重试计数器和上次日志时间
    int retry_count = 0;
    uint32_t last_log_time = 0;

    while (1) {
        // 限制日志输出频率
        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
        bool should_log = (current_time - last_log_time > 10000); // 每10秒输出一次详细日志

        if (should_log) {
            ESP_LOGI(TAG, "UDP多播任务运行中...");
            last_log_time = current_time;
        }

        if (addr_family == AF_INET) {
            dest_addr.sin_addr.s_addr = htonl(INADDR_ANY);
            dest_addr.sin_family = AF_INET;
            dest_addr.sin_port = htons(MULTICAST_PORT);
            ip_protocol = IPPROTO_IP;
        }

        sock = socket(addr_family, SOCK_DGRAM, ip_protocol);
        if (sock < 0) {
            ESP_LOGE(TAG, "无法创建套接字: errno %d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000)); // 添加延迟，避免快速重试
            continue; // 使用continue而不是break，允许重试
        }

        if (should_log) {
            ESP_LOGI(TAG, "多播套接字已创建");
        }

        // 设置SO_REUSEADDR选项
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        // 设置广播选项，同时支持广播
        int broadcast = 1;
        setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

        // 绑定端口
        int err = bind(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
        if (err < 0) {
            ESP_LOGE(TAG, "套接字无法绑定: errno %d", errno);
            closesocket(sock); // 确保关闭套接字
            vTaskDelay(pdMS_TO_TICKS(1000)); // 添加延迟，避免快速重试
            continue; // 使用continue而不是break，允许重试
        }

        if (should_log) {
            ESP_LOGI(TAG, "套接字已绑定，端口 %d", MULTICAST_PORT);
        }

        // 加入组播组
        memset(&imr, 0, sizeof(struct ip_mreq));
        inet_aton(MULTICAST_IPV4_ADDR, &imr.imr_multiaddr);
        imr.imr_interface.s_addr = htonl(INADDR_ANY);

        err = setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr, sizeof(struct ip_mreq));
        if (err < 0) {
            ESP_LOGE(TAG, "加入组播组失败: errno %d", errno);
            // 即使加入组播组失败，也继续运行，因为我们还支持广播
            if (should_log) {
                ESP_LOGI(TAG, "继续运行，但不支持组播，仍支持广播");
            }
        } else if (should_log) {
            ESP_LOGI(TAG, "已加入组播组 %s", MULTICAST_IPV4_ADDR);
        }

        // 设置组播TTL
        uint8_t ttl = MULTICAST_TTL;
        setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

        // 设置IP_PKTINFO，以便获取目标IP信息
        setsockopt(sock, IPPROTO_IP, IP_PKTINFO, &opt, sizeof(opt));

        // 打印当前网络信息，便于调试
        if (should_log) {
            ESP_LOGI(TAG, "设备网络信息 - IP: %s, 子网掩码: %s, 网关: %s",
                    got_ip_addrs, got_ip_netmask, got_ip_gw);
            ESP_LOGI(TAG, "正在监听发现请求，端口 %d", MULTICAST_PORT);
        }

        socklen_t socklen = sizeof(source_addr);
        int consecutive_errors = 0; // 跟踪连续错误次数
        bool waiting_logged = false; // 是否已经输出了等待消息

        // 设置套接字超时，而不是使用非阻塞模式
        struct timeval tv;
        tv.tv_sec = 1;  // 1秒超时
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        while (1) {
            // 减少日志输出频率
            current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (current_time - last_log_time > 30000) { // 每30秒输出一次
                ESP_LOGI(TAG, "等待组播/广播数据...");
                last_log_time = current_time;
                waiting_logged = true;
            }

            // 接收数据
            int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0,
                             (struct sockaddr *)&source_addr, &socklen);

            // 错误处理
            if (len < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
                    // 超时，正常情况
                    vTaskDelay(pdMS_TO_TICKS(10)); // 短暂延迟
                    continue;
                }

                consecutive_errors++;
                if (consecutive_errors > 5) {
                    ESP_LOGE(TAG, "recvfrom失败: errno %d", errno);
                    consecutive_errors = 0;
                    break; // 退出内部循环，重新创建套接字
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            // 成功接收数据，重置错误计数
            consecutive_errors = 0;
            waiting_logged = false;

            // 获取发送者的IP地址
            if (source_addr.sin_family == AF_INET) {
                inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str,
                          sizeof(addr_str) - 1);
            }

            rx_buffer[len] = 0; // 确保字符串以null结尾
            ESP_LOGI(TAG, "收到 %d 字节，来自 %s", len, addr_str);

            // 只在调试级别显示接收到的数据内容
            ESP_LOGI(TAG, "数据内容: %s", rx_buffer);

            // 解析JSON
            cJSON *pJsonRoot = cJSON_Parse(rx_buffer);
            if (pJsonRoot == NULL) {
                const char *error_ptr = cJSON_GetErrorPtr();
                if (error_ptr != NULL) {
                    ESP_LOGE(TAG, "JSON解析错误: %s", error_ptr);
                }
                // 添加打印收到的原始消息内容
                ESP_LOGE(TAG, "收到的原始消息内容: %s", rx_buffer);
                continue;
            }

            // 处理命令
            char *udpmsg = NULL;
            cJSON *device = cJSON_GetObjectItem(pJsonRoot, "device");
            if (cJSON_IsString(device) && (device->valuestring != NULL)) {
                if (strcmp(device->valuestring, "scan") == 0) {
                    // 设备搜索请求，返回设备信息
                    ESP_LOGI(TAG, "收到扫描命令，来自 %s", addr_str);

                    // 获取设备信息
                    udpmsg = send_device_ip();
                    if (udpmsg == NULL) {
                        ESP_LOGE(TAG, "生成设备信息响应失败");
                        cJSON_Delete(pJsonRoot);
                        continue;
                    }

                    // 创建一个新的套接字专门用于发送响应
                    int reply_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                    if (reply_sock < 0) {
                        ESP_LOGE(TAG, "无法创建回复套接字: errno %d", errno);
                        free(udpmsg);
                        cJSON_Delete(pJsonRoot);
                        continue;
                    }

                    // 设置套接字选项
                    uint8_t ttl_val = MULTICAST_TTL;
                    setsockopt(reply_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl_val, sizeof(ttl_val));
                    int opt_val = 1;
                    setsockopt(reply_sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

                    // 绑定到设备IP地址
                    struct sockaddr_in bind_addr;
                    bind_addr.sin_family = AF_INET;
                    bind_addr.sin_port = htons(0);  // 使用任意可用端口
                    inet_aton(got_ip_addrs, &bind_addr.sin_addr);

                    if (bind(reply_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
                        ESP_LOGE(TAG, "回复套接字无法绑定: errno %d", errno);
                        closesocket(reply_sock);
                        free(udpmsg);
                        cJSON_Delete(pJsonRoot);
                        continue;
                    }

                    // 准备组播地址
                    struct sockaddr_in multicast_addr;
                    memset(&multicast_addr, 0, sizeof(multicast_addr));
                    multicast_addr.sin_family = AF_INET;
                    multicast_addr.sin_port = htons(MULTICAST_PORT);
                    inet_aton(MULTICAST_IPV4_ADDR, &multicast_addr.sin_addr);

                    // 准备广播地址
                    struct sockaddr_in broadcast_addr;
                    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
                    broadcast_addr.sin_family = AF_INET;
                    broadcast_addr.sin_port = htons(MULTICAST_PORT);
                    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

                    // 发送响应 - 尝试多种方式
                    bool response_sent = false;

                    // 1. 直接回复给请求者
                    err = sendto(reply_sock, udpmsg, strlen(udpmsg), 0,
                              (struct sockaddr *)&source_addr, sizeof(source_addr));
                    if (err >= 0) {
                        ESP_LOGI(TAG, "直接响应发送成功: %d 字节", err);
                        response_sent = true;
                    } else {
                        ESP_LOGE(TAG, "发送直接响应错误: %s", strerror(errno));
                    }

                    // 短暂延迟
                    vTaskDelay(pdMS_TO_TICKS(10));

                    // 2. 发送到组播地址
                    err = sendto(reply_sock, udpmsg, strlen(udpmsg), 0,
                              (struct sockaddr *)&multicast_addr, sizeof(multicast_addr));
                    if (err >= 0) {
                        ESP_LOGI(TAG, "组播响应发送成功: %d 字节", err);
                        response_sent = true;
                    } else {
                        ESP_LOGE(TAG, "发送组播响应错误: %s", strerror(errno));
                    }

                    // 短暂延迟
                    vTaskDelay(pdMS_TO_TICKS(10));

                    // 3. 发送到广播地址
                    err = sendto(reply_sock, udpmsg, strlen(udpmsg), 0,
                              (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
                    if (err >= 0) {
                        ESP_LOGI(TAG, "广播响应发送成功: %d 字节", err);
                        response_sent = true;
                    } else {
                        ESP_LOGE(TAG, "发送广播响应错误: %s", strerror(errno));
                    }

                    // 如果所有方法都失败，尝试创建新套接字
                    if (!response_sent) {
                        ESP_LOGI(TAG, "尝试使用新套接字...");
                        closesocket(reply_sock);

                        reply_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                        if (reply_sock >= 0) {
                            // 设置套接字选项
                            setsockopt(reply_sock, SOL_SOCKET, SO_BROADCAST, &opt_val, sizeof(opt_val));

                            // 尝试广播
                            err = sendto(reply_sock, udpmsg, strlen(udpmsg), 0,
                                      (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
                            if (err >= 0) {
                                ESP_LOGI(TAG, "使用新套接字广播响应成功: %d 字节", err);
                            } else {
                                ESP_LOGE(TAG, "使用新套接字发送广播响应仍然失败: %s", strerror(errno));
                            }
                        }
                    }

                    // 关闭回复套接字并释放内存
                    closesocket(reply_sock);
                    free(udpmsg);
                }
                else if (strcmp(device->valuestring, "set") == 0) {
                    // 设置设备参数请求
                    ESP_LOGI(TAG, "收到设置命令，来自 %s", addr_str);

                    // 检查是否有用户名和密码
                    cJSON *username = cJSON_GetObjectItem(pJsonRoot, "username");
                    cJSON *password = cJSON_GetObjectItem(pJsonRoot, "password");

                    // 如果没有提供用户名和密码，返回认证错误
                    if (!cJSON_IsString(username) || !cJSON_IsString(password) ||
                        username->valuestring == NULL || password->valuestring == NULL) {
                        ESP_LOGE(TAG, "未提供用户名或密码");

                        // 创建错误响应
                        cJSON *response = cJSON_CreateObject();
                        cJSON_AddStringToObject(response, "status", "error");
                        cJSON_AddStringToObject(response, "message", "Authentication required");
                        char *errMsg = cJSON_Print(response);
                        cJSON_Delete(response);

                        // 发送错误响应
                        int reply_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                        if (reply_sock >= 0) {
                            sendto(reply_sock, errMsg, strlen(errMsg), 0,
                                  (struct sockaddr *)&source_addr, sizeof(source_addr));
                            closesocket(reply_sock);
                        }

                        free(errMsg);
                        cJSON_Delete(pJsonRoot);
                        continue;
                    }

                    // 验证用户名和密码
                    bool auth_success = verify_credentials(username->valuestring, password->valuestring);
                    if (!auth_success) {
                        ESP_LOGE(TAG, "认证失败");

                        // 创建认证失败响应
                        cJSON *response = cJSON_CreateObject();
                        cJSON_AddStringToObject(response, "status", "error");
                        cJSON_AddStringToObject(response, "message", "Authentication failed");
                        char *errMsg = cJSON_Print(response);
                        cJSON_Delete(response);

                        // 发送错误响应
                        int reply_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                        if (reply_sock >= 0) {
                            sendto(reply_sock, errMsg, strlen(errMsg), 0,
                                  (struct sockaddr *)&source_addr, sizeof(source_addr));
                            closesocket(reply_sock);
                        }

                        free(errMsg);
                        cJSON_Delete(pJsonRoot);
                        continue;
                    }

                    // 认证成功，处理设置参数
                    ESP_LOGI(TAG, "认证成功，处理设置参数");

                    // 获取参数对象
                    cJSON *parm = cJSON_GetObjectItem(pJsonRoot, "parm");
                    if (!cJSON_IsObject(parm)) {
                        ESP_LOGE(TAG, "参数格式错误");
                        cJSON_Delete(pJsonRoot);
                        continue;
                    }

                    // 处理设置参数
                    bool update_success = update_device_settings(parm);

                    // 创建响应
                    cJSON *response = cJSON_CreateObject();
                    if (update_success) {
                        cJSON_AddStringToObject(response, "status", "success");
                        cJSON_AddStringToObject(response, "message", "Settings updated");
                        cJSON_AddStringToObject(response, "change", "ok");
                    } else {
                        cJSON_AddStringToObject(response, "status", "error");
                        cJSON_AddStringToObject(response, "message", "Failed to update settings");
                    }

                    char *respMsg = cJSON_Print(response);
                    cJSON_Delete(response);

                    // 发送响应 - 跨网段优化版
                    ESP_LOGI(TAG, "准备发送设置响应: %s", respMsg);

                    // 创建响应套接字 - 不绑定到特定IP
                    int reply_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
                    if (reply_sock < 0) {
                        ESP_LOGE(TAG, "创建响应套接字失败: errno %d (%s)", errno, strerror(errno));
                    } else {
                        // 设置套接字选项
                        int opt_val = 1;
                        setsockopt(reply_sock, SOL_SOCKET, SO_REUSEADDR, &opt_val, sizeof(opt_val));

                        // 不绑定套接字，使用系统自动分配的地址和端口
                        ESP_LOGI(TAG, "使用未绑定套接字发送响应");

                        // 1. 首先尝试直接发送到源地址
                        // 确保使用源地址的端口
                        struct sockaddr_in direct_addr = source_addr;

                        int err = sendto(reply_sock, respMsg, strlen(respMsg), 0,
                                      (struct sockaddr *)&direct_addr, sizeof(direct_addr));
                        if (err < 0) {
                            ESP_LOGE(TAG, "直接发送响应失败: errno %d (%s)", errno, strerror(errno));
                        } else {
                            ESP_LOGI(TAG, "直接发送响应成功: %d 字节到 %s:%d",
                                   err, addr_str, ntohs(direct_addr.sin_port));
                        }

                        // 短暂延迟，确保数据包有时间传输
                        vTaskDelay(pdMS_TO_TICKS(50));

                        // 2. 通过组播发送响应
                        struct sockaddr_in multicast_addr;
                        memset(&multicast_addr, 0, sizeof(multicast_addr));
                        multicast_addr.sin_family = AF_INET;
                        multicast_addr.sin_port = htons(MULTICAST_PORT);  // 使用与工具相同的端口
                        multicast_addr.sin_addr.s_addr = inet_addr(MULTICAST_IPV4_ADDR);

                        // 设置组播TTL为最大值，确保跨网段传输
                        uint8_t ttl = 255;  // 最大TTL值
                        setsockopt(reply_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

                        // 发送到组播地址
                        err = sendto(reply_sock, respMsg, strlen(respMsg), 0,
                                  (struct sockaddr *)&multicast_addr, sizeof(multicast_addr));
                        if (err < 0) {
                            ESP_LOGE(TAG, "组播发送响应失败: errno %d (%s)", errno, strerror(errno));
                        } else {
                            ESP_LOGI(TAG, "组播发送响应成功: %d 字节到 %s:%d",
                                   err, MULTICAST_IPV4_ADDR, MULTICAST_PORT);
                        }

                        // 短暂延迟，确保数据包有时间传输
                        vTaskDelay(pdMS_TO_TICKS(50));

                        // 3. 通过广播发送响应
                        struct sockaddr_in broadcast_addr;
                        memset(&broadcast_addr, 0, sizeof(broadcast_addr));
                        broadcast_addr.sin_family = AF_INET;
                        broadcast_addr.sin_port = htons(MULTICAST_PORT);  // 使用与工具相同的端口
                        broadcast_addr.sin_addr.s_addr = inet_addr("255.255.255.255");

                        // 设置广播选项
                        int broadcast_enable = 1;
                        setsockopt(reply_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));

                        // 发送到广播地址
                        err = sendto(reply_sock, respMsg, strlen(respMsg), 0,
                                  (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
                        if (err < 0) {
                            ESP_LOGE(TAG, "广播发送响应失败: errno %d (%s)", errno, strerror(errno));
                        } else {
                            ESP_LOGI(TAG, "广播发送响应成功: %d 字节到 255.255.255.255:%d",
                                   err, MULTICAST_PORT);
                        }

                        // 短暂延迟，确保数据包有时间传输
                        vTaskDelay(pdMS_TO_TICKS(50));

                        // 4. 尝试发送到特定子网的广播地址
                        // 提取源IP的网段，构造该网段的广播地址
                        char source_network[16] = {0};
                        strncpy(source_network, addr_str, sizeof(source_network) - 1);
                        // 查找最后一个点
                        char *last_dot = strrchr(source_network, '.');
                        if (last_dot != NULL) {
                            // 替换为广播地址
                            strcpy(last_dot + 1, "255");

                            struct sockaddr_in subnet_broadcast;
                            memset(&subnet_broadcast, 0, sizeof(subnet_broadcast));
                            subnet_broadcast.sin_family = AF_INET;
                            subnet_broadcast.sin_port = htons(MULTICAST_PORT);
                            subnet_broadcast.sin_addr.s_addr = inet_addr(source_network);

                            err = sendto(reply_sock, respMsg, strlen(respMsg), 0,
                                      (struct sockaddr *)&subnet_broadcast, sizeof(subnet_broadcast));
                            if (err < 0) {
                                ESP_LOGE(TAG, "子网广播发送响应失败: errno %d (%s)", errno, strerror(errno));
                            } else {
                                ESP_LOGI(TAG, "子网广播发送响应成功: %d 字节到 %s:%d",
                                       err, source_network, MULTICAST_PORT);
                            }
                        }

                        // 5. 重试直接发送，使用不同的端口
                        // 有时工具可能在不同的端口监听
                        direct_addr.sin_port = htons(MULTICAST_PORT);
                        err = sendto(reply_sock, respMsg, strlen(respMsg), 0,
                                  (struct sockaddr *)&direct_addr, sizeof(direct_addr));
                        if (err < 0) {
                            ESP_LOGE(TAG, "重试直接发送响应失败: errno %d (%s)", errno, strerror(errno));
                        } else {
                            ESP_LOGI(TAG, "重试直接发送响应成功: %d 字节到 %s:%d",
                                   err, addr_str, MULTICAST_PORT);
                        }

                        // 关闭套接字
                        closesocket(reply_sock);
                    }

                    free(respMsg);

                    // 如果设置成功且包含重启命令，则重启设备
                    if (update_success) {
                        cJSON *command = cJSON_GetObjectItem(parm, "command");
                        if (cJSON_IsString(command) && command->valuestring != NULL &&
                            strcmp(command->valuestring, "reboot") == 0) {
                            ESP_LOGI(TAG, "收到重启命令，设备将在3秒后重启");
                            vTaskDelay(pdMS_TO_TICKS(3000));
                            esp_restart();
                        }
                    }
                }
            }

            // 释放JSON对象
            cJSON_Delete(pJsonRoot);
        }

        // 如果跳出内部循环，关闭套接字并重新开始
        if (sock >= 0) {
            // 尝试离开组播组
            setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &imr, sizeof(struct ip_mreq));
            closesocket(sock);
        }

        ESP_LOGI(TAG, "套接字已关闭，正在重启UDP多播任务");
        vTaskDelay(pdMS_TO_TICKS(1000)); // 添加延迟，避免快速重试

        // 增加重试计数
        retry_count++;
        if (retry_count > 10) {
            // 如果重试次数过多，延长等待时间
            ESP_LOGI(TAG, "多次重试后仍失败，延长等待时间");
            vTaskDelay(pdMS_TO_TICKS(5000));
            retry_count = 0;
        }
    }
}

void start_udp_multicast(void) {
    // 如果任务已经存在，先停止它
    if (xHandleUDPMulticastTask != NULL) {
        kill_udp_multicast();
        vTaskDelay(pdMS_TO_TICKS(100)); // 等待任务完全停止
    }
    // 创建新任务
    xTaskCreate(udp_multicast_task, "udp_multicast", 4096, (void *)AF_INET, 5, &xHandleUDPMulticastTask);
}

void kill_udp_multicast(void) {
    // 使用更安全的方式删除任务
    TaskHandle_t temp = xHandleUDPMulticastTask;
    if (temp != NULL) {
        xHandleUDPMulticastTask = NULL; // 先将全局句柄置空，防止重复删除
        vTaskDelay(pdMS_TO_TICKS(10)); // 给任务一点时间完成当前操作

        // 检查任务是否仍然存在
        if (eTaskGetState(temp) != eDeleted) {
            vTaskDelete(temp);
        }
    }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
