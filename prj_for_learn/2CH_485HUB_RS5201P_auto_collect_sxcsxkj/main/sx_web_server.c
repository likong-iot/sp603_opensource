/*
 * @Author: Orion
 * @Date: 2024-05-23 21:10:47
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-18 17:28:56
 * @Description:
 *
 */
/*
 * @Author: Orion
 * @Date: 2024-01-23 16:09:45
 * @LastEditors: Orion
 * @LastEditTime: 2024-12-24 17:09:43
 * @FilePath: \SERIIAL_SERVER_P\main\sx_web_server.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "sx_web_server.h"
#include "cJSON.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_tls.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/priv/tcp_priv.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_chip_info.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_sleep.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "math.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"
#include "sx_auto_collect.h"
#include "sx_dns_server.h"
#include "sx_http_ota.h"
#include "sx_log.h"
#include "sx_timer_tasks.h"
#include "sx_time_manager.h"




#include "sx_utils.h"
#include "sx_wifi_scan.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <sys/param.h>

EXT_RAM_BSS_ATTR uint8_t sta_mac[6];

EXT_RAM_BSS_ATTR char dIp[20];
EXT_RAM_BSS_ATTR char dNetmask[20];
EXT_RAM_BSS_ATTR char dGateway[20];
EXT_RAM_BSS_ATTR uint8_t mac_addr[6];
EXT_RAM_BSS_ATTR char device_mac[30];

EXT_RAM_BSS_ATTR char th[20];
EXT_RAM_BSS_ATTR char device_mac_end[20];
EXT_RAM_BSS_ATTR char got_ip_addrs[16];
EXT_RAM_BSS_ATTR char got_ip_netmask[16];
EXT_RAM_BSS_ATTR char got_ip_gw[16];
int httpreason = 0;

EXT_RAM_BSS_ATTR char device_sta_mac[24];
EXT_RAM_BSS_ATTR char device_eth_mac[24];
EXT_RAM_BSS_ATTR char device_name[40];

static const char *TAG = "RS5201";

static void *web_psram_calloc(size_t count, size_t size);

static esp_err_t send_json_status(httpd_req_t *req, int code,
                                  const char *message) {
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddNumberToObject(root, "code", code);
  if (message != NULL) {
    cJSON_AddStringToObject(root, "message", message);
  }
  char *json_data = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (json_data == NULL) {
    return ESP_ERR_NO_MEM;
  }
  httpd_resp_set_type(req, "application/json");
  if (code >= 400) {
    httpd_resp_set_status(req, code >= 500 ? "500 Internal Server Error"
                                           : "400 Bad Request");
  }
  esp_err_t ret = httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  return ret;
}

static int hex_digit_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

extern const char root_start[] asm("_binary_root_html_start");
extern const char root_end[] asm("_binary_root_html_end");

extern const char web_start[] asm("_binary_web_html_start");
extern const char web_end[] asm("_binary_web_html_end");

extern const char logo_start[] asm("_binary_logo_png_start");
extern const char logo_end[] asm("_binary_logo_png_end");

extern const char js_start[] asm("_binary_web_js_start");
extern const char js_end[] asm("_binary_web_js_end");

extern const char css_start[] asm("_binary_web_css_start");
extern const char css_end[] asm("_binary_web_css_end");

#define ESP_WIFI_SSID "RS5201"
#define ESP_WIFI_PASS "12345678"
#define MAX_STA_CONN 8

static void apply_ap_config(wifi_config_t *wifi_config, const char *ssid,
                            const char *password) {
  if (wifi_config == NULL || ssid == NULL || password == NULL) {
    return;
  }

  memset(wifi_config, 0, sizeof(*wifi_config));
  snprintf((char *)wifi_config->ap.ssid, sizeof(wifi_config->ap.ssid), "%s",
           ssid);
  snprintf((char *)wifi_config->ap.password,
           sizeof(wifi_config->ap.password), "%s", password);
  wifi_config->ap.ssid_len = strlen((char *)wifi_config->ap.ssid);
  wifi_config->ap.max_connection = MAX_STA_CONN;
  wifi_config->ap.authmode =
      strlen((char *)wifi_config->ap.password) == 0 ? WIFI_AUTH_OPEN
                                                    : WIFI_AUTH_WPA_WPA2_PSK;
}

static esp_err_t start_softap_with_config(const char *ssid,
                                          const char *password) {
  if (ssid == NULL || password == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (strlen(ssid) >= sizeof(((wifi_config_t *)0)->ap.ssid) ||
      strlen(password) >= sizeof(((wifi_config_t *)0)->ap.password)) {
    ESP_LOGE(TAG, "AP config too long, ssid_len=%zu password_len=%zu",
             strlen(ssid), strlen(password));
    return ESP_ERR_INVALID_SIZE;
  }

  wifi_config_t wifi_config;
  apply_ap_config(&wifi_config, ssid, password);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  esp_netif_ip_info_t ip_info;
  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                        &ip_info);

  char ip_addr[16];
  inet_ntoa_r(ip_info.ip.addr, ip_addr, sizeof(ip_addr));
  ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);
  ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s' password:'%s'", ssid,
           password);
  return ESP_OK;
}

httpd_handle_t http_server = NULL;

// 添加WebSocket相关变量
// static httpd_handle_t websocket_server = NULL;

// 定义常量和全局变量
#define MAX_WS_CLIENTS 4   // 限制最大WebSocket客户端数量
#define WS_QUEUE_SIZE 8    // WebSocket消息队列大小 - 减少队列数量但保持消息大小
#define WS_MAX_MSG_LEN 10240 // 最大消息长度 - 扩展到10KB以支持更大UART包显示

typedef struct {
  int client_fd;
  bool in_use;
} ws_client_t;

static ws_client_t *ws_clients = NULL;
static QueueHandle_t ws_msg_queue = NULL;
static TaskHandle_t ws_task_handle = NULL;
static StaticQueue_t *ws_queue_struct = NULL;
static uint8_t *ws_queue_storage = NULL;
static char *ws_msg_buffer = NULL;



// static void disconnect_handler(void *arg, esp_event_base_t event_base,
//                                int32_t event_id, void *event_data);

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  if (event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
             event->aid);
  } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
             event->aid);
  }
}

static void wifi_init_softap(void) {
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_netif_create_default_wifi_ap();

  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                             &wifi_event_handler, NULL));

  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);

  ESP_LOGI(TAG, "ESP_MAC_WIFI_STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
           sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4],
           sta_mac[5]);
  sprintf(device_mac, "%02X%02X%02X%02X%02X%02X", sta_mac[0], sta_mac[1],
          sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(device_mac_end, "%02X%02X", sta_mac[4], sta_mac[5]);

  // esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);

  sprintf(device_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0],
          sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);


  char ap_ssid[80] = {0};
  snprintf(ap_ssid, sizeof(ap_ssid), "%s_%s", ESP_WIFI_SSID, device_mac_end);

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  size_t ap_name = 0;
  size_t ap_password = 0;
  size_t ap_wait_time = 0;
  esp_err_t name_ret = nvs_get_str(nvs_handle, "ap_name", NULL, &ap_name);
  esp_err_t password_ret =
      nvs_get_str(nvs_handle, "ap_password", NULL, &ap_password);
  esp_err_t wait_ret =
      nvs_get_str(nvs_handle, "ap_wait_time", NULL, &ap_wait_time);

  if (name_ret == ESP_OK && password_ret == ESP_OK && wait_ret == ESP_OK) {
    printf("ap_name set success");
  } else if (name_ret == ESP_ERR_NVS_NOT_FOUND ||
             password_ret == ESP_ERR_NVS_NOT_FOUND ||
             wait_ret == ESP_ERR_NVS_NOT_FOUND) {
    printf("no ap_name set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_name", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_password", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_wait_time", ""));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    if (start_softap_with_config(ap_ssid, ESP_WIFI_PASS) != ESP_OK) {
      ESP_ERROR_CHECK(start_softap_with_config(ESP_WIFI_SSID, ESP_WIFI_PASS));
    }
    nvs_close(nvs_handle);
    return;
  }

  if (name_ret != ESP_OK || password_ret != ESP_OK || wait_ret != ESP_OK) {
    ESP_LOGE(TAG, "Error getting AP config size: name=%s password=%s wait=%s",
             esp_err_to_name(name_ret), esp_err_to_name(password_ret),
             esp_err_to_name(wait_ret));
    if (start_softap_with_config(ap_ssid, ESP_WIFI_PASS) != ESP_OK) {
      ESP_ERROR_CHECK(start_softap_with_config(ESP_WIFI_SSID, ESP_WIFI_PASS));
    }
  } else {
    // 分配缓冲区
    char *nvs_ap_name = malloc(ap_name);
    char *nvs_ap_password = malloc(ap_password);
    char *nvs_ap_wait_time = malloc(ap_wait_time);
    if (nvs_ap_name == NULL || nvs_ap_password == NULL ||
        nvs_ap_wait_time == NULL) {
      printf("Memory allocation failed\n");
      if (start_softap_with_config(ap_ssid, ESP_WIFI_PASS) != ESP_OK) {
        ESP_ERROR_CHECK(start_softap_with_config(ESP_WIFI_SSID, ESP_WIFI_PASS));
      }
    } else {
      ret = nvs_get_str(nvs_handle, "ap_name", nvs_ap_name, &ap_name);
      password_ret =
          nvs_get_str(nvs_handle, "ap_password", nvs_ap_password, &ap_password);
      wait_ret = nvs_get_str(nvs_handle, "ap_wait_time", nvs_ap_wait_time,
                             &ap_wait_time);
      if (ret == ESP_OK && password_ret == ESP_OK && wait_ret == ESP_OK) {
        printf("Value for 'ap_name' is %s\n", nvs_ap_name);
        printf("Value for 'ap_password' is %s\n", nvs_ap_password);
        printf("Value for 'ap_wait_time' is %s\n", nvs_ap_wait_time);
        if (strlen(nvs_ap_name) == 0 && strlen(nvs_ap_password) == 0) {
          if (start_softap_with_config(ap_ssid, ESP_WIFI_PASS) != ESP_OK) {
            ESP_ERROR_CHECK(
                start_softap_with_config(ESP_WIFI_SSID, ESP_WIFI_PASS));
          }
        } else {
          if (start_softap_with_config(nvs_ap_name, nvs_ap_password) !=
              ESP_OK) {
            ESP_LOGW(TAG, "Invalid NVS AP config, fallback to default AP");
            if (start_softap_with_config(ap_ssid, ESP_WIFI_PASS) != ESP_OK) {
              ESP_ERROR_CHECK(
                  start_softap_with_config(ESP_WIFI_SSID, ESP_WIFI_PASS));
            }
          }
        }

      } else {
        ESP_LOGE(TAG, "Error reading AP config: name=%s password=%s wait=%s",
                 esp_err_to_name(ret), esp_err_to_name(password_ret),
                 esp_err_to_name(wait_ret));
        if (start_softap_with_config(ap_ssid, ESP_WIFI_PASS) != ESP_OK) {
          ESP_ERROR_CHECK(start_softap_with_config(ESP_WIFI_SSID, ESP_WIFI_PASS));
        }
      }
    }
    free(nvs_ap_name);
    free(nvs_ap_password);
    free(nvs_ap_wait_time);
  }

  nvs_close(nvs_handle);
}

static esp_err_t get_devinfo_get_handler(httpd_req_t *req) {

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t host_names, ntp_server, is_dhcp, netconn = 0;
  ret = nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server);
  nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);
  nvs_get_str(nvs_handle, "netconn", NULL, &netconn);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    printf("no ip set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "以太网两路缓存485集线器"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ntp_server", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", ""));
    break;
  default:
    break;
  }
  ret = nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", "2")); // 默认使用WiFi
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'host_names': %s\n", esp_err_to_name(ret));
  } else {
    // 分配缓冲区
    char *nvs_host_names = malloc(host_names);
    char *nvs_ntp_server = malloc(ntp_server);
    char *nvs_is_dhcp = malloc(is_dhcp);
    char *nvs_netconn = malloc(netconn);
    if (nvs_host_names == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "host_names", nvs_host_names, &host_names);
      nvs_get_str(nvs_handle, "ntp_server", nvs_ntp_server, &ntp_server);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);
      if (ret == ESP_OK) {

        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "device_sta_mac",
                              cJSON_CreateString(device_sta_mac));
        

        cJSON_AddItemToObject(root, "netconn", cJSON_CreateString(nvs_netconn));
        // cJSON_AddItemToObject(root, "current_time",
        //                       cJSON_CreateString(strftime_buf));
        // printf("11111当前时间是: %s\n", strftime_buf);
        // printf("nvs_netconn: %s\n", nvs_netconn);
        cJSON_AddItemToObject(root, "sta_ip",
                              cJSON_CreateString(got_ip_addrs));
        cJSON_AddItemToObject(root, "is_dhcp", cJSON_CreateString(nvs_is_dhcp));
        // 添加系统运行时间
        int64_t time_since_boot = esp_timer_get_time();
        cJSON_AddItemToObject(root, "uptime", cJSON_CreateNumber(time_since_boot));
        
        cJSON_AddItemToObject(root, "host_names",
                              cJSON_CreateString(nvs_host_names));
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        // char *json_data = cJSON_Print(root);
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));

        free(json_data);
        cJSON_Delete(root);

        // get_current_time();
        // esp_netif_sntp_deinit();

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
    }
    free(nvs_host_names);
    free(nvs_ntp_server);
    free(nvs_is_dhcp);
    free(nvs_netconn);
  }

  nvs_close(nvs_handle);

  return ESP_OK;
}

static esp_err_t get_sys_get_handler(httpd_req_t *req) {
  // 统计内部RAM堆和PSRAM堆，分开展示
  size_t free_heap_internal_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t min_heap_internal_bytes =
      heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
  size_t free_heap_psram_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t min_heap_psram_bytes =
      heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);

  float free_heap_internal_kb = free_heap_internal_bytes / 1024.0f;
  float min_heap_internal_kb = min_heap_internal_bytes / 1024.0f;
  float free_heap_psram_kb = free_heap_psram_bytes / 1024.0f;
  float min_heap_psram_kb = min_heap_psram_bytes / 1024.0f;
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  int64_t time_since_boot = esp_timer_get_time();
  esp_reset_reason_t res_reason = esp_reset_reason();

  // 获取当前活跃套接字数量
  int socket_count = 0;
  struct tcp_pcb *pcb = tcp_active_pcbs;
  while (pcb != NULL) {
    socket_count++;
    pcb = pcb->next;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "chip_info_cpu_core",
                        cJSON_CreateNumber(chip_info.cores));
  cJSON_AddItemToObject(root, "wifi_frq", cJSON_CreateNumber(2.4));
  cJSON_AddItemToObject(root, "free_heap_internal_kb",
                        cJSON_CreateNumber(free_heap_internal_kb));
  cJSON_AddItemToObject(root, "min_heap_internal_kb",
                        cJSON_CreateNumber(min_heap_internal_kb));
  cJSON_AddItemToObject(root, "free_heap_psram_kb",
                        cJSON_CreateNumber(free_heap_psram_kb));
  cJSON_AddItemToObject(root, "min_heap_psram_kb",
                        cJSON_CreateNumber(min_heap_psram_kb));
  // 兼容旧字段（内部RAM数值）
  cJSON_AddItemToObject(root, "free_heap_size",
                        cJSON_CreateNumber(free_heap_internal_kb));
  cJSON_AddItemToObject(root, "min_ever_free_heap_size",
                        cJSON_CreateNumber(min_heap_internal_kb));
  cJSON_AddItemToObject(root, "time_since_boot",
                        cJSON_CreateNumber(time_since_boot));
  cJSON_AddItemToObject(root, "res_reason",
                        cJSON_CreateString(reset_reason_to_string(res_reason)));
  cJSON_AddItemToObject(root, "version", cJSON_CreateString(VERSION));
  cJSON_AddItemToObject(root, "active_sockets",
                        cJSON_CreateNumber(socket_count));
  char *json_data = cJSON_Print(root);
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  const uint32_t root_len = root_end - root_start;
  ESP_LOGI(TAG, "Serve root from: %s", req->uri);
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, root_start, root_len);
  return ESP_OK;
}

static esp_err_t get_status_get_handler(httpd_req_t *req) {
  const uint32_t web_len = web_end - web_start;
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_send(req, web_start, web_len);
  return ESP_OK;
}

static esp_err_t js_get_handler(httpd_req_t *req) {
  const uint32_t js_len = js_end - js_start;
  ESP_LOGI(TAG, "Serve js");
  httpd_resp_set_type(req, "application/javascript; charset=utf-8");
  httpd_resp_send(req, js_start, js_len);
  return ESP_OK;
}
static esp_err_t css_get_handler(httpd_req_t *req) {
  const uint32_t css_len = css_end - css_start;
  httpd_resp_set_type(req, "text/css; charset=utf-8");
  httpd_resp_send(req, css_start, css_len);
  return ESP_OK;
}

static esp_err_t post_handler(httpd_req_t *req) {
  char *buf = malloc(req->content_len + 1);
  if (buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for request data");
    return ESP_ERR_NO_MEM;
  }
  
  int ret, remaining = req->content_len;
  int total_received = 0;
  
  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf + total_received, remaining)) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      free(buf);
      return ESP_FAIL;
    }
    remaining -= ret;
    total_received += ret;
  }
  
  buf[total_received] = '\0';  // 确保字符串结束
  ESP_LOGI(TAG, "Received data length: %d", total_received);
  ESP_LOGI(TAG, "Received data: %s", buf);
  
  ESP_ERROR_CHECK(nvs_init());
  esp_err_t save_result = save_to_nvs(buf);
  
  // 检查是否包含ap_timeout配置，如果是则重新加载配置
  if (save_result == ESP_OK && strstr(buf, "ap_timeout") != NULL) {
    reload_ap_timeout_config();
    ESP_LOGI(TAG, "检测到AP超时配置更新，已重新加载配置");
  }
  
  free(buf);
  
  cJSON *root = cJSON_CreateObject();
  if (save_result == ESP_OK) {
    cJSON_AddItemToObject(root, "msg", cJSON_CreateString("success"));
    cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(200));
  } else {
    ESP_LOGE(TAG, "Failed to save configuration: %s", esp_err_to_name(save_result));
    cJSON_AddItemToObject(root, "msg", cJSON_CreateString("Configuration save failed"));
    cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(500));
  }
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(root);
  httpd_resp_send(req, json_data, strlen(json_data));

  free(json_data);
  cJSON_Delete(root);
  return ESP_OK;
}



static esp_err_t get_net_set_post_handler(httpd_req_t *req) {
  post_handler(req);
  return ESP_OK;
}

static esp_err_t get_module_set_post_handler(httpd_req_t *req) {
  post_handler(req);
  return ESP_OK;
}

// 时间同步API处理函数
static esp_err_t sync_time_handler(httpd_req_t *req) {
    char buffer[128];
    int ret = httpd_req_recv(req, buffer, sizeof(buffer) - 1);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buffer[ret] = '\0';

    // 解析JSON
    cJSON *root = cJSON_Parse(buffer);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *timestamp = cJSON_GetObjectItem(root, "timestamp");
    if (!cJSON_IsNumber(timestamp)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing timestamp");
        return ESP_FAIL;
    }

    uint64_t browser_timestamp_ms = (uint64_t)timestamp->valuedouble;
    cJSON_Delete(root);

    // 同步时间
    esp_err_t err = time_manager_sync_from_browser(browser_timestamp_ms);
    
    // 构建响应
    cJSON *response = cJSON_CreateObject();
    cJSON_AddBoolToObject(response, "success", err == ESP_OK);
    cJSON_AddNumberToObject(response, "current_time", (double)time_manager_get_current_ms());
    cJSON_AddBoolToObject(response, "synced", time_manager_is_synced());

    char *json_str = cJSON_Print(response);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json_str);

    free(json_str);
    cJSON_Delete(response);
    
    return ESP_OK;
}

static esp_err_t get_ap_set_post_handler(httpd_req_t *req) {
  post_handler(req);
  return ESP_OK;
}

static bool serial_json_int_in_range(cJSON *json, const char *name,
                                     int min_value, int max_value,
                                     int *out_value) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(json, name);
  if (!cJSON_IsString(item) || item->valuestring == NULL ||
      item->valuestring[0] == '\0') {
    return false;
  }
  char *end = NULL;
  long value = strtol(item->valuestring, &end, 10);
  if (end == item->valuestring || *end != '\0' ||
      value < min_value || value > max_value) {
    return false;
  }
  if (out_value != NULL) {
    *out_value = (int)value;
  }
  return true;
}

static bool validate_serial_config_json(cJSON *json, const char **error_msg) {
  cJSON *port = cJSON_GetObjectItemCaseSensitive(json, "serial_port");
  int baudrate = 0;
  int data_bits = 0;
  int parity = 0;

  if (!cJSON_IsNumber(port) || port->valueint < 1 || port->valueint > 3) {
    *error_msg = "invalid serial_port";
    return false;
  }
  if (!serial_json_int_in_range(json, "baud_rate", 300, 2000000,
                                &baudrate)) {
    *error_msg = "invalid baud_rate";
    return false;
  }
  if (!serial_json_int_in_range(json, "data_bit", 5, 8, &data_bits)) {
    *error_msg = "invalid data_bit";
    return false;
  }
  if (!serial_json_int_in_range(json, "check_bit", 0, 2, &parity)) {
    *error_msg = "invalid check_bit";
    return false;
  }

  cJSON *stop = cJSON_GetObjectItemCaseSensitive(json, "stop_bit");
  if (!cJSON_IsString(stop) || stop->valuestring == NULL ||
      (strcmp(stop->valuestring, "1") != 0 &&
       strcmp(stop->valuestring, "1.5") != 0 &&
       strcmp(stop->valuestring, "2") != 0)) {
    *error_msg = "invalid stop_bit";
    return false;
  }
  if (!serial_json_int_in_range(json, "frame_time", 1, 1000, NULL)) {
    *error_msg = "invalid frame_time";
    return false;
  }
  if (!serial_json_int_in_range(json, "frame_len", 1, 8192, NULL)) {
    *error_msg = "invalid frame_len";
    return false;
  }
  if (!serial_json_int_in_range(json, "reply_timeout", 1, 60000, NULL)) {
    *error_msg = "invalid reply_timeout";
    return false;
  }

  (void)baudrate;
  (void)data_bits;
  (void)parity;
  return true;
}

static esp_err_t get_serial_set_handler(httpd_req_t *req) {

  char *buf = malloc(req->content_len + 1);
  if (buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate memory for serial request data");
    return ESP_ERR_NO_MEM;
  }
  
  int ret, remaining = req->content_len;
  int total_received = 0;
  
  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf + total_received, remaining)) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      free(buf);
      return ESP_FAIL;
    }
    remaining -= ret;
    total_received += ret;
  }
  
  buf[total_received] = '\0';  // 确保字符串结束
  ESP_LOGI(TAG, "Received serial data: %s", buf);

  cJSON *serial_json = cJSON_Parse(buf);
  const char *validation_error = "invalid serial config";
  if (serial_json == NULL ||
      !validate_serial_config_json(serial_json, &validation_error)) {
    if (serial_json != NULL) {
      cJSON_Delete(serial_json);
    }
    free(buf);
    return send_json_status(req, 400, validation_error);
  }
  cJSON *defer_item =
      cJSON_GetObjectItemCaseSensitive(serial_json, "defer_apply");
  bool defer_apply = cJSON_IsBool(defer_item) && cJSON_IsTrue(defer_item);
  cJSON_Delete(serial_json);
  
  ESP_ERROR_CHECK(nvs_init());
  esp_err_t save_result = save_to_nvs(buf);
  free(buf);
  
  if (save_result != ESP_OK) {
    ESP_LOGE(TAG, "Failed to save serial configuration: %s", esp_err_to_name(save_result));
    // 返回错误响应
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "msg", cJSON_CreateString("Serial configuration save failed"));
    cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(500));
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_set_type(req, "application/json");
    char *json_data = cJSON_Print(root);
    httpd_resp_send(req, json_data, strlen(json_data));
    free(json_data);
    cJSON_Delete(root);
    return ESP_OK;
  }

  // 批量保存前两个通道时只写NVS，最后一个请求再统一重配三路UART。
  if (defer_apply) {
    return send_json_status(req, 200, "saved");
  }

  // 读取三个通道的配置并重新初始化UART
  nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  
  // 为三个通道分别读取配置
  int baudrates[3] = {9600, 9600, 9600};
  int data_bits[3] = {8, 8, 8};
  uart_parity_t parities[3] = {UART_PARITY_DISABLE, UART_PARITY_DISABLE, UART_PARITY_DISABLE};
  uart_stop_bits_t stop_bits[3] = {UART_STOP_BITS_1, UART_STOP_BITS_1, UART_STOP_BITS_1};
  int frame_times[3] = {50, 50, 50};
  int frame_lens[3] = {512, 512, 512};
  int reply_timeouts[3] = {1000, 1000, 1000};
  
  // 读取通道1配置
  size_t size = 0;
  char *value = NULL;
  
  // 通道1
  if (nvs_get_str(nvs_handle, "ch1_baud_rate", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_baud_rate", value, &size) == ESP_OK) {
      baudrates[0] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch1_data_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_data_bit", value, &size) == ESP_OK) {
      data_bits[0] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch1_check_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_check_bit", value, &size) == ESP_OK) {
      // 支持数字字符串格式（前端实际发送的格式）
      if (strcmp(value, "0") == 0) {
        parities[0] = UART_PARITY_DISABLE;
      } else if (strcmp(value, "1") == 0) {
        parities[0] = UART_PARITY_ODD;
      } else if (strcmp(value, "2") == 0) {
        parities[0] = UART_PARITY_EVEN;
      // 移除旧的字符串格式兼容性，统一使用数字格式
      } else {
        parities[0] = UART_PARITY_DISABLE;
      }
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch1_stop_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_stop_bit", value, &size) == ESP_OK) {
      if (strcmp(value, "1.5") == 0) {
        stop_bits[0] = UART_STOP_BITS_1_5;
      } else if (strcmp(value, "2") == 0) {
        stop_bits[0] = UART_STOP_BITS_2;
      } else {
        stop_bits[0] = UART_STOP_BITS_1;
      }
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch1_frame_time", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_frame_time", value, &size) == ESP_OK) {
      frame_times[0] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch1_frame_len", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_frame_len", value, &size) == ESP_OK) {
      frame_lens[0] = atoi(value);
    }
    free(value);
  }

  if (nvs_get_str(nvs_handle, "ch1_timeout", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch1_timeout", value, &size) == ESP_OK) {
      reply_timeouts[0] = atoi(value);
    }
    free(value);
  }
  
  // 通道2
  if (nvs_get_str(nvs_handle, "ch2_baud_rate", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_baud_rate", value, &size) == ESP_OK) {
      baudrates[1] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch2_data_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_data_bit", value, &size) == ESP_OK) {
      data_bits[1] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch2_check_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_check_bit", value, &size) == ESP_OK) {
      // 支持数字字符串格式（前端实际发送的格式）
      if (strcmp(value, "0") == 0) {
        parities[1] = UART_PARITY_DISABLE;
      } else if (strcmp(value, "1") == 0) {
        parities[1] = UART_PARITY_ODD;
      } else if (strcmp(value, "2") == 0) {
        parities[1] = UART_PARITY_EVEN;
      // 移除旧的字符串格式兼容性，统一使用数字格式
      } else {
        parities[1] = UART_PARITY_DISABLE;
      }
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch2_stop_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_stop_bit", value, &size) == ESP_OK) {
      if (strcmp(value, "1.5") == 0) {
        stop_bits[1] = UART_STOP_BITS_1_5;
      } else if (strcmp(value, "2") == 0) {
        stop_bits[1] = UART_STOP_BITS_2;
      } else {
        stop_bits[1] = UART_STOP_BITS_1;
      }
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch2_frame_time", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_frame_time", value, &size) == ESP_OK) {
      frame_times[1] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch2_frame_len", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_frame_len", value, &size) == ESP_OK) {
      frame_lens[1] = atoi(value);
    }
    free(value);
  }

  if (nvs_get_str(nvs_handle, "ch2_timeout", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch2_timeout", value, &size) == ESP_OK) {
      reply_timeouts[1] = atoi(value);
    }
    free(value);
  }
  
  // 通道3
  if (nvs_get_str(nvs_handle, "ch3_baud_rate", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_baud_rate", value, &size) == ESP_OK) {
      baudrates[2] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch3_data_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_data_bit", value, &size) == ESP_OK) {
      data_bits[2] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch3_check_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_check_bit", value, &size) == ESP_OK) {
      // 支持数字字符串格式（前端实际发送的格式）
      if (strcmp(value, "0") == 0) {
        parities[2] = UART_PARITY_DISABLE;
      } else if (strcmp(value, "1") == 0) {
        parities[2] = UART_PARITY_ODD;
      } else if (strcmp(value, "2") == 0) {
        parities[2] = UART_PARITY_EVEN;
      // 移除旧的字符串格式兼容性，统一使用数字格式
      } else {
        parities[2] = UART_PARITY_DISABLE;
      }
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch3_stop_bit", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_stop_bit", value, &size) == ESP_OK) {
      if (strcmp(value, "1.5") == 0) {
        stop_bits[2] = UART_STOP_BITS_1_5;
      } else if (strcmp(value, "2") == 0) {
        stop_bits[2] = UART_STOP_BITS_2;
      } else {
        stop_bits[2] = UART_STOP_BITS_1;
      }
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch3_frame_time", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_frame_time", value, &size) == ESP_OK) {
      frame_times[2] = atoi(value);
    }
    free(value);
  }
  
  if (nvs_get_str(nvs_handle, "ch3_frame_len", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_frame_len", value, &size) == ESP_OK) {
      frame_lens[2] = atoi(value);
    }
    free(value);
  }

  if (nvs_get_str(nvs_handle, "ch3_timeout", NULL, &size) == ESP_OK) {
    value = malloc(size);
    if (value && nvs_get_str(nvs_handle, "ch3_timeout", value, &size) == ESP_OK) {
      reply_timeouts[2] = atoi(value);
    }
    free(value);
  }
  
  // 在关闭NVS之前，读取UART配置模式
  char serial_sync_mode[32] = {0};
  size_t sync_mode_size = sizeof(serial_sync_mode);
  esp_err_t sync_mode_ret = nvs_get_str(nvs_handle, "serialSyncMode", serial_sync_mode, &sync_mode_size);
  
  // 如果读取失败，尝试读取旧的serial_mode配置
  if (sync_mode_ret != ESP_OK) {
    char serial_mode[32] = {0};
    size_t mode_size = sizeof(serial_mode);
    esp_err_t mode_ret = nvs_get_str(nvs_handle, "serial_mode", serial_mode, &mode_size);
    if (mode_ret == ESP_OK) {
      strncpy(serial_sync_mode, serial_mode, sizeof(serial_sync_mode) - 1);
      sync_mode_ret = ESP_OK;
    }
  }
  
  nvs_close(nvs_handle);
  
  // 转换数据位格式
  uart_word_length_t uart_data_bits[3];
  for (int i = 0; i < 3; i++) {
    switch (data_bits[i]) {
      case 5: uart_data_bits[i] = UART_DATA_5_BITS; break;
      case 6: uart_data_bits[i] = UART_DATA_6_BITS; break;
      case 7: uart_data_bits[i] = UART_DATA_7_BITS; break;
      default: uart_data_bits[i] = UART_DATA_8_BITS; break;
    }
  }
  
  // 使用三个通道的独立配置重新初始化UART
  // 按当前绑定：CH1->UART2, CH2->UART0, CH3->UART1
  // uart_reinit函数参数顺序：(ch1_params, ch2_params, ch3_params)
  uart_reinit(baudrates[0], uart_data_bits[0], parities[0], stop_bits[0], frame_times[0], frame_lens[0], reply_timeouts[0],  // ch1配置给UART2
              baudrates[1], uart_data_bits[1], parities[1], stop_bits[1], frame_times[1], frame_lens[1], reply_timeouts[1],  // ch2配置给UART0
              baudrates[2], uart_data_bits[2], parities[2], stop_bits[2], frame_times[2], frame_lens[2], reply_timeouts[2]); // ch3配置给UART1

  // UART配置更新后，立即更新UART配置模式标志
  ESP_LOGI(TAG, "UART配置更新完成，立即更新UART配置模式");
  if (sync_mode_ret == ESP_OK) {
    ESP_LOGI(TAG, "读取到串口同步模式: %s", serial_sync_mode);
    if (strcmp(serial_sync_mode, "slave_follow") == 0) {
      set_uart_config_mode(UART_CONFIG_MODE_SLAVE_FOLLOW);
      ESP_LOGI(TAG, "立即更新UART配置模式为：从机跟随模式");
    } else {
      set_uart_config_mode(UART_CONFIG_MODE_NORMAL);
      ESP_LOGI(TAG, "立即更新UART配置模式为：正常模式");
    }
  } else {
    set_uart_config_mode(UART_CONFIG_MODE_NORMAL);
    ESP_LOGI(TAG, "未找到串口同步模式配置，设置为默认：正常模式");
  }

  // 返回成功响应
  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "msg", cJSON_CreateString("success"));
  cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(200));
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(root);
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  cJSON_Delete(root);

  return ESP_OK;
}

static esp_err_t get_serial_ctl_handler(httpd_req_t *req) {
  const int max_body_len = 4096;
  int remaining = req->content_len;
  int received = 0;
  char *buf = NULL;
  char *clean_str = NULL;
  uint8_t *hex_data = NULL;
  cJSON *json = NULL;
  esp_err_t result = ESP_FAIL;
  int response_code = 200;
  const char *response_msg = "ok";
  char response_msg_buf[64] = {0};

  if (remaining <= 0 || remaining > max_body_len) {
    return send_json_status(req, 400, "invalid body length");
  }

  buf = calloc((size_t)remaining + 1U, 1);
  if (buf == NULL) {
    return send_json_status(req, 500, "no memory");
  }

  while (received < remaining) {
    int ret = httpd_req_recv(req, buf + received, remaining - received);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      response_code = 500;
      response_msg = "receive failed";
      goto cleanup;
    }
    received += ret;
  }
  buf[received] = '\0';
  ESP_LOGI(TAG, "%s", buf);

  json = cJSON_Parse(buf);
  if (json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      ESP_LOGE("JSON", "Error before: %s", error_ptr);
    }
    response_code = 400;
    response_msg = "invalid json";
    goto cleanup;
  }

  cJSON *instruction = cJSON_GetObjectItemCaseSensitive(json, "instruction");
  cJSON *sendType = cJSON_GetObjectItemCaseSensitive(json, "sendType");
  cJSON *channel = cJSON_GetObjectItemCaseSensitive(json, "channel");

  if (!cJSON_IsString(instruction) || instruction->valuestring == NULL) {
    response_code = 400;
    response_msg = "invalid instruction";
    goto cleanup;
  }

  int target_channel = 3;
  if (cJSON_IsNumber(channel)) {
    target_channel = channel->valueint;
  }
  if (target_channel < 1 || target_channel > 3) {
    response_code = 400;
    response_msg = "invalid channel";
    goto cleanup;
  }

  if (cJSON_IsString(sendType) && strcmp(sendType->valuestring, "hex") == 0) {
    size_t len = strlen(instruction->valuestring);
    clean_str = malloc(len + 1U);
    if (clean_str == NULL) {
      response_code = 500;
      response_msg = "no memory";
      goto cleanup;
    }

    size_t clean_len = 0;
    for (size_t i = 0; i < len; i++) {
      unsigned char ch = (unsigned char)instruction->valuestring[i];
      if (isspace(ch)) {
        continue;
      }
      if (hex_digit_value((char)ch) < 0) {
        response_code = 400;
        response_msg = "invalid hex character";
        goto cleanup;
      }
      clean_str[clean_len++] = (char)ch;
    }
    clean_str[clean_len] = '\0';

    if (clean_len == 0 || (clean_len % 2U) != 0U) {
      response_code = 400;
      response_msg = "invalid hex length";
      goto cleanup;
    }

    size_t hex_len = clean_len / 2U;
    if (hex_len > UART_TRACE_BUFFER_SIZE) {
      response_code = 400;
      response_msg = "data too long";
      goto cleanup;
    }

    hex_data = malloc(hex_len);
    if (hex_data == NULL) {
      response_code = 500;
      response_msg = "no memory";
      goto cleanup;
    }

    for (size_t i = 0; i < clean_len; i += 2U) {
      int high = hex_digit_value(clean_str[i]);
      int low = hex_digit_value(clean_str[i + 1U]);
      hex_data[i / 2U] = (uint8_t)((high << 4) | low);
    }

    result = tx_tasks_to_channel(hex_data, hex_len, target_channel);
  } else {
    size_t ascii_len = strlen(instruction->valuestring);
    if (ascii_len == 0 || ascii_len > UART_TRACE_BUFFER_SIZE) {
      response_code = 400;
      response_msg = "invalid data length";
      goto cleanup;
    }
    result = tx_tasks_to_channel((uint8_t *)instruction->valuestring,
                                 ascii_len, target_channel);
  }

  if (result != ESP_OK) {
    response_code = 500;
    snprintf(response_msg_buf, sizeof(response_msg_buf), "send failed: %s",
             esp_err_to_name(result));
    response_msg = response_msg_buf;
    goto cleanup;
  }

cleanup:
  if (hex_data != NULL) {
    free(hex_data);
  }
  if (clean_str != NULL) {
    free(clean_str);
  }
  if (json != NULL) {
    cJSON_Delete(json);
  }
  if (buf != NULL) {
    free(buf);
  }
  return send_json_status(req, response_code, response_msg);
}

static esp_err_t get_uart_response_handler(httpd_req_t *req) {
    cJSON *root = NULL;
    char *json_response = NULL;
    char *tx_hex_str = NULL;
    char *tx_ascii_str = NULL;
    char *rx_hex_str = NULL;
    char *rx_ascii_str = NULL;
    bool has_data = false;

    // 创建 JSON 对象
    root = cJSON_CreateObject();
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to create JSON object");
        return ESP_FAIL;
    }

    tx_hex_str = web_psram_calloc(1, 2048);
    tx_ascii_str = web_psram_calloc(1, 1024);
    rx_hex_str = web_psram_calloc(1, 2048);
    rx_ascii_str = web_psram_calloc(1, 1024);
    if (tx_hex_str == NULL || tx_ascii_str == NULL || rx_hex_str == NULL ||
        rx_ascii_str == NULL) {
        ESP_LOGE(TAG, "Failed to allocate UART response buffers");
        cJSON_Delete(root);
        free(tx_hex_str);
        free(tx_ascii_str);
        free(rx_hex_str);
        free(rx_ascii_str);
        return ESP_ERR_NO_MEM;
    }

    // 获取发送的数据
    portENTER_CRITICAL(&uart_spinlock);
    if (uart_tx_data != NULL && tx_data_len > 0) {
        size_t hex_pos = 0;

        // 安全地构建十六进制字符串
        for (int i = 0; i < tx_data_len && hex_pos < 2048 - 3; i++) {
            hex_pos += snprintf(tx_hex_str + hex_pos, 2048 - hex_pos,
                              "%02X ", uart_tx_data[i]);
        }

        // 安全地构建 ASCII 字符串
        for (int i = 0; i < tx_data_len && i < 1024 - 1; i++) {
            tx_ascii_str[i] = isprint(uart_tx_data[i]) ? uart_tx_data[i] : '.';
        }

        cJSON_AddStringToObject(root, "tx_hex", tx_hex_str);
        cJSON_AddStringToObject(root, "tx_ascii", tx_ascii_str);
        cJSON_AddNumberToObject(root, "tx_timestamp", (double)uart_timestamps.tx_timestamp);
        has_data = true;

        // 不再清除发送数据，因为现在使用WebSocket实时发送
        // memset(uart_tx_data, 0, sizeof(uart_tx_data));
        // tx_data_len = 0;
    }
    portEXIT_CRITICAL(&uart_spinlock);

    // 获取接收的数据
    portENTER_CRITICAL(&uart_spinlock);
    if (uart_response != NULL && response_len > 0) {
        size_t hex_pos = 0;

        // 安全地构建十六进制字符串
        for (int i = 0; i < response_len && hex_pos < 2048 - 3; i++) {
            hex_pos += snprintf(rx_hex_str + hex_pos, 2048 - hex_pos,
                              "%02X ", uart_response[i]);
        }

        // 安全地构建 ASCII 字符串
        for (int i = 0; i < response_len && i < 1024 - 1; i++) {
            rx_ascii_str[i] = isprint(uart_response[i]) ? uart_response[i] : '.';
        }

        cJSON_AddStringToObject(root, "rx_hex", rx_hex_str);
        cJSON_AddStringToObject(root, "rx_ascii", rx_ascii_str);
        cJSON_AddNumberToObject(root, "rx_timestamp", (double)uart_timestamps.rx_timestamp);
        has_data = true;
        
        // 清除接收数据，为下一次接收做准备
        memset(uart_response, 0, UART_TRACE_BUFFER_SIZE);
        response_len = 0;
    }
    portEXIT_CRITICAL(&uart_spinlock);

    // 设置响应类型
    httpd_resp_set_type(req, "application/json");

    // 只有在有数据时才发送响应
    if (has_data) {
        json_response = cJSON_Print(root);
        if (json_response) {
            httpd_resp_sendstr(req, json_response);
            free(json_response);
        }
    } else {
        httpd_resp_sendstr(req, "{}");
    }

    // 清理资源
    if (root) {
        cJSON_Delete(root);
    }
    free(tx_hex_str);
    free(tx_ascii_str);
    free(rx_hex_str);
    free(rx_ascii_str);

    return ESP_OK;
}


// 添加全局变量来存储OTA进度
static int ota_progress = 0;

// 添加新的处理函数来获取OTA进度
static esp_err_t get_ota_progress_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "progress", get_ota_progress());
  cJSON_AddStringToObject(root, "status", get_ota_status());

  char *json_str = cJSON_Print(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, strlen(json_str));

  free(json_str);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t get_update_post_handler(httpd_req_t *req) {
  char buf[1024];
  int ret, remaining = req->content_len;

  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf, sizeof(buf))) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        // httpd_resp_send_500(req);
        continue;
      }
      return ESP_FAIL;
    }
    remaining -= ret;
    ESP_LOGI(TAG, "%.*s", ret, buf);
  }

  printf("%s\n", buf);

  httpd_resp_send(req, "DI", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// 添加OTA任务函数
static void ota_task(void *pvParameter) {
  char *url = (char *)pvParameter;
  
  ESP_LOGI(TAG, "OTA任务启动，URL: %s", url);
  
  // 调用智能OTA函数，自动选择HTTP或HTTPS
  simple_ota_task(url);
  
  // 释放分配的URL内存
  free(url);
  
  // 任务完成后删除自己
  vTaskDelete(NULL);
}

static esp_err_t get_ota_post_handler(httpd_req_t *req) {
  char buf[1024];
  int ret, remaining = req->content_len;

  // 读取POST请求数据
  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf, MIN(remaining, sizeof(buf)))) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    remaining -= ret;
    ESP_LOGI(TAG, "%.*s", ret, buf);
  }

  printf("ota post :%s\n", buf);

  // 解析JSON数据
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      ESP_LOGE("JSON", "Error before: %s", error_ptr);
    }
    return ESP_FAIL;
  }

  // 获取OTA URL
  cJSON *ota_url = cJSON_GetObjectItemCaseSensitive(json, "ota_url");
  if (!cJSON_IsString(ota_url) || (ota_url->valuestring == NULL)) {
    ESP_LOGE(TAG, "无效的URL");
    cJSON_Delete(json);
    return ESP_FAIL;
  }

  printf("ota_url: %s\n", ota_url->valuestring);

  // 重置OTA状态
  ota_progress = 0;

  // 发送初始响应
  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "msg", cJSON_CreateString("success"));
  cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(200));
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(root);
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  cJSON_Delete(root);

  // 在新任务中执行OTA更新
  char *url_copy = strdup(ota_url->valuestring); // 复制URL字符串
  if (url_copy != NULL) {
    TaskHandle_t task_handle;
    // OTA验证阶段需要内部RAM栈，保持默认xTaskCreate
    if (xTaskCreatePinnedToCore(ota_task, "ota_task", 12288,
                                (void *)url_copy, 10, &task_handle,
                                SX_NETWORK_CORE_ID) != pdPASS) {
      free(url_copy);
      ESP_LOGE(TAG, "创建OTA任务失败");
    }
  } else {
    ESP_LOGE(TAG, "内存分配失败");
  }

  cJSON_Delete(json);
  return ESP_OK;
}

static esp_err_t get_operate_get_handler(httpd_req_t *req) {
  // 在重启前确保工作模式已设置，但不强制设为默认模式
  nvs_handle_t storage_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &storage_handle);
  if (err == ESP_OK) {
    // 检查是否已设置工作模式
    size_t required_size;
    err = nvs_get_str(storage_handle, "w_mode", NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      // 如果没有设置，才设置默认值为tcp_transparent
      nvs_set_str(storage_handle, "w_mode", "tcp_transparent");
      nvs_commit(storage_handle);
    } else {
      // 如果已设置，但值可能没有被提交，确保此时提交
      char work_mode[32];
      nvs_get_str(storage_handle, "w_mode", work_mode, &required_size);
      ESP_LOGI(TAG, "重启前确认工作模式: %s", work_mode);
      // 重新写入当前值并确保提交，防止未提交的情况
      nvs_set_str(storage_handle, "w_mode", work_mode);
      nvs_commit(storage_handle);
    }
    nvs_close(storage_handle);
  }

  httpd_resp_send(req, "{\"restart\":\"success\"}", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(100);
  esp_restart();
  return ESP_OK;
}

static esp_err_t get_restore_get_handler(httpd_req_t *req) {
  // 保存当前工作模式
  char current_work_mode[32] = "tcp_transparent";  // 默认为TCP透传
  nvs_handle_t storage_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &storage_handle);
  if (err == ESP_OK) {
    // 获取当前工作模式
    size_t required_size = sizeof(current_work_mode);
    err = nvs_get_str(storage_handle, "w_mode", current_work_mode, &required_size);
    if (err != ESP_OK) {
      // 如果获取失败，使用默认值
      strcpy(current_work_mode, "tcp_transparent");
    }
    ESP_LOGI(TAG, "恢复出厂设置前，保存当前工作模式: %s", current_work_mode);
    nvs_close(storage_handle);
  }

  httpd_resp_send(req, "{\"restore\":\"success\"}", HTTPD_RESP_USE_STRLEN);

  esp_err_t reset_err = perform_factory_reset(current_work_mode);
  if (reset_err != ESP_OK) {
    ESP_LOGE(TAG, "恢复出厂设置失败: %s", esp_err_to_name(reset_err));
    return ESP_OK;
  }

  vTaskDelay(pdMS_TO_TICKS(1000));
  esp_restart();
  return ESP_OK;
}

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
  httpd_resp_set_status(req, "302 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
  // ESP_LOGI(TAG, "Redirecting to root");
  return ESP_OK;
}

// Windows NCSI检测处理器 - 返回错误的内容触发强制门户
static esp_err_t windows_ncsi_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "Windows NCSI detection request: %s", req->uri);

  // 检查Host头来确定是否为Windows NCSI请求
  char host_header[128] = {0};
  size_t host_len = httpd_req_get_hdr_value_len(req, "Host");
  if (host_len > 0 && host_len < sizeof(host_header)) {
    httpd_req_get_hdr_value_str(req, "Host", host_header, sizeof(host_header));
    ESP_LOGI(TAG, "Host header: %s", host_header);

    // 检查是否为Microsoft的NCSI域名
    if (strstr(host_header, "msftconnecttest.com") || strstr(host_header, "msftncsi.com")) {
      ESP_LOGI(TAG, "Detected Windows NCSI request, returning captive portal response");

      // 对于Windows NCSI检测，我们返回错误的内容而不是期望的"Microsoft Connect Test"
      // 这会让Windows认为网络需要认证，从而触发强制门户
      httpd_resp_set_hdr(req, "Content-Type", "text/plain");
      httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
      httpd_resp_set_hdr(req, "Pragma", "no-cache");
      httpd_resp_set_hdr(req, "Expires", "0");
      httpd_resp_send(req, "Captive Portal Required", HTTPD_RESP_USE_STRLEN);
      return ESP_OK;
    }
  }

  // 如果不是NCSI请求，重定向到登录页面
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// 强制门户检测处理器 - 用于各种操作系统的自动检测
static esp_err_t captive_portal_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "Captive portal detection request: %s", req->uri);

  // 返回302重定向到登录页面，这会触发强制门户
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");
  httpd_resp_set_hdr(req, "Expires", "0");
  httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// 生成204响应的处理器 - 某些系统需要这个来检测强制门户
static esp_err_t generate_204_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "Generate 204 request: %s", req->uri);

  // 返回302重定向而不是204，强制显示强制门户
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
  httpd_resp_send(req, "Redirect to captive portal", HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}

// SERIAL GET Handler
static esp_err_t get_serial_set_info_get_handler(httpd_req_t *req) {
  // 获取端口参数
  char port_param[8] = {0};
  esp_err_t param_ret = httpd_req_get_url_query_str(req, port_param, sizeof(port_param));
  
  int port = 1; // 默认端口1
  if (param_ret == ESP_OK) {
    char port_value[8] = {0};
    if (httpd_query_key_value(port_param, "port", port_value, sizeof(port_value)) == ESP_OK) {
      port = atoi(port_value);
      if (port < 1 || port > 3) {
        port = 1; // 如果端口号无效，使用默认值
      }
    }
  }
  
  ESP_LOGI(TAG, "Getting serial config for port %d", port);
  
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  
  // 构建通道前缀
  char prefix[8];
  snprintf(prefix, sizeof(prefix), "ch%d", port);
  
  size_t baud_rate, data_bit, check_bit, stop_bit, frame_time, frame_len = 0;
  char key_name[32];
  
  // 构建键名并获取大小
  snprintf(key_name, sizeof(key_name), "%s_baud_rate", prefix);
  ret = nvs_get_str(nvs_handle, key_name, NULL, &baud_rate);
  snprintf(key_name, sizeof(key_name), "%s_data_bit", prefix);
  nvs_get_str(nvs_handle, key_name, NULL, &data_bit);
  snprintf(key_name, sizeof(key_name), "%s_check_bit", prefix);
  nvs_get_str(nvs_handle, key_name, NULL, &check_bit);
  snprintf(key_name, sizeof(key_name), "%s_stop_bit", prefix);
  nvs_get_str(nvs_handle, key_name, NULL, &stop_bit);
  snprintf(key_name, sizeof(key_name), "%s_frame_time", prefix);
  nvs_get_str(nvs_handle, key_name, NULL, &frame_time);
  snprintf(key_name, sizeof(key_name), "%s_frame_len", prefix);
  nvs_get_str(nvs_handle, key_name, NULL, &frame_len);

  // 如果配置不存在，设置默认值
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_frame_len", "512"));
    
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_frame_len", "512"));
    
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_frame_len", "512"));
    break;
  default:
    break;
  }
  
  if (ret != ESP_OK) {
    printf("Error getting size of 'SERIAL': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_baud_rate = malloc(baud_rate);
    char *nvs_data_bit = malloc(data_bit);
    char *nvs_check_bit = malloc(check_bit);
    char *nvs_stop_bit = malloc(stop_bit);
    char *nvs_frame_time = malloc(frame_time);
    char *nvs_frame_len = malloc(frame_len);

    if (nvs_baud_rate == NULL) {
      printf("Memory allocation failed\n");
    } else {
      // 读取指定通道的配置
      snprintf(key_name, sizeof(key_name), "%s_baud_rate", prefix);
      ret = nvs_get_str(nvs_handle, key_name, nvs_baud_rate, &baud_rate);
      snprintf(key_name, sizeof(key_name), "%s_data_bit", prefix);
      nvs_get_str(nvs_handle, key_name, nvs_data_bit, &data_bit);
      snprintf(key_name, sizeof(key_name), "%s_check_bit", prefix);
      nvs_get_str(nvs_handle, key_name, nvs_check_bit, &check_bit);
      snprintf(key_name, sizeof(key_name), "%s_stop_bit", prefix);
      nvs_get_str(nvs_handle, key_name, nvs_stop_bit, &stop_bit);
      snprintf(key_name, sizeof(key_name), "%s_frame_time", prefix);
      nvs_get_str(nvs_handle, key_name, nvs_frame_time, &frame_time);
      snprintf(key_name, sizeof(key_name), "%s_frame_len", prefix);
      nvs_get_str(nvs_handle, key_name, nvs_frame_len, &frame_len);
      
      if (ret == ESP_OK) {
        printf("Port %d - baud_rate: %s, data_bit: %s, check_bit: %s, stop_bit: %s, frame_time: %s, frame_len: %s\n", 
               port, nvs_baud_rate, nvs_data_bit, nvs_check_bit, nvs_stop_bit, nvs_frame_time, nvs_frame_len);
        
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "baud_rate", cJSON_CreateString(nvs_baud_rate));
        cJSON_AddItemToObject(root, "data_bit", cJSON_CreateString(nvs_data_bit));
        cJSON_AddItemToObject(root, "check_bit", cJSON_CreateString(nvs_check_bit));
        cJSON_AddItemToObject(root, "stop_bit", cJSON_CreateString(nvs_stop_bit));
        cJSON_AddItemToObject(root, "frame_time", cJSON_CreateString(nvs_frame_time));
        cJSON_AddItemToObject(root, "frame_len", cJSON_CreateString(nvs_frame_len));
        
        // 读取回复超时时间
        char *nvs_reply_timeout = NULL;
        size_t reply_timeout_size = 0;
        snprintf(key_name, sizeof(key_name), "%s_timeout", prefix);
        if (nvs_get_str(nvs_handle, key_name, NULL, &reply_timeout_size) == ESP_OK) {
          nvs_reply_timeout = malloc(reply_timeout_size);
          if (nvs_reply_timeout && nvs_get_str(nvs_handle, key_name, nvs_reply_timeout, &reply_timeout_size) == ESP_OK) {
            cJSON_AddItemToObject(root, "reply_timeout", cJSON_CreateString(nvs_reply_timeout));
          } else {
            cJSON_AddItemToObject(root, "reply_timeout", cJSON_CreateString("500"));
          }
          free(nvs_reply_timeout);
        } else {
          cJSON_AddItemToObject(root, "reply_timeout", cJSON_CreateString("500"));
        }
        
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));
        free(json_data);
        cJSON_Delete(root);
      } else {
        printf("Error reading serial config: %s\n", esp_err_to_name(ret));
      }
      free(nvs_baud_rate);
      free(nvs_data_bit);
      free(nvs_check_bit);
      free(nvs_stop_bit);
      free(nvs_frame_time);
      free(nvs_frame_len);
    }
  }
  nvs_close(nvs_handle);
  return ESP_OK;
}

// 添加工作模式设置处理函数
static esp_err_t get_work_mode_set_handler(httpd_req_t *req) {
  char *buf = malloc(req->content_len + 1);
  if (buf == NULL) {
    return ESP_ERR_NO_MEM;
  }

  int received = 0;
  while (received < req->content_len) {
    int ret = httpd_req_recv(req, buf + received, req->content_len - received);
    if (ret <= 0) {
      free(buf);
      return send_json_status(req, 500, "数据接收失败");
    }
    received += ret;
  }
  buf[received] = '\0';

  cJSON *json = cJSON_Parse(buf);
  free(buf);
  if (json == NULL) {
    return send_json_status(req, 400, "JSON格式错误");
  }

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    cJSON_Delete(json);
    return send_json_status(req, 500, "存储系统错误");
  }

  err = nvs_set_str(nvs_handle, "w_mode", "auto_collect");
  if (err == ESP_OK) {
    err = nvs_commit(nvs_handle);
  }
  nvs_close(nvs_handle);
  cJSON_Delete(json);

  if (err != ESP_OK) {
    return send_json_status(req, 500, "保存工作模式失败");
  }

  (void)sx_auto_collect_init();
  return send_json_status(req, 200, "工作模式已固定为自动采集");
}

// 自动采集模式配置保存（独立接口，避免与工作模式切换耦合）
#define WEB_AC_MAX_ITEMS 60
#define WEB_AC_MAX_VIRTUAL_REG 49000
#define WEB_AC_DEFAULT_ERROR_MARKER_BASE 0x8000U

_Static_assert(WEB_AC_MAX_ITEMS <= 100,
               "automatic collection NVS keys support two-digit indexes");
_Static_assert(sizeof("ac3_item59_baud") <= NVS_KEY_NAME_MAX_SIZE,
               "automatic collection NVS item key is too long");
_Static_assert(sizeof("ac3_item59_" SX_AC_NVS_ERROR_CLEAR_SUFFIX) <=
                   NVS_KEY_NAME_MAX_SIZE,
               "automatic collection error-clear NVS key is too long");

static uint16_t web_ac_default_error_marker(int item_index) {
  return (uint16_t)(WEB_AC_DEFAULT_ERROR_MARKER_BASE + item_index + 1U);
}

static bool ac_validate_channel_json(cJSON *channel, const char *channel_name,
                                     char *message, size_t message_size) {
  cJSON *items = channel ? cJSON_GetObjectItem(channel, "items") : NULL;
  if (!cJSON_IsArray(items)) {
    return true;
  }

  int count = cJSON_GetArraySize(items);
  if (count > WEB_AC_MAX_ITEMS) {
    snprintf(message, message_size, "%s条目数量不能超过%d", channel_name,
             WEB_AC_MAX_ITEMS);
    return false;
  }

  uint32_t range_starts[WEB_AC_MAX_ITEMS] = {0};
  uint32_t range_ends[WEB_AC_MAX_ITEMS] = {0};
  int range_count = 0;

  for (int i = 0; i < count; i++) {
    cJSON *item = cJSON_GetArrayItem(items, i);
    if (!cJSON_IsObject(item)) {
      snprintf(message, message_size, "%s第%d条格式无效", channel_name,
               i + 1);
      return false;
    }

    cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
    if (!cJSON_IsBool(enabled) || !cJSON_IsTrue(enabled)) {
      continue;
    }

    cJSON *quantity = cJSON_GetObjectItem(item, "register_num");
    if (!cJSON_IsNumber(quantity) || quantity->valuedouble < 1 ||
        quantity->valuedouble > UINT16_MAX ||
        quantity->valuedouble != (double)(uint16_t)quantity->valuedouble) {
      snprintf(message, message_size, "%s第%d条寄存器数量无效",
               channel_name, i + 1);
      return false;
    }

    cJSON *function_code = cJSON_GetObjectItem(item, "function_code");
    if (!cJSON_IsNumber(function_code) || function_code->valuedouble < 0 ||
        function_code->valuedouble > UINT8_MAX ||
        function_code->valuedouble !=
            (double)(uint8_t)function_code->valuedouble) {
      snprintf(message, message_size, "%s第%d条功能码无效", channel_name,
               i + 1);
      return false;
    }

    uint8_t fc = (uint8_t)function_code->valuedouble;
    uint32_t max_quantity = (fc == 0x01 || fc == 0x02) ? 2000U : 125U;
    if ((fc != 0x01 && fc != 0x02 && fc != 0x03 && fc != 0x04 &&
         fc != 0x06 && fc != 0x36) ||
        (fc == 0x06 && quantity->valuedouble != 1) ||
        quantity->valuedouble > max_quantity) {
      snprintf(message, message_size, "%s第%d条功能码与寄存器数量不匹配",
               channel_name, i + 1);
      return false;
    }

    cJSON *error_marker = cJSON_GetObjectItem(item, "error_marker");
    if (!cJSON_IsNumber(error_marker) || error_marker->valuedouble < 2 ||
        error_marker->valuedouble > UINT16_MAX ||
        error_marker->valuedouble !=
            (double)(uint16_t)error_marker->valuedouble) {
      snprintf(message, message_size,
               "%s第%d条错误标记必须在2-65535范围内", channel_name,
               i + 1);
      return false;
    }

    cJSON *real_address = cJSON_GetObjectItem(item, "register_addr");
    if (!cJSON_IsNumber(real_address) || real_address->valuedouble < 0 ||
        real_address->valuedouble > UINT16_MAX ||
        real_address->valuedouble !=
            (double)(uint16_t)real_address->valuedouble ||
        real_address->valuedouble + quantity->valuedouble > 65536.0) {
      snprintf(message, message_size, "%s第%d条真实寄存器范围无效",
               channel_name, i + 1);
      return false;
    }

    cJSON *mapped = cJSON_GetObjectItem(item, "mapped_register_addr");
    if (!cJSON_IsNumber(mapped)) {
      mapped = cJSON_GetObjectItem(item, "register_addr");
    }
    if (!cJSON_IsNumber(mapped) || mapped->valuedouble < 0 ||
        mapped->valuedouble > UINT16_MAX ||
        mapped->valuedouble != (double)(uint16_t)mapped->valuedouble) {
      snprintf(message, message_size, "%s第%d条映射寄存器地址无效",
               channel_name, i + 1);
      return false;
    }

    uint32_t start = (uint16_t)mapped->valuedouble;
    uint32_t register_count = (uint16_t)quantity->valuedouble;
    uint32_t end = start + register_count - 1U;
    if (end > WEB_AC_MAX_VIRTUAL_REG) {
      snprintf(message, message_size,
               "%s第%d条映射范围超出%d，49001-49060保留为错误标志位",
               channel_name, i + 1, WEB_AC_MAX_VIRTUAL_REG);
      return false;
    }

    for (int range_index = 0; range_index < range_count; range_index++) {
      if (!(start > range_ends[range_index] ||
            range_starts[range_index] > end)) {
        snprintf(message, message_size, "%s第%d条映射范围与已有条目重叠",
                 channel_name, i + 1);
        return false;
      }
    }
    range_starts[range_count] = start;
    range_ends[range_count] = end;
    range_count++;
  }
  return true;
}

static esp_err_t ac_erase_legacy_item_keys(nvs_handle_t nvs,
                                           const char *prefix, int index) {
  static const char *const suffixes[] = {
      "en", "rs", "fc", "ra", "mra", "rn", "err",
      SX_AC_NVS_ERROR_CLEAR_SUFFIX, "int", "to", "baud", "db", "par", "sb"};
  char key[32];
  for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
    snprintf(key, sizeof(key), "%s_item%d_%s", prefix, index, suffixes[i]);
    esp_err_t err = nvs_erase_key(nvs, key);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      return err;
    }
  }
  return ESP_OK;
}

static void ac_json_to_stored_item(cJSON *item, int index,
                                   sx_ac_nvs_item_t *stored) {
  *stored = (sx_ac_nvs_item_t){
      .version = SX_AC_NVS_ITEM_VERSION,
      .real_slave_addr = 1,
      .function_code = 3,
      .data_bits = 8,
      .stop_bits = 1,
      .register_num = 1,
      .error_marker = web_ac_default_error_marker(index),
      .error_clear_count = 50,
      .interval_ms = 100,
      .timeout_ms = 1000,
      .baudrate = 9600,
  };
#define AC_JSON_STORE(field, json_name, type)                                \
  do {                                                                       \
    cJSON *value = cJSON_GetObjectItem(item, (json_name));                   \
    if (cJSON_IsNumber(value)) stored->field = (type)value->valuedouble;     \
  } while (0)
  cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
  stored->enabled = cJSON_IsBool(enabled) && cJSON_IsTrue(enabled);
  AC_JSON_STORE(real_slave_addr, "real_slave_addr", uint8_t);
  AC_JSON_STORE(function_code, "function_code", uint8_t);
  AC_JSON_STORE(register_addr, "register_addr", uint16_t);
  cJSON *mapped_register =
      cJSON_GetObjectItem(item, "mapped_register_addr");
  stored->mapped_register_addr = cJSON_IsNumber(mapped_register)
                                         ? (uint16_t)mapped_register->valuedouble
                                         : stored->register_addr;
  AC_JSON_STORE(register_num, "register_num", uint16_t);
  AC_JSON_STORE(error_marker, "error_marker", uint16_t);
  AC_JSON_STORE(error_clear_count, "error_clear_count", uint16_t);
  AC_JSON_STORE(interval_ms, "interval_ms", uint32_t);
  AC_JSON_STORE(timeout_ms, "timeout_ms", uint32_t);
  AC_JSON_STORE(baudrate, "baudrate", uint32_t);
  AC_JSON_STORE(data_bits, "data_bits", uint8_t);
  AC_JSON_STORE(parity, "parity", uint8_t);
  AC_JSON_STORE(stop_bits, "stop_bits", uint8_t);
#undef AC_JSON_STORE
}

static esp_err_t ac_save_channel_to_nvs(nvs_handle_t nvs, cJSON *channel,
                                        const char *prefix) {
#define AC_NVS_SET_OR_RETURN(operation)                                       \
  do {                                                                        \
    esp_err_t set_err = (operation);                                          \
    if (set_err != ESP_OK) {                                                  \
      ESP_LOGE("AUTO_COLLECT_SET", "NVS operation failed near key %s: %s",  \
               key, esp_err_to_name(set_err));                                \
      return set_err;                                                         \
    }                                                                         \
  } while (0)

  char key[32];
  uint8_t mapped = (strcmp(prefix, "ac3") == 0) ? 3 : 2;
  if (channel) {
    cJSON *maddr = cJSON_GetObjectItem(channel, "mapped_slave_addr");
    if (cJSON_IsNumber(maddr)) mapped = (uint8_t)maddr->valuedouble;
  }
  snprintf(key, sizeof(key), "%s_maddr", prefix);
  AC_NVS_SET_OR_RETURN(nvs_set_u8(nvs, key, mapped));

  uint8_t allow_exception_response = 0;
  if (channel) {
    cJSON *allow =
        cJSON_GetObjectItem(channel, "allow_exception_response");
    if (cJSON_IsBool(allow)) {
      allow_exception_response = cJSON_IsTrue(allow) ? 1 : 0;
    } else if (cJSON_IsNumber(allow)) {
      allow_exception_response = allow->valuedouble != 0 ? 1 : 0;
    }
  }
  snprintf(key, sizeof(key), "%s_err_rsp", prefix);
  AC_NVS_SET_OR_RETURN(nvs_set_u8(nvs, key, allow_exception_response));

  int count = 0;
  cJSON *items = channel ? cJSON_GetObjectItem(channel, "items") : NULL;
  if (cJSON_IsArray(items)) {
    count = cJSON_GetArraySize(items);
  }
  if (count < 0) count = 0;
  if (count > 60) count = 60;
  int32_t old_count = 0;
  snprintf(key, sizeof(key), "%s_count", prefix);
  if (nvs_get_i32(nvs, key, &old_count) != ESP_OK || old_count < 0 ||
      old_count > WEB_AC_MAX_ITEMS) {
    old_count = 0;
  }
  AC_NVS_SET_OR_RETURN(nvs_set_i32(nvs, key, count));

  for (int i = 0; i < count; i++) {
    cJSON *it = cJSON_GetArrayItem(items, i);
    if (!cJSON_IsObject(it)) continue;
    sx_ac_nvs_item_t stored;
    ac_json_to_stored_item(it, i, &stored);

    snprintf(key, sizeof(key), "%s_i%d", prefix, i);
    AC_NVS_SET_OR_RETURN(nvs_set_blob(nvs, key, &stored, sizeof(stored)));
    AC_NVS_SET_OR_RETURN(ac_erase_legacy_item_keys(nvs, prefix, i));
  }

  for (int i = count; i < old_count; i++) {
    snprintf(key, sizeof(key), "%s_i%d", prefix, i);
    esp_err_t erase_err = nvs_erase_key(nvs, key);
    if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) {
      return erase_err;
    }
    AC_NVS_SET_OR_RETURN(ac_erase_legacy_item_keys(nvs, prefix, i));
  }

#undef AC_NVS_SET_OR_RETURN
  return ESP_OK;
}

static esp_err_t ac_verify_channel_storage(nvs_handle_t nvs, cJSON *channel,
                                           const char *prefix) {
  uint8_t expected_mapped = (strcmp(prefix, "ac3") == 0) ? 3 : 2;
  cJSON *mapped = cJSON_GetObjectItem(channel, "mapped_slave_addr");
  if (cJSON_IsNumber(mapped)) {
    expected_mapped = (uint8_t)mapped->valuedouble;
  }
  uint8_t saved_mapped = 0;
  char key[16];
  snprintf(key, sizeof(key), "%s_maddr", prefix);
  esp_err_t err = nvs_get_u8(nvs, key, &saved_mapped);
  if (err != ESP_OK || saved_mapped != expected_mapped) {
    return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
  }

  uint8_t expected_allow = 0;
  cJSON *allow = cJSON_GetObjectItem(channel, "allow_exception_response");
  if (cJSON_IsBool(allow)) {
    expected_allow = cJSON_IsTrue(allow) ? 1 : 0;
  } else if (cJSON_IsNumber(allow)) {
    expected_allow = allow->valuedouble != 0 ? 1 : 0;
  }
  uint8_t saved_allow = 0;
  snprintf(key, sizeof(key), "%s_err_rsp", prefix);
  err = nvs_get_u8(nvs, key, &saved_allow);
  if (err != ESP_OK || saved_allow != expected_allow) {
    return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
  }

  cJSON *items = cJSON_GetObjectItem(channel, "items");
  int expected_count = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
  int32_t saved_count = -1;
  snprintf(key, sizeof(key), "%s_count", prefix);
  err = nvs_get_i32(nvs, key, &saved_count);
  if (err != ESP_OK || saved_count != expected_count) {
    return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
  }

  for (int i = 0; i < expected_count; i++) {
    sx_ac_nvs_item_t expected;
    sx_ac_nvs_item_t saved = {0};
    ac_json_to_stored_item(cJSON_GetArrayItem(items, i), i, &expected);
    size_t saved_size = sizeof(saved);
    snprintf(key, sizeof(key), "%s_i%d", prefix, i);
    err = nvs_get_blob(nvs, key, &saved, &saved_size);
    if (err != ESP_OK || saved_size != sizeof(saved) ||
        memcmp(&saved, &expected, sizeof(saved)) != 0) {
      return err == ESP_OK ? ESP_ERR_INVALID_STATE : err;
    }
  }
  return ESP_OK;
}

static esp_err_t auto_collect_set_handler(httpd_req_t *req) {
  static const char *TAG = "AUTO_COLLECT_SET";
  int total_len = req->content_len;
  if (total_len <= 0 || total_len > 32768) {
    ESP_LOGE(TAG, "内容长度异常: %d", total_len);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid length");
    return ESP_FAIL;
  }

  char *buf = calloc(1, total_len + 1);
  if (!buf) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  int received = 0;
  while (received < total_len) {
    int ret = httpd_req_recv(req, buf + received, total_len - received);
    if (ret <= 0) {
      free(buf);
      return ESP_FAIL;
    }
    received += ret;
  }
  buf[received] = '\0';

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    ESP_LOGE(TAG, "JSON解析失败");
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "parse error");
    return ESP_FAIL;
  }

  nvs_handle_t nvs_ac;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_ac);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS打开失败: %s", esp_err_to_name(err));
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "nvs open fail");
    return ESP_FAIL;
  }

  cJSON *ch2 = cJSON_GetObjectItem(root, "ch2");
  cJSON *ch3 = cJSON_GetObjectItem(root, "ch3");
  char validation_message[128] = {0};
  if (!ac_validate_channel_json(ch2, "CH2", validation_message,
                                sizeof(validation_message)) ||
      !ac_validate_channel_json(ch3, "CH3", validation_message,
                                sizeof(validation_message))) {
    nvs_close(nvs_ac);
    cJSON_Delete(root);
    return send_json_status(req, 400, validation_message);
  }
  cJSON *ch2_maddr = ch2 ? cJSON_GetObjectItem(ch2, "mapped_slave_addr") : NULL;
  cJSON *ch3_maddr = ch3 ? cJSON_GetObjectItem(ch3, "mapped_slave_addr") : NULL;
  if (!cJSON_IsNumber(ch2_maddr) || !cJSON_IsNumber(ch3_maddr) ||
      ch2_maddr->valuedouble < 1 || ch2_maddr->valuedouble > 247 ||
      ch3_maddr->valuedouble < 1 || ch3_maddr->valuedouble > 247 ||
      ch2_maddr->valuedouble != (double)(uint8_t)ch2_maddr->valuedouble ||
      ch3_maddr->valuedouble != (double)(uint8_t)ch3_maddr->valuedouble) {
    nvs_close(nvs_ac);
    cJSON_Delete(root);
    return send_json_status(req, 400, "映射从机地址必须在1-247范围内");
  }
  uint8_t ch2_mapped = (uint8_t)ch2_maddr->valuedouble;
  uint8_t ch3_mapped = (uint8_t)ch3_maddr->valuedouble;
  if (ch2_mapped == ch3_mapped) {
    nvs_close(nvs_ac);
    cJSON_Delete(root);
    return send_json_status(req, 400, "CH2和CH3的映射从机地址不能相同");
  }
  err = ac_save_channel_to_nvs(nvs_ac, ch2, "ac2");
  if (err == ESP_OK) {
    err = ac_save_channel_to_nvs(nvs_ac, ch3, "ac3");
  }
  if (err == ESP_OK) {
    err = nvs_commit(nvs_ac);
  }
  nvs_close(nvs_ac);

  bool verify_failed = false;
  if (err == ESP_OK) {
    err = nvs_open("storage", NVS_READONLY, &nvs_ac);
    if (err == ESP_OK) {
      err = ac_verify_channel_storage(nvs_ac, ch2, "ac2");
      if (err == ESP_OK) {
        err = ac_verify_channel_storage(nvs_ac, ch3, "ac3");
      }
      nvs_close(nvs_ac);
    }
    verify_failed = err != ESP_OK;
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "自动采集配置%s失败: %s",
             verify_failed ? "回读校验" : "写入", esp_err_to_name(err));
    cJSON_Delete(root);
    return send_json_status(
        req, 500,
        verify_failed ? "自动采集配置存储校验失败"
                      : "自动采集配置保存失败");
  }

  cJSON *resp = cJSON_CreateObject();
  cJSON_AddNumberToObject(resp, "code", 200);
  cJSON_AddStringToObject(resp, "msg", "auto_collect 保存成功");
  char *resp_str = cJSON_Print(resp);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, resp_str);

  free(resp_str);
  cJSON_Delete(resp);
  cJSON_Delete(root);
  return ESP_OK;
}

#if 0
// Modbus地址过滤配置API
static esp_err_t get_modbus_filter_config_handler(httpd_req_t *req) {
  static const char *TAG = "MODBUS_FILTER_CONFIG";
  ESP_LOGI(TAG, "获取Modbus地址过滤配置");
  
  cJSON *response = cJSON_CreateObject();
  if (response == NULL) {
    return ESP_ERR_NO_MEM;
  }
  
  modbus_queue_config_t config;
  esp_err_t ret = modbus_queue_load_config(&config);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "成功加载配置: enabled=%d, mode=%d, range_count=%d", 
             config.enabled, config.mode, config.range_count);
    cJSON_AddBoolToObject(response, "enabled", config.enabled);
    cJSON_AddNumberToObject(response, "mode", config.mode);
    
    cJSON *ranges = cJSON_CreateArray();
    for (int i = 0; i < config.range_count; i++) {
      cJSON *range = cJSON_CreateObject();
      cJSON_AddNumberToObject(range, "slave_id", config.addr_ranges[i].slave_id);
      cJSON_AddNumberToObject(range, "start", config.addr_ranges[i].start_addr);
      cJSON_AddNumberToObject(range, "end", config.addr_ranges[i].end_addr);
      cJSON_AddItemToArray(ranges, range);
    }
    cJSON_AddItemToObject(response, "ranges", ranges);
  } else {
    // 如果加载失败，返回默认配置并尝试初始化
    ESP_LOGW(TAG, "加载配置失败: %s，返回默认配置", esp_err_to_name(ret));
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGI(TAG, "首次运行，将在保存时创建配置");
    }
    cJSON_AddBoolToObject(response, "enabled", false);
    cJSON_AddNumberToObject(response, "mode", 1); // 默认白名单模式
    cJSON *ranges = cJSON_CreateArray();
    cJSON_AddItemToObject(response, "ranges", ranges);
  }
  
  const char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, strlen(json_str));
  
  free((void*)json_str);
  cJSON_Delete(response);
  
  return ESP_OK;
}

static esp_err_t set_modbus_filter_config_handler(httpd_req_t *req) {
  static const char *TAG = "MODBUS_FILTER_CONFIG";
  ESP_LOGI(TAG, "设置Modbus地址过滤配置");
  
  char buf[1024];
  int ret, remaining = req->content_len;
  size_t total_len = 0;
  
  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf + total_len, MIN(remaining, sizeof(buf) - total_len - 1))) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    remaining -= ret;
    total_len += ret;
  }
  buf[total_len] = '\0';
  
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    ESP_LOGI(TAG, "JSON解析失败");
    return ESP_FAIL;
  }
  
  // 先加载现有配置，避免覆盖映射配置
  modbus_queue_config_t config = {0};
  esp_err_t load_ret = modbus_queue_load_config(&config);
  if (load_ret != ESP_OK) {
    ESP_LOGW(TAG, "加载现有配置失败，使用默认配置");
    // 设置默认值
    config.queue_timeout_ms = 1000;  // 默认超时1秒
    config.max_queue_size = 10;      // 默认队列大小10
  }
  
  // 解析配置
  cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
  if (cJSON_IsBool(enabled)) {
    config.enabled = cJSON_IsTrue(enabled);
  }
  
  cJSON *mode = cJSON_GetObjectItem(json, "mode");
  if (cJSON_IsNumber(mode)) {
    config.mode = (modbus_queue_filter_mode_t)mode->valueint;
  }
  
  cJSON *ranges = cJSON_GetObjectItem(json, "ranges");
  if (cJSON_IsArray(ranges)) {
    int array_size = cJSON_GetArraySize(ranges);
    config.range_count = 0;
    for (int i = 0; i < array_size && i < MODBUS_MAX_ADDR_RANGES; i++) {
      cJSON *range = cJSON_GetArrayItem(ranges, i);
      if (cJSON_IsObject(range)) {
        cJSON *slave_id = cJSON_GetObjectItem(range, "slave_id");
        cJSON *start = cJSON_GetObjectItem(range, "start");
        cJSON *end = cJSON_GetObjectItem(range, "end");
        if (cJSON_IsNumber(start) && cJSON_IsNumber(end)) {
          config.addr_ranges[config.range_count].slave_id = cJSON_IsNumber(slave_id) ? (uint8_t)slave_id->valueint : 0;
          config.addr_ranges[config.range_count].start_addr = (uint16_t)start->valueint;
          config.addr_ranges[config.range_count].end_addr = (uint16_t)end->valueint;
          config.range_count++;
          ESP_LOGI(TAG, "添加地址范围: 从机号=%d, 起始=%d, 结束=%d", 
                  config.addr_ranges[config.range_count-1].slave_id,
                  config.addr_ranges[config.range_count-1].start_addr,
                  config.addr_ranges[config.range_count-1].end_addr);
        }
      }
    }
  }
  
  // 保存前打印配置详情
  ESP_LOGI(TAG, "准备保存配置: enabled=%d, mode=%d, range_count=%d, timeout=%u, queue_size=%d", 
           config.enabled, config.mode, config.range_count, config.queue_timeout_ms, config.max_queue_size);
  for (int i = 0; i < config.range_count; i++) {
    ESP_LOGI(TAG, "  范围[%d]: 从机号=%d, 地址=%d-%d", i, 
             config.addr_ranges[i].slave_id, config.addr_ranges[i].start_addr, config.addr_ranges[i].end_addr);
  }
  
  // 保存配置
  ret = modbus_queue_save_config(&config);
  
  // 配置已保存到NVS
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Modbus过滤配置已保存到NVS");
    
    // 立即重新加载配置到运行中的工作模式
    esp_err_t reload_ret = modbus_queue_reload_config();
    if (reload_ret == ESP_OK) {
      ESP_LOGI(TAG, "过滤配置已立即应用到工作模式");
    } else {
      ESP_LOGW(TAG, "过滤配置重新加载失败: %s", esp_err_to_name(reload_ret));
    }
  } else {
    ESP_LOGE(TAG, "保存Modbus过滤配置失败: %s", esp_err_to_name(ret));
  }
  
  cJSON_Delete(json);
  
  // 返回响应
  cJSON *response = cJSON_CreateObject();
  cJSON_AddBoolToObject(response, "success", ret == ESP_OK);
  if (ret != ESP_OK) {
    cJSON_AddStringToObject(response, "error", esp_err_to_name(ret));
  } else {
    cJSON_AddStringToObject(response, "message", "配置已保存到NVS");
  }
  
  const char *json_str = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_str, strlen(json_str));
  
  free((void*)json_str);
  cJSON_Delete(response);
  
  return ESP_OK;
}

// Modbus从机地址映射配置API
static esp_err_t get_slave_mapping_config_handler(httpd_req_t *req) {
  static const char *TAG = "SLAVE_MAPPING_CONFIG";
  ESP_LOGI(TAG, "获取Modbus从机地址映射配置");
  
  cJSON *response = cJSON_CreateObject();
  if (response == NULL) {
    return ESP_ERR_NO_MEM;
  }
  
  modbus_queue_config_t config;
  esp_err_t ret = modbus_queue_load_config(&config);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "成功加载配置: mapping_enabled=%d, mapping_count=%d", 
             config.mapping_enabled, config.mapping_count);
    cJSON_AddBoolToObject(response, "enabled", config.mapping_enabled);
    
    cJSON *mappings = cJSON_CreateArray();
    for (int i = 0; i < config.mapping_count; i++) {
      cJSON *mapping = cJSON_CreateObject();
      cJSON_AddNumberToObject(mapping, "virtual_addr", config.slave_mappings[i].virtual_addr);
      cJSON_AddNumberToObject(mapping, "real_addr", config.slave_mappings[i].real_addr);
      cJSON_AddBoolToObject(mapping, "enabled", config.slave_mappings[i].enabled);
      cJSON_AddItemToArray(mappings, mapping);
    }
    cJSON_AddItemToObject(response, "mappings", mappings);
  } else {
    ESP_LOGE(TAG, "加载配置失败: %s", esp_err_to_name(ret));
    cJSON_AddBoolToObject(response, "enabled", false);
    cJSON_AddItemToObject(response, "mappings", cJSON_CreateArray());
  }
  
  char *json_string = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_send(req, json_string, strlen(json_string));
  
  free(json_string);
  cJSON_Delete(response);
  
  return ESP_OK;
}

static esp_err_t set_slave_mapping_config_handler(httpd_req_t *req) {
  static const char *TAG = "SLAVE_MAPPING_CONFIG";
  ESP_LOGI(TAG, "设置Modbus从机地址映射配置");
  
  char buf[2048];
  int ret, remaining = req->content_len;
  size_t total_len = 0;
  
  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf + total_len, MIN(remaining, sizeof(buf) - total_len - 1))) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    remaining -= ret;
    total_len += ret;
  }
  buf[total_len] = '\0';
  
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    ESP_LOGI(TAG, "JSON解析失败");
    return ESP_FAIL;
  }
  
  // 首先加载现有配置
  modbus_queue_config_t config;
  esp_err_t load_ret = modbus_queue_load_config(&config);
  if (load_ret != ESP_OK) {
    ESP_LOGW(TAG, "加载现有配置失败，使用默认配置");
    memset(&config, 0, sizeof(config));
    // 设置默认值
    config.queue_timeout_ms = 1000;
    config.max_queue_size = 10;
  }
  
  // 解析映射配置
  cJSON *enabled = cJSON_GetObjectItem(json, "enabled");
  if (enabled && cJSON_IsBool(enabled)) {
    config.mapping_enabled = cJSON_IsTrue(enabled);
  }
  
  cJSON *mappings = cJSON_GetObjectItem(json, "mappings");
  if (mappings && cJSON_IsArray(mappings)) {
    int mapping_count = cJSON_GetArraySize(mappings);
    config.mapping_count = (mapping_count > MODBUS_MAX_SLAVE_MAPPINGS) ? MODBUS_MAX_SLAVE_MAPPINGS : mapping_count;
    
    for (int i = 0; i < config.mapping_count; i++) {
      cJSON *mapping = cJSON_GetArrayItem(mappings, i);
      if (mapping) {
        cJSON *virtual_addr = cJSON_GetObjectItem(mapping, "virtual_addr");
        cJSON *real_addr = cJSON_GetObjectItem(mapping, "real_addr");
        cJSON *mapping_enabled = cJSON_GetObjectItem(mapping, "enabled");
        
        if (virtual_addr && cJSON_IsNumber(virtual_addr)) {
          config.slave_mappings[i].virtual_addr = (uint8_t)cJSON_GetNumberValue(virtual_addr);
        }
        if (real_addr && cJSON_IsNumber(real_addr)) {
          config.slave_mappings[i].real_addr = (uint8_t)cJSON_GetNumberValue(real_addr);
        }
        if (mapping_enabled && cJSON_IsBool(mapping_enabled)) {
          config.slave_mappings[i].enabled = cJSON_IsTrue(mapping_enabled);
        }
      }
    }
  }
  
  ESP_LOGI(TAG, "配置解析完成: mapping_enabled=%d, mapping_count=%d", 
           config.mapping_enabled, config.mapping_count);
  
  for (int i = 0; i < config.mapping_count; i++) {
    ESP_LOGI(TAG, "  映射[%d]: 虚拟地址=%d -> 真实地址=%d, 启用=%d", i, 
             config.slave_mappings[i].virtual_addr, 
             config.slave_mappings[i].real_addr,
             config.slave_mappings[i].enabled);
  }
  
  // 保存配置
  ret = modbus_queue_save_config(&config);
  
  cJSON_Delete(json);
  
  // 返回结果
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Modbus从机地址映射配置已保存到NVS");
    
    // 立即重新加载配置到运行中的工作模式
    esp_err_t reload_ret = modbus_queue_reload_config();
    if (reload_ret == ESP_OK) {
      ESP_LOGI(TAG, "地址映射配置已立即应用到工作模式");
    } else {
      ESP_LOGW(TAG, "地址映射配置重新加载失败: %s", esp_err_to_name(reload_ret));
    }
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"code\":200,\"msg\":\"配置保存成功并已立即应用\"}", -1);
  } else {
    ESP_LOGE(TAG, "保存配置失败: %s", esp_err_to_name(ret));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"code\":500,\"msg\":\"配置保存失败\"}", -1);
  }
  
  return ESP_OK;
}

#endif

static bool ac_add_compact_item_json(nvs_handle_t nvs, const char *prefix,
                                     int index, cJSON *item) {
  sx_ac_nvs_item_t stored = {0};
  size_t stored_size = sizeof(stored);
  char key[16];
  snprintf(key, sizeof(key), "%s_i%d", prefix, index);
  if (nvs_get_blob(nvs, key, &stored, &stored_size) != ESP_OK ||
      stored_size != sizeof(stored) ||
      stored.version != SX_AC_NVS_ITEM_VERSION) {
    return false;
  }

  cJSON_AddBoolToObject(item, "enabled", stored.enabled != 0);
  cJSON_AddNumberToObject(item, "real_slave_addr", stored.real_slave_addr);
  cJSON_AddNumberToObject(item, "function_code", stored.function_code);
  cJSON_AddNumberToObject(item, "register_addr", stored.register_addr);
  cJSON_AddNumberToObject(item, "mapped_register_addr",
                         stored.mapped_register_addr);
  cJSON_AddNumberToObject(item, "register_num", stored.register_num);
  cJSON_AddNumberToObject(
      item, "error_marker",
      stored.error_marker >= 2 ? stored.error_marker
                               : web_ac_default_error_marker(index));
  cJSON_AddNumberToObject(item, "error_clear_count",
                         stored.error_clear_count ? stored.error_clear_count
                                                  : 50);
  cJSON_AddNumberToObject(item, "interval_ms", stored.interval_ms);
  cJSON_AddNumberToObject(item, "timeout_ms", stored.timeout_ms);
  cJSON_AddNumberToObject(item, "baudrate", stored.baudrate);
  cJSON_AddNumberToObject(item, "data_bits", stored.data_bits);
  cJSON_AddNumberToObject(item, "parity", stored.parity);
  cJSON_AddNumberToObject(item, "stop_bits", stored.stop_bits);
  return true;
}

// 添加工作模式信息获取处理函数
static esp_err_t get_work_mode_info_handler(httpd_req_t *req) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    // 处理 NVS 打开失败
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to open NVS");
    return ESP_FAIL;
  }

  cJSON *response = cJSON_CreateObject();
  if (response == NULL) {
    nvs_close(nvs_handle);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to create JSON object");
    return ESP_FAIL;
  }

  char work_mode[32] = {0};
  size_t len = sizeof(work_mode);
  if (nvs_get_str(nvs_handle, "w_mode", work_mode, &len) != ESP_OK ||
      strcmp(work_mode, "auto_collect") != 0) {
    strcpy(work_mode, "auto_collect");
  }
  cJSON_AddStringToObject(response, "work_mode", work_mode);

  cJSON *ch2 = cJSON_CreateObject();
  cJSON *ch3 = cJSON_CreateObject();
  if (ch2 && ch3) {
    uint8_t maddr = 2;
    nvs_get_u8(nvs_handle, "ac2_maddr", &maddr);
    cJSON_AddNumberToObject(ch2, "mapped_slave_addr", maddr);
    uint8_t allow_exception_response = 0;
    nvs_get_u8(nvs_handle, "ac2_err_rsp", &allow_exception_response);
    cJSON_AddBoolToObject(ch2, "allow_exception_response",
                          allow_exception_response != 0);
    int32_t cnt = 0;
    nvs_get_i32(nvs_handle, "ac2_count", &cnt);
    cJSON *items = cJSON_AddArrayToObject(ch2, "items");
    if (items && cnt > 0) {
      if (cnt > 60) cnt = 60;
      for (int i = 0; i < cnt; i++) {
        cJSON *it = cJSON_CreateObject();
        if (!it) continue;
        if (ac_add_compact_item_json(nvs_handle, "ac2", i, it)) {
          cJSON_AddItemToArray(items, it);
          continue;
        }
        char key[32];
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_en", i), key), &u8);
        cJSON_AddBoolToObject(it, "enabled", u8 != 0);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_rs", i), key), &u8);
        cJSON_AddNumberToObject(it, "real_slave_addr", u8);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_fc", i), key), &u8);
        cJSON_AddNumberToObject(it, "function_code", u8);
        nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_ra", i), key), &u16);
        cJSON_AddNumberToObject(it, "register_addr", u16);
        uint16_t mra = u16;
        if (nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_mra", i), key), &u16) == ESP_OK) {
          mra = u16;
        }
        cJSON_AddNumberToObject(it, "mapped_register_addr", mra);
        nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_rn", i), key), &u16);
        cJSON_AddNumberToObject(it, "register_num", u16);
        uint16_t err_marker = web_ac_default_error_marker(i);
        if (nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_err", i), key), &u16) == ESP_OK &&
            u16 >= 2) {
          err_marker = u16;
        }
        cJSON_AddNumberToObject(it, "error_marker", err_marker);
        uint16_t error_clear_count = 50;
        if (nvs_get_u16(nvs_handle,
                        (snprintf(key, sizeof(key),
                                  "ac2_item%d_" SX_AC_NVS_ERROR_CLEAR_SUFFIX,
                                  i),
                         key),
                        &u16) == ESP_OK &&
            u16 > 0) {
          error_clear_count = u16;
        }
        cJSON_AddNumberToObject(it, "error_clear_count", error_clear_count);
        nvs_get_u32(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_int", i), key), &u32);
        cJSON_AddNumberToObject(it, "interval_ms", u32);
        nvs_get_u32(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_to", i), key), &u32);
        cJSON_AddNumberToObject(it, "timeout_ms", u32);
        nvs_get_u32(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_baud", i), key), &u32);
        cJSON_AddNumberToObject(it, "baudrate", u32);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_db", i), key), &u8);
        cJSON_AddNumberToObject(it, "data_bits", u8);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_par", i), key), &u8);
        cJSON_AddNumberToObject(it, "parity", u8);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac2_item%d_sb", i), key), &u8);
        cJSON_AddNumberToObject(it, "stop_bits", u8);
        cJSON_AddItemToArray(items, it);
      }
    }

    maddr = 3;
    nvs_get_u8(nvs_handle, "ac3_maddr", &maddr);
    cJSON_AddNumberToObject(ch3, "mapped_slave_addr", maddr);
    allow_exception_response = 0;
    nvs_get_u8(nvs_handle, "ac3_err_rsp", &allow_exception_response);
    cJSON_AddBoolToObject(ch3, "allow_exception_response",
                          allow_exception_response != 0);
    cnt = 0;
    nvs_get_i32(nvs_handle, "ac3_count", &cnt);
    items = cJSON_AddArrayToObject(ch3, "items");
    if (items && cnt > 0) {
      if (cnt > 60) cnt = 60;
      for (int i = 0; i < cnt; i++) {
        cJSON *it = cJSON_CreateObject();
        if (!it) continue;
        if (ac_add_compact_item_json(nvs_handle, "ac3", i, it)) {
          cJSON_AddItemToArray(items, it);
          continue;
        }
        char key[32];
        uint8_t u8;
        uint16_t u16;
        uint32_t u32;
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_en", i), key), &u8);
        cJSON_AddBoolToObject(it, "enabled", u8 != 0);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_rs", i), key), &u8);
        cJSON_AddNumberToObject(it, "real_slave_addr", u8);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_fc", i), key), &u8);
        cJSON_AddNumberToObject(it, "function_code", u8);
        nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_ra", i), key), &u16);
        cJSON_AddNumberToObject(it, "register_addr", u16);
        uint16_t mra = u16;
        if (nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_mra", i), key), &u16) == ESP_OK) {
          mra = u16;
        }
        cJSON_AddNumberToObject(it, "mapped_register_addr", mra);
        nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_rn", i), key), &u16);
        cJSON_AddNumberToObject(it, "register_num", u16);
        uint16_t err_marker = web_ac_default_error_marker(i);
        if (nvs_get_u16(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_err", i), key), &u16) == ESP_OK &&
            u16 >= 2) {
          err_marker = u16;
        }
        cJSON_AddNumberToObject(it, "error_marker", err_marker);
        uint16_t error_clear_count = 50;
        if (nvs_get_u16(nvs_handle,
                        (snprintf(key, sizeof(key),
                                  "ac3_item%d_" SX_AC_NVS_ERROR_CLEAR_SUFFIX,
                                  i),
                         key),
                        &u16) == ESP_OK &&
            u16 > 0) {
          error_clear_count = u16;
        }
        cJSON_AddNumberToObject(it, "error_clear_count", error_clear_count);
        nvs_get_u32(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_int", i), key), &u32);
        cJSON_AddNumberToObject(it, "interval_ms", u32);
        nvs_get_u32(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_to", i), key), &u32);
        cJSON_AddNumberToObject(it, "timeout_ms", u32);
        nvs_get_u32(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_baud", i), key), &u32);
        cJSON_AddNumberToObject(it, "baudrate", u32);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_db", i), key), &u8);
        cJSON_AddNumberToObject(it, "data_bits", u8);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_par", i), key), &u8);
        cJSON_AddNumberToObject(it, "parity", u8);
        nvs_get_u8(nvs_handle, (snprintf(key, sizeof(key), "ac3_item%d_sb", i), key), &u8);
        cJSON_AddNumberToObject(it, "stop_bits", u8);
        cJSON_AddItemToArray(items, it);
      }
    }

    cJSON_AddItemToObject(response, "ch2", ch2);
    cJSON_AddItemToObject(response, "ch3", ch3);
  } else {
    cJSON_Delete(ch2);
    cJSON_Delete(ch3);
  }
  cJSON_AddBoolToObject(response, "auto_transparent_cache", true);
  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(response);
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  cJSON_Delete(response);
  nvs_close(nvs_handle);
  return ESP_OK;

#if 0
  // 获取工作模式
  char work_mode[32] = "transparent_queue";
  size_t len = sizeof(work_mode);
  err = nvs_get_str(nvs_handle, "w_mode", work_mode, &len);
  if (err != ESP_ERR_NVS_NOT_FOUND) { // 如果键不存在使用默认值
    cJSON_AddStringToObject(response, "work_mode", work_mode);
  } else {
    // 如果w_mode不存在，添加默认值transparent_queue（排队透传模式）
    cJSON_AddStringToObject(response, "work_mode", "transparent_queue");
  }

  // 自动采集模式：返回通道配置
  if (strcmp(work_mode, "auto_collect") == 0) {
    cJSON *ch2 = cJSON_CreateObject();
    cJSON *ch3 = cJSON_CreateObject();
    if (ch2 && ch3) {
      uint8_t maddr = 1;
      nvs_get_u8(nvs_handle, "ac2_maddr", &maddr);
      cJSON_AddNumberToObject(ch2, "mapped_slave_addr", maddr);
      int32_t cnt = 0;
      nvs_get_i32(nvs_handle, "ac2_count", &cnt);
      cJSON *items = cJSON_AddArrayToObject(ch2, "items");
      if (items && cnt > 0) {
        if (cnt > 60) cnt = 60;
        for (int i = 0; i < cnt; i++) {
          cJSON *it = cJSON_CreateObject();
          if (!it) continue;
          char key[32];
          uint8_t u8; uint16_t u16; uint32_t u32;
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_en",i), key), &u8);
          cJSON_AddBoolToObject(it, "enabled", u8 != 0);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_rs",i), key), &u8);
          cJSON_AddNumberToObject(it, "real_slave_addr", u8);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_fc",i), key), &u8);
          cJSON_AddNumberToObject(it, "function_code", u8);
          nvs_get_u16(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_ra",i), key), &u16);
          cJSON_AddNumberToObject(it, "register_addr", u16);
          uint16_t mra = u16;
          if (nvs_get_u16(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_mra",i), key), &u16) == ESP_OK) {
            mra = u16;
          }
          cJSON_AddNumberToObject(it, "mapped_register_addr", mra);
          nvs_get_u16(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_rn",i), key), &u16);
          cJSON_AddNumberToObject(it, "register_num", u16);
          nvs_get_u32(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_int",i), key), &u32);
          cJSON_AddNumberToObject(it, "interval_ms", u32);
          nvs_get_u32(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_to",i), key), &u32);
          cJSON_AddNumberToObject(it, "timeout_ms", u32);
          // UART
          nvs_get_u32(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_baud",i), key), &u32);
          cJSON_AddNumberToObject(it, "baudrate", u32);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_db",i), key), &u8);
          cJSON_AddNumberToObject(it, "data_bits", u8);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_par",i), key), &u8);
          cJSON_AddNumberToObject(it, "parity", u8);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac2_item%d_sb",i), key), &u8);
          cJSON_AddNumberToObject(it, "stop_bits", u8);
          cJSON_AddItemToArray(items, it);
        }
      }

      maddr = 1;
      nvs_get_u8(nvs_handle, "ac3_maddr", &maddr);
      cJSON_AddNumberToObject(ch3, "mapped_slave_addr", maddr);
      cnt = 0;
      nvs_get_i32(nvs_handle, "ac3_count", &cnt);
      items = cJSON_AddArrayToObject(ch3, "items");
      if (items && cnt > 0) {
        if (cnt > 60) cnt = 60;
        for (int i = 0; i < cnt; i++) {
          cJSON *it = cJSON_CreateObject();
          if (!it) continue;
          char key[32];
          uint8_t u8; uint16_t u16; uint32_t u32;
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_en",i), key), &u8);
          cJSON_AddBoolToObject(it, "enabled", u8 != 0);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_rs",i), key), &u8);
          cJSON_AddNumberToObject(it, "real_slave_addr", u8);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_fc",i), key), &u8);
          cJSON_AddNumberToObject(it, "function_code", u8);
          nvs_get_u16(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_ra",i), key), &u16);
          cJSON_AddNumberToObject(it, "register_addr", u16);
          uint16_t mra3 = u16;
          if (nvs_get_u16(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_mra",i), key), &u16) == ESP_OK) {
            mra3 = u16;
          }
          cJSON_AddNumberToObject(it, "mapped_register_addr", mra3);
          nvs_get_u16(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_rn",i), key), &u16);
          cJSON_AddNumberToObject(it, "register_num", u16);
          nvs_get_u32(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_int",i), key), &u32);
          cJSON_AddNumberToObject(it, "interval_ms", u32);
          nvs_get_u32(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_to",i), key), &u32);
          cJSON_AddNumberToObject(it, "timeout_ms", u32);
          // UART
          nvs_get_u32(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_baud",i), key), &u32);
          cJSON_AddNumberToObject(it, "baudrate", u32);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_db",i), key), &u8);
          cJSON_AddNumberToObject(it, "data_bits", u8);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_par",i), key), &u8);
          cJSON_AddNumberToObject(it, "parity", u8);
          nvs_get_u8(nvs_handle, (snprintf(key,sizeof(key),"ac3_item%d_sb",i), key), &u8);
          cJSON_AddNumberToObject(it, "stop_bits", u8);
          cJSON_AddItemToArray(items, it);
        }
      }

      cJSON_AddItemToObject(response, "ch2", ch2);
      cJSON_AddItemToObject(response, "ch3", ch3);
    }
  }


  // 处理modbus_cache模式
  if (strcmp(work_mode, "modbus_cache") == 0) {
    // 获取自动透明缓存开关状态
    uint8_t auto_cache = 1; // 默认启用
    err = nvs_get_u8(nvs_handle, "auto_cache", &auto_cache);
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) {
      cJSON_AddBoolToObject(response, "auto_transparent_cache", auto_cache == 1);
    } else {
      cJSON_AddBoolToObject(response, "auto_transparent_cache", true); // 默认启用
    }
    
    // 总是返回Modbus配置项，让前端决定是否显示
    int32_t items_count = 0;
    err = nvs_get_i32(nvs_handle, "m_count", &items_count);
    if (add_modbus_items_from_blob(
            response, nvs_handle, (err == ESP_OK) ? items_count : 0) != ESP_OK) {
      if (err == ESP_OK && items_count > 0) {
        if (items_count > MODBUS_CACHE_MAX_ITEMS) {
          ESP_LOGW(TAG, "m_count超出上限(%d > %d)，已截断",
                   items_count, MODBUS_CACHE_MAX_ITEMS);
          items_count = MODBUS_CACHE_MAX_ITEMS;
        }
        cJSON *modbus_items = cJSON_AddArrayToObject(response, "modbus_items");
        if (modbus_items == NULL) {
          cJSON_Delete(response);
          nvs_close(nvs_handle);
          httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                              "Failed to create modbus items array");
          return ESP_FAIL;
        }

        for (int i = 0; i < items_count; i++) {
          cJSON *item = cJSON_CreateObject();
          if (item == NULL) {
            cJSON_Delete(response);
            nvs_close(nvs_handle);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                "Failed to create item object");
            return ESP_FAIL;
          }

          // 读取启用状态
          uint8_t enabled = 0;
          char enabled_key[16];
          snprintf(enabled_key, sizeof(enabled_key), "m%d_en", i);
          nvs_get_u8(nvs_handle, enabled_key, &enabled);
          cJSON_AddBoolToObject(item, "enabled", enabled == 1);

          // 读取其他配置项
          char key[16];
          char value[32];
          size_t value_size = sizeof(value);

          const char *fields[] = {"s_addr",   "f_code",   "r_addr",
                                  "r_num",    "m_s_addr", "m_r_addr",
                                  "timeout",  "d_fmt",    "i_time",
                                  "r_fmt",    "baud_rate", "data_bit",
                                  "stop_bit", "check_bit"};
          const char *json_fields[] = {
              "slave_addr", "function_code", "register_addr", "register_num",
              "mapped_slave_addr", "mapped_register_addr", "timeout",
              "data_format", "interval_time", "report_format", "baud_rate",
              "data_bit", "stop_bit", "check_bit"};

          for (int j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
            value_size = sizeof(value); // 重置大小
            snprintf(key, sizeof(key), "m%d%s", i, fields[j]);
            if (nvs_get_str(nvs_handle, key, value, &value_size) == ESP_OK) {
              cJSON_AddStringToObject(item, json_fields[j], value);
            }
          }

          cJSON_AddItemToArray(modbus_items, item);
        }
      }
    }
  } else if (strcmp(work_mode, "modbus_rtu") == 0) {
    int32_t items_count = 0;
    err = nvs_get_i32(nvs_handle, "m_count", &items_count);
    if (err == ESP_OK && items_count > 0) {
      if (items_count > MODBUS_CACHE_MAX_ITEMS) {
        ESP_LOGW(TAG, "m_count超出上限(%d > %d)，已截断",
                 items_count, MODBUS_CACHE_MAX_ITEMS);
        items_count = MODBUS_CACHE_MAX_ITEMS;
      }
      cJSON *modbus_items = cJSON_AddArrayToObject(response, "modbus_items");
      if (modbus_items == NULL) {
        cJSON_Delete(response);
        nvs_close(nvs_handle);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Failed to create modbus items array");
        return ESP_FAIL;
      }

      for (int i = 0; i < items_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
          cJSON_Delete(response);
          nvs_close(nvs_handle);
          httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                              "Failed to create item object");
          return ESP_FAIL;
        }

        // 读取启用状态
        uint8_t enabled = 0;
        char enabled_key[16];
        snprintf(enabled_key, sizeof(enabled_key), "m%d_en", i);
        nvs_get_u8(nvs_handle, enabled_key, &enabled);
        cJSON_AddBoolToObject(item, "enabled", enabled == 1);

        // 读取其他配置项
        char key[16];
        char value[32];
        size_t value_size = sizeof(value);

        const char *fields[] = {"s_addr",   "f_code",   "r_addr",
                                "r_num",    "m_s_addr", "m_r_addr",
                                "timeout",  "d_fmt",    "i_time",   
                                "r_fmt",    "baud_rate", "data_bit", 
                                "stop_bit", "check_bit"};
        const char *json_fields[] = {
            "slave_addr", "function_code", "register_addr", "register_num",
            "mapped_slave_addr", "mapped_register_addr", "timeout", "data_format",   
            "interval_time", "report_format", "baud_rate",  "data_bit",      
            "stop_bit",      "check_bit"};

        for (int j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
          value_size = sizeof(value); // 重置大小
          snprintf(key, sizeof(key), "m%d%s", i, fields[j]);
          if (nvs_get_str(nvs_handle, key, value, &value_size) == ESP_OK) {
            cJSON_AddStringToObject(item, json_fields[j], value);
          }
        }

        cJSON_AddItemToArray(modbus_items, item);
      }
    }
  }

  char *json_str = cJSON_Print(response);
  if (json_str == NULL) {
    cJSON_Delete(response);
    nvs_close(nvs_handle);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to print JSON");
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json_str);

  free(json_str);
  cJSON_Delete(response);
  nvs_close(nvs_handle);

  return ESP_OK;
#endif
}






// WiFi诊断测试处理器
static esp_err_t get_wifi_test_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();

  ESP_LOGI(TAG, "WiFi diagnostic test requested");

  // 测试1: WiFi硬件状态
  wifi_mode_t mode;
  esp_err_t ret = esp_wifi_get_mode(&mode);
  cJSON_AddStringToObject(root, "wifi_mode_status", ret == ESP_OK ? "OK" : esp_err_to_name(ret));
  cJSON_AddNumberToObject(root, "wifi_mode", mode);

  // 测试2: MAC地址
  uint8_t mac[6];
  ret = esp_wifi_get_mac(WIFI_IF_STA, mac);
  if (ret == ESP_OK) {
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "mac_address", mac_str);
  } else {
    cJSON_AddStringToObject(root, "mac_address", "ERROR");
  }

  // 测试3: 简单扫描测试
  uint16_t ap_count = 0;
  ret = esp_wifi_scan_get_ap_num(&ap_count);
  cJSON_AddStringToObject(root, "last_scan_status", esp_err_to_name(ret));
  cJSON_AddNumberToObject(root, "last_scan_count", ap_count);

  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(root);
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  cJSON_Delete(root);

  return ESP_OK;
}

// 获取串口参数配置模式 GET Handler
static esp_err_t get_serial_config_mode_info_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "Getting serial config mode");
  
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  
  size_t required_size = 0;
  // 先获取需要的大小
  ret = nvs_get_str(nvs_handle, "serial_mode", NULL, &required_size);
  
  char *config_mode = NULL;
  if (ret == ESP_OK && required_size > 0) {
    config_mode = malloc(required_size);
    if (config_mode) {
      ret = nvs_get_str(nvs_handle, "serial_mode", config_mode, &required_size);
      if (ret != ESP_OK) {
        free(config_mode);
        config_mode = NULL;
      }
    }
  }
  
  // 如果获取失败或为空，使用默认值
  if (!config_mode) {
    config_mode = strdup("unified"); // 默认为统一配置
    // 保存默认值到NVS
    nvs_set_str(nvs_handle, "serial_mode", config_mode);
    nvs_commit(nvs_handle);
  }
  
  nvs_close(nvs_handle);
  
  // 构建JSON响应
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "config_mode", config_mode);
  
  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(root);
  httpd_resp_send(req, json_data, strlen(json_data));
  
  // 清理资源
  free(json_data);
  free(config_mode);
  cJSON_Delete(root);
  
  ESP_LOGI(TAG, "Serial config mode response sent");
  return ESP_OK;
}

// 设置串口参数配置模式 POST Handler
static esp_err_t get_serial_config_mode_set_handler(httpd_req_t *req) {
  char buf[1024];
  int ret, remaining = req->content_len;
  
  ESP_LOGI(TAG, "Setting serial config mode, content length: %d", remaining);
  
  if (remaining >= sizeof(buf)) {
    ESP_LOGE(TAG, "Content length too large");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  // 读取请求数据
  if ((ret = httpd_req_recv(req, buf, remaining)) <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_408(req);
    }
    return ESP_FAIL;
  }
  buf[ret] = '\0';
  
  ESP_LOGI(TAG, "Received serial config mode data: %s", buf);
  
  // 解析JSON
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    ESP_LOGE(TAG, "Failed to parse JSON");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  cJSON *config_mode = cJSON_GetObjectItem(json, "config_mode");
  if (!cJSON_IsString(config_mode)) {
    ESP_LOGE(TAG, "config_mode is not a string");
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  const char *mode = config_mode->valuestring;
  ESP_LOGI(TAG, "Setting serial config mode to: %s", mode);
  
  // 验证配置模式值
  if (strcmp(mode, "unified") != 0 && 
      strcmp(mode, "separate") != 0 && 
      strcmp(mode, "slave_follow") != 0) {
    ESP_LOGE(TAG, "Invalid config mode: %s", mode);
    cJSON_Delete(json);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_send(req, "Invalid config mode", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }
  
  // 保存到NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  err = nvs_set_str(nvs_handle, "serial_mode", mode);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error setting serial config mode: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    cJSON_Delete(json);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  // 同时保存为serialSyncMode以保持与前端的兼容性
  err = nvs_set_str(nvs_handle, "serialSyncMode", mode);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error setting serialSyncMode: %s", esp_err_to_name(err));
    // 这不是致命错误，继续执行
  }
  
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(err));
  }
  nvs_close(nvs_handle);
  
  // 立即更新运行时的UART配置模式
  if (strcmp(mode, "slave_follow") == 0) {
    set_uart_config_mode(UART_CONFIG_MODE_SLAVE_FOLLOW);
    ESP_LOGI(TAG, "运行时UART配置模式已更新为：从机跟随模式");
  } else {
    set_uart_config_mode(UART_CONFIG_MODE_NORMAL);
    ESP_LOGI(TAG, "运行时UART配置模式已更新为：正常模式");
  }
  
  // 构建成功响应
  cJSON *response = cJSON_CreateObject();
  cJSON_AddNumberToObject(response, "code", 200);
  cJSON_AddStringToObject(response, "message", "Serial config mode saved successfully");
  cJSON_AddStringToObject(response, "config_mode", mode);
  
  httpd_resp_set_type(req, "application/json");
  char *json_response = cJSON_Print(response);
  httpd_resp_send(req, json_response, strlen(json_response));
  
  // 清理资源
  free(json_response);
  cJSON_Delete(response);
  cJSON_Delete(json);
  
  ESP_LOGI(TAG, "Serial config mode set successfully: %s", mode);
  return ESP_OK;
}

static esp_err_t get_find_wifi_get_handler(httpd_req_t *req) {

  WifiValueList data = wifiSearchValue();
  ESP_LOGI(TAG, "WiFi scan completed - Total APs scanned = %u, error_code = %d", data.ap_count, data.error_code);
  cJSON *root = cJSON_CreateObject();
  
  ESP_LOGI(TAG, "Processing WiFi scan result: ap_count=%d, error_code=%d", data.ap_count, data.error_code);

  // 检查错误状态并返回相应的错误信息
  if (data.error_code > 0) {
    switch (data.error_code) {
    case 1: // 扫描间隔过短
      ESP_LOGI(TAG, "WiFi scan refused: interval too short, need to wait %d ms",
               data.wait_time);
      cJSON_AddStringToObject(root, "error", "扫描间隔过短，请稍后再试");
      cJSON_AddNumberToObject(root, "wait_time", data.wait_time);
      break;
    case 2: // 扫描不允许
      ESP_LOGI(TAG, "WiFi scan refused: not allowed at this time");
      cJSON_AddStringToObject(root, "error",
                              "WiFi正在重连中，请稍后再试");
      cJSON_AddNumberToObject(root, "wait_time", 3000); // 默认等待3秒
      break;
    case 4: // 超时错误
      ESP_LOGI(TAG, "WiFi scan failed: timeout");
      cJSON_AddStringToObject(root, "error", "WiFi扫描超时，请检查天线连接或稍后重试");
      cJSON_AddNumberToObject(root, "wait_time", 5000); // 超时等待5秒
      break;
    case 3: // 其他错误
    default:
      ESP_LOGI(TAG, "WiFi scan failed: other error");
      cJSON_AddStringToObject(root, "error", "WiFi扫描失败，可能是系统忙碌，请稍后重试");
      cJSON_AddNumberToObject(root, "wait_time", 2000); // 默认等待2秒
      break;
    }
  } else if (data.ap_count == 0) {
    // 扫描成功但未找到WiFi
    ESP_LOGI(TAG, "Path: ap_count == 0 - WiFi scan completed but no networks found");
    cJSON_AddStringToObject(root, "message", "未找到任何WiFi网络");
  } else {
    // 扫描成功，返回AP列表
    ESP_LOGI(TAG, "Path: ap_count > 0 - Processing %d WiFi networks", data.ap_count);
    // 确保不会超出数组边界
    int count = (data.ap_count < DEFAULT_SCAN_LIST_SIZE)
                    ? data.ap_count
                    : DEFAULT_SCAN_LIST_SIZE;
    ESP_LOGI(TAG, "Processing count: %d (limited from %d)", count, data.ap_count);
    
    int added_count = 0;
    for (int i = 0; i < count; i++) {
      ESP_LOGI(TAG, "Processing AP[%d]: SSID='%s', RSSI=%d, Channel=%d", 
               i, data.ap_info[i].ssid, data.ap_info[i].rssi, data.ap_info[i].primary);
      
      // 检查SSID是否为空
      if (strlen((char *)data.ap_info[i].ssid) > 0) {
        // 直接使用SSID作为键名，cJSON库会自动处理特殊字符的转义
        cJSON_AddNumberToObject(root, (char *)data.ap_info[i].ssid, data.ap_info[i].rssi);
        added_count++;
        ESP_LOGI(TAG, "Added AP to JSON: '%s' -> %d", data.ap_info[i].ssid, data.ap_info[i].rssi);
      } else {
        ESP_LOGW(TAG, "Skipped AP[%d] - empty SSID", i);
      }
    }
    ESP_LOGI(TAG, "Total APs added to JSON response: %d", added_count);
  }

  // 统一使用cJSON响应
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Connection", "close");
  
  char *json_data = cJSON_Print(root);
  if (json_data) {
    ESP_LOGI(TAG, "Sending JSON response: %s", json_data);
    esp_err_t send_result = httpd_resp_send(req, json_data, strlen(json_data));
    ESP_LOGI(TAG, "httpd_resp_send result: %s", esp_err_to_name(send_result));
    free(json_data);
  } else {
    ESP_LOGE(TAG, "Failed to print JSON data");
    httpd_resp_send_500(req);
  }
  
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t get_exit_ap_get_handler(httpd_req_t *req) {
  httpd_resp_send(req, "EXIT AP", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(500);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  return ESP_OK;
}

static esp_err_t get_net_set_info_get_handler(httpd_req_t *req) {

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  // 读取字符串
  // 首先获取所需的缓冲区大小
  size_t netconn, is_dhcp, static_ip, static_netmask, static_gateway, static_dns1, static_dns2, wifi_ssid,
      wifi_password = 0;
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
    // printf("NET set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    printf("no NET set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", "2")); // 默认使用WiFi
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_ip", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_netmask", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_gateway", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns1", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns2", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_password", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    // 分配缓冲区
    char *nvs_netconn = malloc(netconn);
    char *nvs_is_dhcp = malloc(is_dhcp);
    char *nvs_static_ip = malloc(static_ip);
    char *nvs_static_netmask = malloc(static_netmask);
    char *nvs_static_gateway = malloc(static_gateway);
    char *nvs_static_dns1 = malloc(static_dns1);
    char *nvs_static_dns2 = malloc(static_dns2);
    char *nvs_wifi_ssid = malloc(wifi_ssid);
    char *nvs_wifi_password = malloc(wifi_password);
    if (nvs_netconn == NULL) {
      printf("Memory allocation failed\n");
    } else {
      // 读取字符串到分配的缓冲区
      ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      nvs_get_str(nvs_handle, "static_ip", nvs_static_ip, &static_ip);
      nvs_get_str(nvs_handle, "static_netmask", nvs_static_netmask,
                  &static_netmask);
      nvs_get_str(nvs_handle, "static_gateway", nvs_static_gateway,
                  &static_gateway);
      nvs_get_str(nvs_handle, "static_dns1", nvs_static_dns1,
                  &static_dns1);
      nvs_get_str(nvs_handle, "static_dns2", nvs_static_dns2,
                  &static_dns2);
      nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
      nvs_get_str(nvs_handle, "wifi_password", nvs_wifi_password,
                  &wifi_password);

      if (ret == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "netconn", cJSON_CreateString(nvs_netconn));
        cJSON_AddItemToObject(root, "is_dhcp", cJSON_CreateString(nvs_is_dhcp));
        cJSON_AddItemToObject(root, "static_ip",
                              cJSON_CreateString(nvs_static_ip));
        cJSON_AddItemToObject(root, "static_netmask",
                              cJSON_CreateString(nvs_static_netmask));
        cJSON_AddItemToObject(root, "static_gateway",
                              cJSON_CreateString(nvs_static_gateway));
        cJSON_AddItemToObject(root, "static_dns1",
                              cJSON_CreateString(nvs_static_dns1));
        cJSON_AddItemToObject(root, "static_dns2",
                              cJSON_CreateString(nvs_static_dns2));
        cJSON_AddItemToObject(root, "wifi_ssid",
                              cJSON_CreateString(nvs_wifi_ssid));
        cJSON_AddItemToObject(root, "wifi_password",
                              cJSON_CreateString(nvs_wifi_password));
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));

        free(json_data);
        cJSON_Delete(root);

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_netconn);
      free(nvs_is_dhcp);
      free(nvs_static_ip);
      free(nvs_static_netmask);
      free(nvs_static_gateway);
      free(nvs_static_dns1);
      free(nvs_static_dns2);
      free(nvs_wifi_ssid);
      free(nvs_wifi_password);
    }
  }

  nvs_close(nvs_handle);

  return ESP_OK;
}

static esp_err_t get_ap_set_info_get_handler(httpd_req_t *req) {
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  // 读取AP超时配置
  size_t ap_timeout = 0;
  ret = nvs_get_str(nvs_handle, "ap_timeout", NULL, &ap_timeout);
  
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    // 如果没有找到配置，设置默认值
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_timeout", "30")); // 默认30分钟
    ESP_ERROR_CHECK(nvs_commit(nvs_handle)); // 提交更改
    ap_timeout = 3; // "30" + null terminator
    break;
  default:
    break;
  }

  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    printf("Error getting ap_timeout: %s\n", esp_err_to_name(ret));
    nvs_close(nvs_handle);
    return ESP_FAIL;
  } else {
    // 分配缓冲区并读取数据
    char *nvs_ap_timeout = malloc(ap_timeout);
    if (nvs_ap_timeout == NULL) {
      printf("Memory allocation failed for ap_timeout\n");
      nvs_close(nvs_handle);
      return ESP_ERR_NO_MEM;
    }

    ret = nvs_get_str(nvs_handle, "ap_timeout", nvs_ap_timeout, &ap_timeout);
    if (ret == ESP_OK) {
      cJSON *root = cJSON_CreateObject();
      cJSON_AddItemToObject(root, "ap_timeout", cJSON_CreateString(nvs_ap_timeout));
      
      httpd_resp_set_hdr(req, "Connection", "close");
      httpd_resp_set_type(req, "application/json");
      char *json_data = cJSON_Print(root);
      httpd_resp_send(req, json_data, strlen(json_data));

      free(json_data);
      cJSON_Delete(root);
    } else {
      printf("Error reading ap_timeout: %s\n", esp_err_to_name(ret));
    }
    
    free(nvs_ap_timeout);
  }

  nvs_close(nvs_handle);
  return ESP_OK;
}

static esp_err_t get_module_set_info_get_handler(httpd_req_t *req) {

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t host_names, ntp_server, lgname, lgpwd = 0;

  ret = nvs_get_str(nvs_handle, "lgname", NULL, &lgname);
  nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd);
  nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server);

  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "以太网两路缓存485集线器"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ntp_server", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgname", "admin"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgpwd", "12345678"));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'host_names': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_host_names = malloc(host_names);
    char *nvs_ntp_server = malloc(ntp_server);
    char *nvs_lgname = malloc(lgname);
    char *nvs_lgpwd = malloc(lgpwd);
    if (nvs_host_names == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "host_names", nvs_host_names, &host_names);
      nvs_get_str(nvs_handle, "ntp_server", nvs_ntp_server, &ntp_server);
      nvs_get_str(nvs_handle, "lgname", nvs_lgname, &lgname);
      nvs_get_str(nvs_handle, "lgpwd", nvs_lgpwd, &lgpwd);

      if (ret == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "host_names",
                              cJSON_CreateString(nvs_host_names));
        cJSON_AddItemToObject(root, "ntp_server",
                              cJSON_CreateString(nvs_ntp_server));
        cJSON_AddItemToObject(root, "lgname", cJSON_CreateString(nvs_lgname));
        cJSON_AddItemToObject(root, "lgpwd", cJSON_CreateString(nvs_lgpwd));
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));

        free(json_data);
        cJSON_Delete(root);

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_host_names);
      free(nvs_ntp_server);
      free(nvs_lgname);
      free(nvs_lgpwd);
    }
  }

  nvs_close(nvs_handle);

  return ESP_OK;
}



static void *web_psram_calloc(size_t count, size_t size) {
  void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr == NULL) {
    ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
  }
  return ptr;
}

static bool ensure_websocket_buffers(void) {
  if (ws_clients == NULL) {
    ws_clients = web_psram_calloc(MAX_WS_CLIENTS, sizeof(ws_client_t));
  }
  if (ws_queue_storage == NULL) {
    ws_queue_storage = web_psram_calloc(WS_QUEUE_SIZE, WS_MAX_MSG_LEN);
  }
  if (ws_queue_struct == NULL) {
    ws_queue_struct = web_psram_calloc(1, sizeof(StaticQueue_t));
  }
  if (ws_msg_buffer == NULL) {
    ws_msg_buffer = web_psram_calloc(1, WS_MAX_MSG_LEN);
  }
  return ws_clients != NULL && ws_queue_storage != NULL &&
         ws_queue_struct != NULL && ws_msg_buffer != NULL;
}

static void ws_send_task(void *pvParameters) {
  if (!ensure_websocket_buffers()) {
    ESP_LOGE(TAG, "WebSocket buffers unavailable, send task exits");
    delete_self_app_task_with_caps();
    return;
  }

  httpd_ws_frame_t ws_pkt = {
      .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)ws_msg_buffer, .len = 0};
  
  TickType_t last_cleanup_time = xTaskGetTickCount();
  const TickType_t cleanup_interval = pdMS_TO_TICKS(30000); // 30秒检查一次

  while (1) {
    if (xQueueReceive(ws_msg_queue, ws_msg_buffer, pdMS_TO_TICKS(1000))) {
      ws_pkt.len = strlen(ws_msg_buffer);

      // 遍历所有客户端发送消息
      for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].in_use) {
          esp_err_t ret = httpd_ws_send_frame_async(
              http_server, ws_clients[i].client_fd, &ws_pkt);
          if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send WS message to client %d: %d", i, ret);
            // 如果发送失败，移除客户端
            ws_clients[i].in_use = false;
            ESP_LOGI(TAG, "Removed disconnected WS client, slot: %d", i);
          }
        }
      }
    }
    
    // 定期清理断开的连接
    TickType_t current_time = xTaskGetTickCount();
    if (current_time - last_cleanup_time >= cleanup_interval) {
      ESP_LOGD(TAG, "Performing periodic WebSocket cleanup");
      last_cleanup_time = current_time;
      
      // 这里可以添加额外的连接状态检查逻辑
      // 例如发送PING消息来检测连接状态
    }
  }
}

// 初始化WebSocket系统
static void init_websocket(void) {
  if (!ensure_websocket_buffers()) {
    ESP_LOGE(TAG, "Failed to allocate WS buffers (%d items, %d bytes)", WS_QUEUE_SIZE,
             WS_MAX_MSG_LEN);
    return;
  }

  // 创建消息队列 - 使用PSRAM缓冲区
  ws_msg_queue = xQueueCreateStatic(WS_QUEUE_SIZE, WS_MAX_MSG_LEN,
                                    ws_queue_storage, ws_queue_struct);
  if (ws_msg_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create WS message queue (%d items, %d bytes)", WS_QUEUE_SIZE, WS_MAX_MSG_LEN);
    return;
  }
  ESP_LOGI(TAG, "WS message queue created successfully (%d items, %d bytes each)", WS_QUEUE_SIZE, WS_MAX_MSG_LEN);

  // 创建WebSocket发送任务
  BaseType_t ret = create_app_task_psram(ws_send_task, "ws_send_task",
                                         8192, // 更宽裕的PSRAM栈空间
                                         NULL, 10, // WebSocket任务优先级
                                         &ws_task_handle, SX_NETWORK_CORE_ID);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create WS task, trying smaller stack");
    // 尝试更小的栈大小
    ret = create_app_task_psram(ws_send_task, "ws_send_task",
                                4096, // 4KB栈空间
                                NULL, 10, &ws_task_handle,
                                SX_NETWORK_CORE_ID);
    if (ret != pdPASS) {
      ESP_LOGE(TAG, "Failed to create WS task with reduced stack");
      vQueueDelete(ws_msg_queue);
      ws_msg_queue = NULL;
      return;
    } else {
      ESP_LOGW(TAG, "WS task created with reduced stack (2KB)");
    }
  } else {
    ESP_LOGI(TAG, "WS task created successfully");
  }
  
  ESP_LOGI(TAG, "WebSocket system initialized - Queue: %s, Task: %s", 
           ws_msg_queue ? "OK" : "FAIL", 
           ws_task_handle ? "OK" : "FAIL");
}

// WebSocket处理函数
static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    // 查找可用的客户端槽位
    int client_slot = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
      if (!ws_clients[i].in_use) {
        client_slot = i;
        break;
      }
    }

    if (client_slot >= 0) {
      ws_clients[client_slot].client_fd = httpd_req_to_sockfd(req);
      ws_clients[client_slot].in_use = true;
      ESP_LOGI(TAG, "New WS client connected, slot: %d", client_slot);
    } else {
      ESP_LOGW(TAG, "No free WS client slots");
      return ESP_FAIL;
    }
    return ESP_OK;
  } else if (req->method == HTTP_POST) {
    // 处理WebSocket消息
    ESP_LOGI(TAG, "WebSocket message received");
    
    // 检查客户端是否仍然连接
    int client_fd = httpd_req_to_sockfd(req);
    bool client_found = false;
    
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
      if (ws_clients[i].in_use && ws_clients[i].client_fd == client_fd) {
        client_found = true;
        break;
      }
    }
    
    if (!client_found) {
      ESP_LOGW(TAG, "WebSocket client not found in active list");
      return ESP_FAIL;
    }
  }

  return ESP_OK;
}



// UART端口号到通道号的映射函数
static int uart_port_to_channel(uart_port_t uart_num) {
  switch (uart_num) {
    case UART_NUM_0: return 2; // CH2
    case UART_NUM_1: return 3; // CH3
    case UART_NUM_2: return 1; // CH1
    default: return 0;         // 未知通道
  }
}

// 发送串口数据到WebSocket的函数
void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx, int channel) {
  if (!ws_msg_queue) {
    return;
  }

  // 创建JSON对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "Failed to create JSON object for UART data");
    return;
  }

  // 添加数据类型标识
  cJSON_AddStringToObject(root, "type", "uart_data");
  cJSON_AddBoolToObject(root, "is_tx", is_tx);
  cJSON_AddNumberToObject(root, "channel", channel);

  // 添加时间戳（使用统一时间管理器）
  uint64_t timestamp = time_manager_get_current_us();
  cJSON_AddNumberToObject(root, "timestamp", (double)timestamp);

  char *hex_str = web_psram_calloc(1, WS_MAX_MSG_LEN);
  char *ascii_str = web_psram_calloc(1, WS_MAX_MSG_LEN);
  char *temp_buffer = web_psram_calloc(1, WS_MAX_MSG_LEN);
  if (hex_str == NULL || ascii_str == NULL || temp_buffer == NULL) {
    ESP_LOGE(TAG, "Failed to allocate WS UART conversion buffers");
    free(hex_str);
    free(ascii_str);
    free(temp_buffer);
    cJSON_Delete(root);
    return;
  }

  // 构建十六进制字符串
  size_t hex_pos = 0;
  for (int i = 0; i < len && hex_pos < WS_MAX_MSG_LEN - 3; i++) {
    hex_pos += snprintf(hex_str + hex_pos, WS_MAX_MSG_LEN - hex_pos,
                      "%02X ", data[i]);
  }
  cJSON_AddStringToObject(root, "hex", hex_str);

  // 构建ASCII字符串
  for (int i = 0; i < len && i < WS_MAX_MSG_LEN - 1; i++) {
    ascii_str[i] = isprint(data[i]) ? data[i] : '.';
  }
  ascii_str[len < WS_MAX_MSG_LEN ? len : WS_MAX_MSG_LEN - 1] = '\0';
  cJSON_AddStringToObject(root, "ascii", ascii_str);

  // 转换为字符串
  char *json_str = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);

  if (json_str) {
    size_t json_len = strlen(json_str);
    if (json_len >= WS_MAX_MSG_LEN) {
      ESP_LOGW(TAG, "WS message too large (%zu bytes), dropping", json_len);
      free(json_str);
      free(hex_str);
      free(ascii_str);
      free(temp_buffer);
      return;
    }

    memcpy(temp_buffer, json_str, json_len + 1);
    free(json_str);

    // 使用超时时间发送到队列
    if (xQueueSend(ws_msg_queue, temp_buffer, pdMS_TO_TICKS(100)) != pdPASS) {
      ESP_LOGW(TAG, "WS message queue full");
    }
  }

  free(hex_str);
  free(ascii_str);
  free(temp_buffer);
}

// 检查WebSocket是否可用
bool is_websocket_ready(void) {
  return (ws_msg_queue != NULL && ws_task_handle != NULL);
}

// 从UART端口号自动获取通道号并发送到WebSocket
void send_uart_to_websocket_from_port(const uint8_t *data, size_t len, bool is_tx, uart_port_t uart_num) {
  if (!is_websocket_ready()) {
    ESP_LOGD(TAG, "WebSocket not ready, skipping message");
    return;
  }
  
  int channel = uart_port_to_channel(uart_num);
  send_uart_to_websocket(data, len, is_tx, channel);
}


/* 登录处理函数 */
static esp_err_t web_login_handler(httpd_req_t *req) {
  char buf[1024];
  int ret, remaining = req->content_len;

  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf, sizeof(buf))) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    remaining -= ret;
    ESP_LOGI(TAG, "%.*s", ret, buf);
  }
  printf("%s\n", buf);
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      ESP_LOGE("JSON", "Error before: %s", error_ptr);
    }
    return ESP_FAIL;
  }
  cJSON *username = cJSON_GetObjectItemCaseSensitive(json, "username");
  if (cJSON_IsString(username) && (username->valuestring != NULL)) {
    ESP_LOGI("JSON", "username: %s", username->valuestring);
  }
  cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
  if (cJSON_IsString(password) && (password->valuestring != NULL)) {
    ESP_LOGI("JSON", "password: %s", password->valuestring);
  }

  esp_err_t rets = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t host_names, ntp_server, lgname, lgpwd = 0;

  rets = nvs_get_str(nvs_handle, "lgname", NULL, &lgname);
  nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd);
  nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server);

  switch (rets) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "以太网两路缓存485集线器"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ntp_server", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgname", "admin"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgpwd", "12345678"));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    // 重新获取大小
    nvs_get_str(nvs_handle, "lgname", NULL, &lgname);
    nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd);
    nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
    nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server);
    break;
  default:
    break;
  }
  if (rets != ESP_OK) {
    printf("Error getting size of 'host_names': %s\n", esp_err_to_name(rets));
  } else {
    char *nvs_host_names = malloc(host_names);
    char *nvs_ntp_server = malloc(ntp_server);
    char *nvs_lgname = malloc(lgname);
    char *nvs_lgpwd = malloc(lgpwd);
    if (nvs_host_names == NULL) {
      printf("Memory allocation failed\n");
    } else {
      rets = nvs_get_str(nvs_handle, "host_names", nvs_host_names, &host_names);
      nvs_get_str(nvs_handle, "ntp_server", nvs_ntp_server, &ntp_server);
      nvs_get_str(nvs_handle, "lgname", nvs_lgname, &lgname);
      nvs_get_str(nvs_handle, "lgpwd", nvs_lgpwd, &lgpwd);

      if (rets == ESP_OK) {
        printf("Value for 'host_names' is %s\n", nvs_host_names);
        printf("Value for 'ntp_server' is %s\n", nvs_ntp_server);
        printf("Value for 'lgname' is %s\n", nvs_lgname);
        printf("Value for 'lgpwd' is %s\n", nvs_lgpwd);
        printf("Comparing credentials:\n");
        printf("Received username: '%s'\n", username->valuestring);
        printf("Received password: '%s'\n", password->valuestring);
        printf("Stored username: '%s'\n", nvs_lgname);
        printf("Stored password: '%s'\n", nvs_lgpwd);
        
        if (strcmp(username->valuestring, nvs_lgname) == 0 &&
            strcmp(password->valuestring, nvs_lgpwd) == 0) {
          printf("login success\n");
          httpd_resp_send(req, "success", HTTPD_RESP_USE_STRLEN);
        } else {
          printf("login fail - credentials do not match\n");
          httpd_resp_send(req, "fail", HTTPD_RESP_USE_STRLEN);
        }
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(rets));
      }
      free(nvs_host_names);
      free(nvs_ntp_server);
      free(nvs_lgname);
      free(nvs_lgpwd);
    }
  }
  nvs_close(nvs_handle);
  cJSON_Delete(json);
  return ESP_OK;
}

