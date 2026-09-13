/*
 * @Author: Orion
 * @Date: 2024-02-20 15:58:29
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-10 14:33:06
 * @FilePath: \ETH_TH\main\sx_init_wifi.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "sx_gpio.h"

#include "sx_web_server.h"
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sx_utils.h>

// 添加外部函数和变量声明
extern esp_timer_handle_t platform_timer;
extern esp_timer_handle_t heartbeat_timer;
extern void platform_timer_callback(void *arg);
extern void heartbeat_timer_callback(void *arg);
extern bool is_network_connected;

// 添加缺失的变量定义
static bool is_intranet_mode = false; // 默认外网模式

// 添加强制网络状态更新函数
void force_network_status_update(void) {
    // 这个函数可以用来强制更新网络状态
    // 在此处添加网络状态更新逻辑
    ESP_LOGI("NETWORK_STATUS", "Network status update forced");
    
    // 可以在这里添加其他网络状态更新逻辑
    // 例如：检查网络连接状态、更新IP地址显示等
}

// #define EXAMPLE_WIFI_SSID "Xiaomi_LIKONG"
#define EXAMPLE_MAXIMUM_RETRY 86400

#ifdef CONFIG_EXAMPLE_STATIC_DNS_AUTO
#define EXAMPLE_MAIN_DNS_SERVER EXAMPLE_STATIC_GW_ADDR
#define EXAMPLE_BACKUP_DNS_SERVER "0.0.0.0"
#else
#define EXAMPLE_MAIN_DNS_SERVER "8.8.8.8"
#define EXAMPLE_BACKUP_DNS_SERVER "114.114.114.114"
#endif
#ifdef CONFIG_EXAMPLE_STATIC_DNS_RESOLVE_TEST
#define EXAMPLE_RESOLVE_DOMAIN CONFIG_EXAMPLE_STATIC_RESOLVE_DOMAIN
#endif

static EventGroupHandle_t s_wifi_event_group;

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static const char *TAG = "WIFI_STA";

static int s_retry_num = 0;

typedef struct {
  char ssid[32];
  char password[64];
} wifi_credentials_t;

int is_netif_initialized = 0;
#define HOUR_IN_MS (60 * 60 * 1000)
#define MIN_IN_MS (60 * 1000)
#define SECOND_IN_MS (1000)
static int64_t s_first_disconnect_time = 0; // 首次断开连接的时间
static int64_t s_last_retry_time = 0;       // 上次重试连接的时间
static bool s_wifi_connecting = false;       // WiFi连接状态标志
static TaskHandle_t s_wifi_retry_task_handle = NULL; // 重连任务句柄

// 添加WiFi事件处理句柄和清理函数
static esp_event_handler_instance_t s_instance_any_id = NULL;
static esp_event_handler_instance_t s_instance_got_ip = NULL;

// 添加WiFi重连控制变量
static bool s_wifi_reconnect_paused = false; // 是否暂停重连
static SemaphoreHandle_t s_wifi_control_mutex = NULL; // 控制互斥锁

// 初始化控制互斥锁
static void init_wifi_control_mutex(void) {
  if (s_wifi_control_mutex == NULL) {
    s_wifi_control_mutex = xSemaphoreCreateMutex();
  }
}

// 暂停WiFi自动重连
void pause_wifi_reconnect(void) {
  init_wifi_control_mutex();
  if (s_wifi_control_mutex != NULL && xSemaphoreTake(s_wifi_control_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // 只有当状态真正改变时才记录日志
    if (!s_wifi_reconnect_paused) {
      s_wifi_reconnect_paused = true;
      ESP_LOGI(TAG, "WiFi reconnect paused, previous state: false");
    }
    xSemaphoreGive(s_wifi_control_mutex);
  } else {
    ESP_LOGW(TAG, "Failed to take mutex for pausing WiFi reconnect");
  }
}

// 恢复WiFi自动重连
void resume_wifi_reconnect(void) {
  init_wifi_control_mutex();
  if (s_wifi_control_mutex != NULL && xSemaphoreTake(s_wifi_control_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // 只有当状态真正改变时才记录日志
    if (s_wifi_reconnect_paused) {
      s_wifi_reconnect_paused = false;
      ESP_LOGI(TAG, "WiFi reconnect resumed, previous state: true");

      // 如果需要重连但之前被暂停，恢复后立即尝试连接一次
      if (s_wifi_connecting) {
        ESP_LOGI(TAG, "Reconnect was needed but paused, trying to connect now");
        esp_err_t err = esp_wifi_connect();
        ESP_LOGI(TAG, "Immediate reconnect attempt result: %s", esp_err_to_name(err));
        s_last_retry_time = esp_timer_get_time() / 1000;
      }
    }
    xSemaphoreGive(s_wifi_control_mutex);
  } else {
    ESP_LOGW(TAG, "Failed to take mutex for resuming WiFi reconnect");
  }
}

// 检查是否允许WiFi扫描
bool is_wifi_scan_allowed(void) {
  // 如果WiFi已连接，则允许扫描
  if (!s_wifi_connecting) {
    return true;
  }

  // 如果WiFi正在尝试连接且距离上次尝试时间不足5秒，不允许扫描
  int64_t current_time = esp_timer_get_time() / 1000;
  if (s_last_retry_time > 0 && (current_time - s_last_retry_time) < 5000) {
    ESP_LOGW(TAG, "WiFi scan not allowed: too close to last reconnect attempt (%lld ms)", current_time - s_last_retry_time);
    return false;
  }

  return true;
}

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

static esp_err_t example_set_dns_server(esp_netif_t *netif, uint32_t addr,
                                        esp_netif_dns_type_t type) {
  if (addr && (addr != IPADDR_NONE)) {
    esp_netif_dns_info_t dns;
    dns.ip.u_addr.ip4.addr = addr;
    dns.ip.type = IPADDR_TYPE_V4;
    ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, type, &dns));
  }
  return ESP_OK;
}

static void example_set_static_ip(esp_netif_t *netif) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_safe_reset());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  size_t netconn, is_dhcp, static_ip, static_netmask, static_gateway, wifi_ssid,
      wifi_password, static_dns1, static_dns2 = 0;
  ret = nvs_get_str(nvs_handle, "netconn", NULL, &netconn);
  nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);
  nvs_get_str(nvs_handle, "static_ip", NULL, &static_ip);
  nvs_get_str(nvs_handle, "static_netmask", NULL, &static_netmask);
  nvs_get_str(nvs_handle, "static_gateway", NULL, &static_gateway);
  nvs_get_str(nvs_handle, "static_dns1", NULL, &static_dns1);
  nvs_get_str(nvs_handle, "static_dns2", NULL, &static_dns2);
  nvs_get_str(nvs_handle, "wifi_ssid", NULL, &wifi_ssid);
  nvs_get_str(nvs_handle, "wifi_password", NULL, &wifi_password);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_ip", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_netmask", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_gateway", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns1", "8.8.8.8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns2", "114.114.114.114"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_password", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'netconn': %s\n", esp_err_to_name(ret));
  } else {
    // 改进内存分配和错误处理
    char *nvs_netconn = NULL;
    char *nvs_is_dhcp = NULL;
    char *nvs_static_ip = NULL;
    char *nvs_static_netmask = NULL;
    char *nvs_static_gateway = NULL;
    char *nvs_static_dns1 = NULL;
    char *nvs_static_dns2 = NULL;
    char *nvs_wifi_ssid = NULL;
    char *nvs_wifi_password = NULL;
    bool alloc_success = true;

    // 逐个分配内存，检查每个分配是否成功
    if ((nvs_netconn = malloc(netconn)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_is_dhcp = malloc(is_dhcp)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_static_ip = malloc(static_ip)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_static_netmask = malloc(static_netmask)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_static_gateway = malloc(static_gateway)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_static_dns1 = malloc(static_dns1)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_static_dns2 = malloc(static_dns2)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_wifi_ssid = malloc(wifi_ssid)) == NULL) alloc_success = false;
    if (alloc_success && (nvs_wifi_password = malloc(wifi_password)) == NULL) alloc_success = false;

    if (!alloc_success) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      nvs_get_str(nvs_handle, "static_ip", nvs_static_ip, &static_ip);
      nvs_get_str(nvs_handle, "static_netmask", nvs_static_netmask,
                  &static_netmask);
      nvs_get_str(nvs_handle, "static_gateway", nvs_static_gateway,
                  &static_gateway);
      nvs_get_str(nvs_handle, "static_dns1", nvs_static_dns1, &static_dns1);
      nvs_get_str(nvs_handle, "static_dns2", nvs_static_dns2, &static_dns2);
      nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
      nvs_get_str(nvs_handle, "wifi_password", nvs_wifi_password,
                  &wifi_password);
      if (ret == ESP_OK) {
        if (atoi(nvs_is_dhcp) == 2) {
          if (strlen(nvs_wifi_ssid) == 0 || strlen(nvs_wifi_password) == 0) {
            printf("wifi_ssid or wifi_password is empty\n");
          } else {
            if (esp_netif_dhcpc_stop(netif) != ESP_OK) {
              ESP_LOGE(TAG, "Failed to stop dhcp client");
            } else {
              esp_netif_ip_info_t ip;
              memset(&ip, 0, sizeof(esp_netif_ip_info_t));
              ip.ip.addr = ipaddr_addr(nvs_static_ip);
              ip.netmask.addr = ipaddr_addr(nvs_static_netmask);
              ip.gw.addr = ipaddr_addr(nvs_static_gateway);
              if (esp_netif_set_ip_info(netif, &ip) != ESP_OK) {
                ESP_LOGE(TAG, "Failed to set ip info");
              } else {
                ESP_LOGD(TAG, "Success to set static ip: %s, netmask: %s, gw: %s",
                        got_ip_addrs, got_ip_netmask, got_ip_gw);

                // 设置主DNS
                if (strlen(nvs_static_dns1) > 0) {
                  esp_netif_dns_info_t dns;
                  dns.ip.u_addr.ip4.addr = ipaddr_addr(nvs_static_dns1);
                  dns.ip.type = IPADDR_TYPE_V4;
                  ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
                  ESP_LOGI(TAG, "Primary DNS set to: %s", nvs_static_dns1);
                } else {
                  // 默认使用谷歌DNS
                  ESP_ERROR_CHECK(example_set_dns_server(
                      netif, ipaddr_addr(EXAMPLE_MAIN_DNS_SERVER), ESP_NETIF_DNS_MAIN));
                }

                // 设置备用DNS
                if (strlen(nvs_static_dns2) > 0) {
                  esp_netif_dns_info_t dns;
                  dns.ip.u_addr.ip4.addr = ipaddr_addr(nvs_static_dns2);
                  dns.ip.type = IPADDR_TYPE_V4;
                  ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &dns));
                  ESP_LOGI(TAG, "Secondary DNS set to: %s", nvs_static_dns2);
                } else {
                  // 默认使用备用DNS
                  ESP_ERROR_CHECK(example_set_dns_server(
                      netif, ipaddr_addr(EXAMPLE_BACKUP_DNS_SERVER),
                      ESP_NETIF_DNS_BACKUP));
                }
              }
            }
          }
        } else {
          printf("sta dhcp start\n");
        }
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
    }

    // 无论成功与否，释放所有已分配的内存
    if (nvs_netconn) free(nvs_netconn);
    if (nvs_is_dhcp) free(nvs_is_dhcp);
    if (nvs_static_ip) free(nvs_static_ip);
    if (nvs_static_netmask) free(nvs_static_netmask);
    if (nvs_static_gateway) free(nvs_static_gateway);
    if (nvs_static_dns1) free(nvs_static_dns1);
    if (nvs_static_dns2) free(nvs_static_dns2);
    if (nvs_wifi_ssid) free(nvs_wifi_ssid);
    if (nvs_wifi_password) free(nvs_wifi_password);
  }

  nvs_close(nvs_handle);
}

// WiFi重连任务
static void wifi_retry_task(void *arg) {
    int check_ap_clients_counter = 0;
    // 重连任务启动时，记录日志
    ESP_LOGI(TAG, "WiFi reconnect task started");

    while (1) {
        // 每10次循环检查一次AP客户端状态
        if (++check_ap_clients_counter >= 10) {
            check_ap_clients_counter = 0;

            // 检查WiFi模式和是否有AP客户端连接
            wifi_mode_t current_mode;
            if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
                if (current_mode == WIFI_MODE_APSTA || current_mode == WIFI_MODE_AP) {
                    // 处于AP模式，检查是否有客户端连接
                    wifi_sta_list_t sta_list;
                    esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);

                    if (err == ESP_OK) {
                        if (sta_list.num == 0 && s_wifi_reconnect_paused) {
                            // 没有客户端连接但重连被暂停，恢复重连
                            resume_wifi_reconnect();
                            ESP_LOGI(TAG, "No AP clients connected, resuming WiFi reconnect");
                        } else if (sta_list.num > 0 && !s_wifi_reconnect_paused) {
                            // 有客户端连接但重连未暂停，暂停重连
                            pause_wifi_reconnect();
                            ESP_LOGI(TAG, "AP clients connected, pausing WiFi reconnect");
                        }
                    }
                } else if (current_mode == WIFI_MODE_STA && s_wifi_reconnect_paused) {
                    // 在纯STA模式下如果重连被暂停，恢复它
                    resume_wifi_reconnect();
                    ESP_LOGI(TAG, "In STA mode but reconnect paused, resuming WiFi reconnect");
                }
            }
        }

        if (s_wifi_connecting && !s_wifi_reconnect_paused) {  // 添加检查，当暂停标志为true时不进行重连
            int64_t current_time = esp_timer_get_time() / 1000; // 转换为毫秒
            int64_t time_since_first_disconnect = current_time - s_first_disconnect_time;
            int64_t time_since_last_retry = current_time - s_last_retry_time;

            // 减少日志输出频率，避免消耗太多栈空间
            static int log_counter = 0;
            if (log_counter++ % 30 == 0) {  // 降低日志输出频率，从10次改为30次
                ESP_LOGI(TAG, "Retry - Time since disconnect: %lld ms, since retry: %lld ms, reconnect_paused=%d",
                        time_since_first_disconnect, time_since_last_retry, s_wifi_reconnect_paused);
            }

            // 检查WiFi连接状态
            wifi_ap_record_t ap_info;
            bool is_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
            if (is_connected) {
                // 如果WiFi已连接，但s_wifi_connecting仍为true，修复状态
                ESP_LOGI(TAG, "WiFi already connected but status flag incorrect, fixing");
                s_wifi_connecting = false;
                s_first_disconnect_time = 0;
                continue;
            }

            // 提高重连等待时间，减轻系统负担
            // 第一个10分钟：每10秒重连一次
            if (time_since_first_disconnect < 10 * MIN_IN_MS) {
                if (time_since_last_retry >= 10 * SECOND_IN_MS) {
                    ESP_LOGI(TAG, "First 10 minutes, reconnecting");
                    // 降低重连优先级，防止阻塞Web服务器
                    vTaskDelay(pdMS_TO_TICKS(100)); // 短暂延迟，让出CPU

                    // 再次检查是否暂停重连，避免在延迟期间状态改变
                    if (!s_wifi_reconnect_paused) {
                        esp_err_t err = esp_wifi_connect();
                        ESP_LOGI(TAG, "Attempting to reconnect, result: %s", esp_err_to_name(err));

                        // 如果连接失败，确保IP地址显示为占位符
                        if (err != ESP_OK) {
                            strcpy(got_ip_addrs, "---");
                            strcpy(got_ip_netmask, "---");
                            strcpy(got_ip_gw, "---");
                            ESP_LOGI(TAG, "Reconnect failed, IP display reset to placeholders");
                        }

                        s_last_retry_time = current_time;
                    } else {
                        ESP_LOGI(TAG, "Reconnect skipped - reconnect paused");
                    }

                    // 重连后增加延迟，减少资源占用
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
            // 10分钟-1小时：每30秒重连一次（从2分钟改为30秒，更快尝试重连）
            else if (time_since_first_disconnect < HOUR_IN_MS) {
                if (time_since_last_retry >= 30 * SECOND_IN_MS) {
                    ESP_LOGI(TAG, "10min-1hour, reconnecting");
                    vTaskDelay(pdMS_TO_TICKS(100));

                    // 再次检查是否暂停重连
                    if (!s_wifi_reconnect_paused) {
                        esp_err_t err = esp_wifi_connect();
                        ESP_LOGI(TAG, "Attempting to reconnect, result: %s", esp_err_to_name(err));
                        s_last_retry_time = current_time;
                    } else {
                        ESP_LOGI(TAG, "Reconnect skipped - reconnect paused");
                    }

                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
            // 1小时后：每2分钟重连一次（从10分钟改为2分钟，更频繁尝试重连）
            else {
                if (time_since_last_retry >= 2 * MIN_IN_MS) {
                    ESP_LOGI(TAG, "After 1 hour, reconnecting every 2 minutes");
                    vTaskDelay(pdMS_TO_TICKS(100));

                    // 再次检查是否暂停重连
                    if (!s_wifi_reconnect_paused) {
                        esp_err_t err = esp_wifi_connect();
                        ESP_LOGI(TAG, "Attempting to reconnect, result: %s", esp_err_to_name(err));
                        s_last_retry_time = current_time;
                    } else {
                        ESP_LOGI(TAG, "Reconnect skipped - reconnect paused");
                    }

                    vTaskDelay(pdMS_TO_TICKS(500));
                }
            }
        } else if (s_wifi_reconnect_paused) {
            // 当重连被暂停时，记录日志（但不要太频繁）
            static int pause_log_counter = 0;
            if (++pause_log_counter % 100 == 0) {
                ESP_LOGI(TAG, "WiFi reconnect paused, connecting=%d", s_wifi_connecting);

                // 检查AP模式下是否还有客户端
                wifi_mode_t current_mode;
                if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
                    if (current_mode == WIFI_MODE_APSTA || current_mode == WIFI_MODE_AP) {
                        wifi_sta_list_t sta_list;
                        esp_err_t err = esp_wifi_ap_get_sta_list(&sta_list);
                        if (err == ESP_OK) {
                            ESP_LOGI(TAG, "AP mode clients: %d", sta_list.num);
                            if (sta_list.num == 0) {
                                // 没有客户端但仍然处于暂停状态，需要恢复重连
                                resume_wifi_reconnect();
                                ESP_LOGI(TAG, "No AP clients but reconnect still paused, resuming");
                            }
                        }
                    } else if (current_mode == WIFI_MODE_STA) {
                        // 在STA模式下不应该暂停重连
                        resume_wifi_reconnect();
                        ESP_LOGI(TAG, "In STA mode but reconnect paused, forcing resume");
                    }
                }
            }
        } else if (!s_wifi_connecting) {
            // WiFi已连接或未请求连接，不需要操作
            static int connected_log_counter = 0;
            if (++connected_log_counter % 300 == 0) {
                ESP_LOGI(TAG, "WiFi not trying to connect (already connected or not requested)");
                // 检查实际连接状态与标志是否一致
                wifi_ap_record_t ap_info;
                bool is_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
                if (!is_connected && is_network_connected) {
                    // 标志不一致，修复它们
                    ESP_LOGW(TAG, "WiFi status inconsistent: flags show connected but WiFi is disconnected");
                    is_network_connected = false;
                    s_wifi_connecting = true;
                    s_first_disconnect_time = esp_timer_get_time() / 1000;
                    s_last_retry_time = s_first_disconnect_time;
                    ESP_LOGI(TAG, "WiFi status flags fixed, will try to reconnect");
                }
            }
        }
        // 延长任务检查间隔，减少CPU使用
        vTaskDelay(pdMS_TO_TICKS(3000)); // 从1秒增加到3秒
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data) {
    esp_err_t ret = nvs_init();
    nvs_handle_t nvs_handle;
    ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
    size_t wifi_flag = 0;
    ret = nvs_get_str(nvs_handle, "wifi_flag", NULL, &wifi_flag);

    // 简化错误处理，减少栈使用
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_flag", "0"));
    }

    char *nvs_wifi_flag = NULL;
    if (ret == ESP_OK) {
        nvs_wifi_flag = malloc(wifi_flag);
        if (nvs_wifi_flag != NULL) {
            ret = nvs_get_str(nvs_handle, "wifi_flag", nvs_wifi_flag, &wifi_flag);
        }
    }

    // WiFi事件处理
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            s_wifi_connecting = true;
            s_last_retry_time = esp_timer_get_time() / 1000;
            esp_wifi_connect();
            ESP_LOGI(TAG, "WiFi started, connecting");
        }
        else if (event_id == WIFI_EVENT_STA_CONNECTED) {
            // 恢复静态IP设置功能
            example_set_static_ip(arg);
        }
        else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            // 不要在主回调中频繁处理，仅设置标志让任务处理

            is_network_connected = false; // 标记网络已断开

            // 在网络断开时将IP地址相关信息设置为占位符
            // 这里只修改显示变量，不改变NVS存储的静态IP配置
            strcpy(got_ip_addrs, "---");
            strcpy(got_ip_netmask, "---");
            strcpy(got_ip_gw, "---");
            ESP_LOGI(TAG, "WiFi disconnected, IP display reset to placeholders");

            ESP_LOGI(TAG, "WiFi断开连接");

            // 确保停止并删除LED闪烁定时器，避免闪烁继续
            extern esp_timer_handle_t time_sync_led_timer;
            if (time_sync_led_timer != NULL) {
                ESP_LOGI(TAG, "WiFi断开：停止并删除LED闪烁定时器");
                esp_err_t stop_err = esp_timer_stop(time_sync_led_timer);
                if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "停止LED闪烁定时器失败: %s", esp_err_to_name(stop_err));
                }

                // 删除定时器并设置为NULL，防止重复创建导致闪烁异常
                esp_err_t delete_err = esp_timer_delete(time_sync_led_timer);
                if (delete_err != ESP_OK) {
                    ESP_LOGW(TAG, "删除LED闪烁定时器失败: %s", esp_err_to_name(delete_err));
                }
                time_sync_led_timer = NULL;

                // LED控制已移除，现在由UART控制
            } else {
                // LED控制已移除，现在由UART控制
            }

            // 确保WiFi重连未被暂停，以允许自动重连
            s_wifi_reconnect_paused = false;

            // 初始化WiFi重连
            if (s_first_disconnect_time == 0) {
                // 只在第一次断开时设置时间
                s_first_disconnect_time = esp_timer_get_time() / 1000;
                s_last_retry_time = s_first_disconnect_time;
                s_wifi_connecting = true;

                // 确保重连任务存在，如果不存在则重新创建
                if (s_wifi_retry_task_handle == NULL) {
                    ESP_LOGI(TAG, "Recreating WiFi retry task");
                    xTaskCreatePinnedToCore(wifi_retry_task, "wifi_retry",
                                            4096, NULL, 4,
                                            &s_wifi_retry_task_handle,
                                            SX_NETWORK_CORE_ID);
                }

                // 为了快速响应，立即尝试一次连接
                esp_wifi_connect();

                ESP_LOGI(TAG, "First disconnect, trying to reconnect immediately and via task");
            } else {
                // 仅设置标志，减少主回调中的处理
                s_wifi_connecting = true;
                ESP_LOGI(TAG, "WiFi disconnected again, reconnect via task");
            }
        }
    }
    // IP事件处理
    else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            s_wifi_connecting = false;
            s_first_disconnect_time = 0;
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(TAG, "IP obtained:" IPSTR, IP2STR(&event->ip_info.ip));
            s_retry_num = 0;
            inet_ntoa_r(event->ip_info.ip, got_ip_addrs, 16);
            inet_ntoa_r(event->ip_info.netmask, got_ip_netmask, 16);
            inet_ntoa_r(event->ip_info.gw, got_ip_gw, 16);

            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            is_network_connected = true; // 标记网络已连接

            // 获取到IP地址后再初始化平台连接定时器和心跳定时器
            // 使用全局声明的变量和函数

            // LED控制已移除，现在由UART控制
            // extern esp_timer_handle_t time_sync_led_timer;
            // extern esp_err_t init_led_timer(void);

            // LED控制已移除，现在由UART控制
            // ESP_LOGI(TAG, "初始化LED闪烁定时器（独立任务模式）");
            // esp_err_t led_timer_err = init_led_timer();
            // if (led_timer_err != ESP_OK) {
            //     ESP_LOGE(TAG, "LED闪烁定时器初始化失败: %s", esp_err_to_name(led_timer_err));
            // } else {
            //     ESP_LOGI(TAG, "LED闪烁定时器已重新启动，LED将开始闪烁");
            // }

            // 初始化平台连接定时器（如果之前没初始化）
            if (platform_timer == NULL) {
                ESP_LOGI(TAG, "初始化平台连接定时器");
                esp_timer_create_args_t platform_timer_args = {
                    .callback = &platform_timer_callback,
                    .name = "platform_timer"
                };
                esp_err_t err = esp_timer_create(&platform_timer_args, &platform_timer);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "无法创建平台连接定时器: %s", esp_err_to_name(err));
                } else {
                    // 启动平台连接定时器，立即开始第一次连接
                    err = esp_timer_start_periodic(platform_timer, 1000 * 1000); // 1000ms
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "无法启动平台连接定时器: %s", esp_err_to_name(err));
                        esp_timer_delete(platform_timer);
                        platform_timer = NULL;
                    } else {
                        ESP_LOGI(TAG, "平台连接定时器已启动");
                    }
                }
            }

            // 初始化心跳定时器（如果之前没初始化）
            if (heartbeat_timer == NULL) {
                ESP_LOGI(TAG, "初始化心跳定时器");
                esp_timer_create_args_t heartbeat_timer_args = {
                    .callback = &heartbeat_timer_callback,
                    .name = "heartbeat_timer"
                };
                esp_err_t err = esp_timer_create(&heartbeat_timer_args, &heartbeat_timer);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "无法创建心跳定时器: %s", esp_err_to_name(err));
                } else {
                    // 等待5秒后再开始发送心跳，确保平台连接已完成
                    err = esp_timer_start_once(heartbeat_timer, 5000 * 1000); // 5000ms后启动第一次心跳
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "无法启动心跳定时器: %s", esp_err_to_name(err));
                        esp_timer_delete(heartbeat_timer);
                        heartbeat_timer = NULL;
                    } else {
                        ESP_LOGI(TAG, "心跳定时器将在5秒后首次触发");
                    }
                }
            }

            if (nvs_wifi_flag != NULL && atoi(nvs_wifi_flag) == 1) {
                ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_flag", "0"));
                esp_restart();
            } else {
                // 确保设置wifi_flag为0
                ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_flag", "0"));

                // 强制更新网络状态，确保心跳能正常工作
                force_network_status_update();

                if (is_network_connected) {
                    ESP_LOGI(TAG, "WiFi网络连接成功 (%s模式)", is_intranet_mode ? "内网" : "外网");
                    // 启动网络服务

                    printf("wifi conn\n");
                    protocol_select();
                } else {
                    ESP_LOGW(TAG, "WiFi已连接但网络不可用");
                }
            }
        }
    }

    if (nvs_wifi_flag != NULL) {
        free(nvs_wifi_flag);
    }
    nvs_close(nvs_handle);
}

// 清理WiFi资源
void wifi_cleanup(void) {
    // 停止重连任务
    if (s_wifi_retry_task_handle != NULL) {
        vTaskDelete(s_wifi_retry_task_handle);
        s_wifi_retry_task_handle = NULL;
    }

    // 注销事件处理程序
    if (s_instance_any_id != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_instance_any_id);
        s_instance_any_id = NULL;
    }
    if (s_instance_got_ip != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_instance_got_ip);
        s_instance_got_ip = NULL;
    }

    // 停止WiFi
    esp_wifi_stop();
    esp_wifi_deinit();

    // 在WiFi停止时，确保IP地址显示为占位符
    strcpy(got_ip_addrs, "---");
    strcpy(got_ip_netmask, "---");
    strcpy(got_ip_gw, "---");
    ESP_LOGI(TAG, "WiFi cleanup: IP display reset to placeholders");

    // 在WiFi清理时将时间设置为占位符
    extern char strftime_buf[64];
    strcpy(strftime_buf, "--:--:--");
    ESP_LOGI(TAG, "WiFi清理：时间显示重置为占位符");

    // 删除事件组
    if (s_wifi_event_group != NULL) {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
}

void wifi_init_sta(const char *w_ssid, const char *w_passwd) {
    printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n");
    printf("w_ssid: %s\n", w_ssid);
    printf("w_passwd: %s\n", w_passwd);

    // 清理之前的WiFi资源（如果有）
    wifi_cleanup();

    // 确保在WiFi连接前将IP地址显示设置为占位符
    strcpy(got_ip_addrs, "---");
    strcpy(got_ip_netmask, "---");
    strcpy(got_ip_gw, "---");
    ESP_LOGI(TAG, "WiFi initialization: IP display set to placeholders");

    // 确保在WiFi初始化时将时间显示设置为占位符
    extern char strftime_buf[64];
    strcpy(strftime_buf, "--:--:--");
    ESP_LOGI(TAG, "WiFi初始化：时间显示设置为占位符");

    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
    assert(sta_netif);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t wifi_ret = esp_wifi_init(&cfg);
    if (wifi_ret) {
        printf("Init0");
    }

    // 增加WiFi重连任务的栈大小和降低优先级
    xTaskCreatePinnedToCore(wifi_retry_task, "wifi_retry", 4096, NULL, 4,
                            &s_wifi_retry_task_handle, SX_NETWORK_CORE_ID);

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, sta_netif,
        &s_instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, sta_netif,
        &s_instance_got_ip));
    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, w_ssid,
            sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, w_passwd,
            sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi_init_sta finished.");

    // 修改为有超时的等待，避免永久阻塞
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                         WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                         pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s", w_ssid, w_passwd);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", w_ssid,
                 w_passwd);
    } else {
        // 超时处理
        ESP_LOGW(TAG, "WiFi connection timeout after 30 seconds");
        ESP_LOGI(TAG, "Continuing with limited functionality...");

        // 设置网络未连接状态
        is_network_connected = false;
        s_wifi_connecting = false;

        // 确保IP显示为占位符
        strcpy(got_ip_addrs, "---");
        strcpy(got_ip_netmask, "---");
        strcpy(got_ip_gw, "---");
    }
}

static void wifi_task(void *pvParameters) {
  wifi_credentials_t *wifi_credentials = (wifi_credentials_t *)pvParameters;
  wifi_init_sta(wifi_credentials->ssid, wifi_credentials->password);

  // 释放内存，避免泄漏
  free(wifi_credentials);

  vTaskDelete(NULL);
}

void start_wifi_task(const char *ssid, const char *password) {
  wifi_credentials_t *wifi_credentials = malloc(sizeof(wifi_credentials_t));
  if (wifi_credentials == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for WiFi credentials");
    return;
  }
  strncpy(wifi_credentials->ssid, ssid, sizeof(wifi_credentials->ssid) - 1);
  strncpy(wifi_credentials->password, password,
          sizeof(wifi_credentials->password) - 1);

  xTaskCreatePinnedToCore(wifi_task, "wifi_task", 4096, wifi_credentials, 8,
                          NULL, SX_NETWORK_CORE_ID);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