static const httpd_uri_t root = {
    .uri = "/", .method = HTTP_GET, .handler = root_get_handler};

static const httpd_uri_t get_status = {
    .uri = "/status", .method = HTTP_GET, .handler = get_status_get_handler};

static const httpd_uri_t js_uri = {
    .uri = "/web.js", .method = HTTP_GET, .handler = js_get_handler};

static const httpd_uri_t css_uri = {
    .uri = "/web.css", .method = HTTP_GET, .handler = css_get_handler};

static const httpd_uri_t get_devinfo = {
    .uri = "/devinfo", .method = HTTP_GET, .handler = get_devinfo_get_handler};

static const httpd_uri_t get_sys = {
    .uri = "/sys", .method = HTTP_GET, .handler = get_sys_get_handler};

static const httpd_uri_t get_operate = {
    .uri = "/operate", .method = HTTP_GET, .handler = get_operate_get_handler};

static const httpd_uri_t get_restore = {
    .uri = "/restore", .method = HTTP_GET, .handler = get_restore_get_handler};

static const httpd_uri_t get_module_set = {.uri = "/module_set",
                                           .method = HTTP_POST,
                                           .handler =
                                               get_module_set_post_handler};

static const httpd_uri_t get_module_set_info = {
    .uri = "/module_set_info",
    .method = HTTP_GET,
    .handler = get_module_set_info_get_handler};


static const httpd_uri_t get_net_set = {.uri = "/net_set",
                                        .method = HTTP_POST,
                                        .handler = get_net_set_post_handler};

static const httpd_uri_t get_net_set_info = {.uri = "/net_set_info",
                                             .method = HTTP_GET,
                                             .handler =
                                                 get_net_set_info_get_handler};

static const httpd_uri_t get_ap_set_info = {.uri = "/ap_set_info",
                                            .method = HTTP_GET,
                                            .handler =
                                                get_ap_set_info_get_handler};







static const httpd_uri_t get_update = {
    .uri = "/update", .method = HTTP_POST, .handler = get_update_post_handler};

static const httpd_uri_t get_ota = {
    .uri = "/ota", .method = HTTP_POST, .handler = get_ota_post_handler};

static const httpd_uri_t get_find_wifi = {.uri = "/find_wifi",
                                          .method = HTTP_GET,
                                          .handler = get_find_wifi_get_handler};

static const httpd_uri_t get_wifi_test = {.uri = "/wifi_test",
                                         .method = HTTP_GET,
                                         .handler = get_wifi_test_handler};

static const httpd_uri_t get_ap_set = {
    .uri = "/ap_set", .method = HTTP_POST, .handler = get_ap_set_post_handler};

static const httpd_uri_t get_serial_set = {.uri = "/serial_set",
                                           .method = HTTP_POST,
                                           .handler = get_serial_set_handler};

static const httpd_uri_t get_serial_ctl = {.uri = "/serial_ctl",
                                           .method = HTTP_POST,
                                           .handler = get_serial_ctl_handler};

static const httpd_uri_t get_serial_set_info = {
    .uri = "/serial_set_info",
    .method = HTTP_GET,
    .handler = get_serial_set_info_get_handler};

static const httpd_uri_t get_serial_config_mode_info = {
    .uri = "/serial_config_mode_info",
    .method = HTTP_GET,
    .handler = get_serial_config_mode_info_handler};

static const httpd_uri_t get_serial_config_mode_set = {
    .uri = "/serial_config_mode_set",
    .method = HTTP_POST,
    .handler = get_serial_config_mode_set_handler};

static const httpd_uri_t work_mode_set = {.uri = "/mode_set",
                                          .method = HTTP_POST,
                                          .handler = get_work_mode_set_handler};

static const httpd_uri_t auto_collect_set = {.uri = "/auto_collect_set",
                                             .method = HTTP_POST,
                                             .handler = auto_collect_set_handler};

static const httpd_uri_t work_mode_info = {.uri = "/mode_info",
                                           .method = HTTP_GET,
                                           .handler =
                                               get_work_mode_info_handler};

static const httpd_uri_t uart_response_uri = {.uri = "/uart_response",
                                              .method = HTTP_GET,
                                              .handler =
                                                  get_uart_response_handler,
                                              .user_ctx = NULL};

static const httpd_uri_t get_exit_ap = {
    .uri = "/exit_ap", .method = HTTP_GET, .handler = get_exit_ap_get_handler};


static const httpd_uri_t web_login = {
    .uri = "/login", .method = HTTP_POST, .handler = web_login_handler};

static const httpd_uri_t ota_progress_uri = {.uri = "/ota_progress",
                                             .method = HTTP_GET,
                                             .handler =
                                                 get_ota_progress_handler,
                                             .user_ctx = NULL};

// 强制门户检测URL路由定义
// Windows检测URL - 使用专用的NCSI处理器
static const httpd_uri_t windows_ncsi = {
    .uri = "/ncsi.txt", .method = HTTP_GET, .handler = windows_ncsi_handler};
static const httpd_uri_t windows_connecttest = {
    .uri = "/connecttest.txt", .method = HTTP_GET, .handler = windows_ncsi_handler};

// Android检测URL
static const httpd_uri_t android_generate_204 = {
    .uri = "/generate_204", .method = HTTP_GET, .handler = generate_204_handler};

// iOS/macOS检测URL
static const httpd_uri_t apple_hotspot_detect = {
    .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_portal_handler};
static const httpd_uri_t apple_library_test = {
    .uri = "/library/test/success.html", .method = HTTP_GET, .handler = captive_portal_handler};

// 通用检测URL
static const httpd_uri_t captive_portal_detect = {
    .uri = "/captive-portal-detect", .method = HTTP_GET, .handler = captive_portal_handler};
static const httpd_uri_t success_txt = {
    .uri = "/success.txt", .method = HTTP_GET, .handler = captive_portal_handler};

// 时间同步API URI定义
static const httpd_uri_t sync_time_uri = {
    .uri = "/api/sync_time",
    .method = HTTP_POST,
    .handler = sync_time_handler,
    .user_ctx = NULL
};

// WebSocket URI定义
static const httpd_uri_t ws_uri = {
    .uri = "/ws/log",
    .method = HTTP_GET,
    .handler = ws_handler,
    .user_ctx = NULL,
    .is_websocket = true
};

static httpd_handle_t start_webserver(void) {

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 10240;
  config.core_id = SX_NETWORK_CORE_ID;
  config.max_open_sockets = 13;
  config.max_uri_handlers = 56; // 增加最大URI处理器数量以支持强制门户检测URL

  config.keep_alive_enable = true;
  config.keep_alive_idle = 1;
  config.keep_alive_interval = 1;
  config.keep_alive_count = 0;
  config.lru_purge_enable = true;
  ESP_LOGI(TAG, "Starting http_server on port: '%d'", config.server_port);
  if (httpd_start(&http_server, &config) == ESP_OK) {
    ESP_LOGI(TAG, "Registering URI handlers");
    init_websocket();
    httpd_register_uri_handler(http_server, &root);
    httpd_register_uri_handler(http_server, &get_status);
    httpd_register_uri_handler(http_server, &js_uri);
    httpd_register_uri_handler(http_server, &css_uri);
    httpd_register_uri_handler(http_server, &get_devinfo);
    httpd_register_uri_handler(http_server, &get_sys);
    httpd_register_uri_handler(http_server, &get_module_set);
    httpd_register_uri_handler(http_server, &get_module_set_info);
    httpd_register_uri_handler(http_server, &get_net_set);
    httpd_register_uri_handler(http_server, &get_net_set_info);
    httpd_register_uri_handler(http_server, &get_ap_set_info);


    httpd_register_uri_handler(http_server, &get_operate);
    httpd_register_uri_handler(http_server, &get_restore);
    httpd_register_uri_handler(http_server, &get_update);
    httpd_register_uri_handler(http_server, &get_ota);
    httpd_register_uri_handler(http_server, &get_find_wifi);
    httpd_register_uri_handler(http_server, &get_wifi_test);
    httpd_register_uri_handler(http_server, &get_exit_ap);
    httpd_register_uri_handler(http_server, &get_ap_set);
    httpd_register_uri_handler(http_server, &get_ap_set_info);

    httpd_register_uri_handler(http_server, &web_login);
    httpd_register_uri_handler(http_server, &get_serial_set);
    httpd_register_uri_handler(http_server, &get_serial_set_info);
    httpd_register_uri_handler(http_server, &get_serial_config_mode_info);
    httpd_register_uri_handler(http_server, &get_serial_config_mode_set);
    httpd_register_uri_handler(http_server, &get_serial_ctl);
    httpd_register_uri_handler(http_server, &uart_response_uri);
    httpd_register_uri_handler(http_server, &work_mode_set);
    httpd_register_uri_handler(http_server, &auto_collect_set);
    httpd_register_uri_handler(http_server, &work_mode_info);
    httpd_register_uri_handler(http_server, &ota_progress_uri);
    httpd_register_uri_handler(http_server, &ws_uri);
    
    // 注册时间同步API
    httpd_register_uri_handler(http_server, &sync_time_uri);
    ESP_LOGI(TAG, "已注册时间同步API: POST /api/sync_time");

    // 注册强制门户检测URL处理器
    httpd_register_uri_handler(http_server, &windows_ncsi);
    httpd_register_uri_handler(http_server, &windows_connecttest);
    httpd_register_uri_handler(http_server, &android_generate_204);
    httpd_register_uri_handler(http_server, &apple_hotspot_detect);
    httpd_register_uri_handler(http_server, &apple_library_test);
    httpd_register_uri_handler(http_server, &captive_portal_detect);
    httpd_register_uri_handler(http_server, &success_txt);

    httpd_register_err_handler(http_server, HTTPD_404_NOT_FOUND,
                               http_404_error_handler);
  }
  return http_server;
}



void http_server_init(void) {
   printf("%s\n", "AP_OK");
  wifi_init_softap();
  start_webserver();
  start_dns_server();
  sx_log_init();
  
  ESP_LOGI(TAG, "Web服务器初始化完成");
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
