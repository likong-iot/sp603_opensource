/*
 * @Author: Orion
 * @Date: 2024-05-23 21:10:47
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 18:12:29
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
#include <inttypes.h>
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_heap_caps.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/priv/tcp_priv.h"
#include "driver/gpio.h"
#include "esp_chip_info.h"
#include "esp_crt_bundle.h"
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
#include "sx_dns_server.h"
#include "sx_http_ota.h"
#include "sx_init_eth.h"
#include "sx_log.h"
#include "sx_modbus.h"
#include "sx_tcp_client.h"
#include "sx_tcp_server.h"
#include "sx_platform.h"
#include "sx_utils.h"
#include "sx_wifi_scan.h"
#include "sx_gpio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <sys/param.h>
#include "lwip/tcp.h"
#include "mqtt_client.h"  // 添加MQTT客户端头文件

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define OTA_HTTP_BUFFER_SIZE 2048
#define WORK_MODE_BODY_MAX (64 * 1024)
#define WORK_MODE_INFO_PAGE_SIZE 10

uint8_t sta_mac[6] = {0};
uint8_t eth_mac[6] = {0};
char dIp[20];
char dNetmask[20];
char dGateway[20];
uint8_t mac_addr[6] = {0};
char device_mac[30];

char th[20] = {0};
char device_mac_end[20];
char got_ip_addrs[16];
char got_ip_netmask[16];
char got_ip_gw[16];
int httpreason = 0;

char device_sta_mac[24];
char device_eth_mac[24];
char device_name[40];

static const char *TAG = "SP501LW";

// NVS访问互斥锁，防止并发访问导致死锁
static SemaphoreHandle_t nvs_mutex = NULL;

typedef struct {
  char transfer_id[64];
  uint8_t *buffer;
  size_t length;
  size_t capacity;
  int expected_chunks;
  int received_chunks;
} serial_chunk_ctx_t;

static serial_chunk_ctx_t serial_chunk_ctx = {0};
static SemaphoreHandle_t serial_chunk_mutex = NULL;

static void serial_chunk_reset_locked(void) {
  if (serial_chunk_ctx.buffer) {
    free(serial_chunk_ctx.buffer);
  }
  memset(&serial_chunk_ctx, 0, sizeof(serial_chunk_ctx));
}

static bool serial_chunk_reserve_locked(size_t required_len) {
  if (required_len <= serial_chunk_ctx.capacity) {
    return true;
  }
  size_t new_capacity = required_len + 512;
  if (new_capacity < required_len) {
    new_capacity = required_len;
  }
  uint8_t *new_buf = realloc(serial_chunk_ctx.buffer, new_capacity);
  if (!new_buf) {
    return false;
  }
  serial_chunk_ctx.buffer = new_buf;
  serial_chunk_ctx.capacity = new_capacity;
  return true;
}

static esp_err_t decode_instruction_payload(const char *instruction_str,
                                            bool is_hex_mode,
                                            uint8_t **out_buf,
                                            size_t *out_len) {
  if (!instruction_str || !out_buf || !out_len) {
    return ESP_ERR_INVALID_ARG;
  }

  size_t raw_len = strlen(instruction_str);
  if (raw_len == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (is_hex_mode) {
    char *clean_str = malloc(raw_len + 1);
    if (!clean_str) {
      return ESP_ERR_NO_MEM;
    }

    size_t clean_len = 0;
    for (size_t i = 0; i < raw_len; i++) {
      if (instruction_str[i] != ' ') {
        clean_str[clean_len++] = instruction_str[i];
      }
    }
    clean_str[clean_len] = '\0';

    if ((clean_len % 2) != 0) {
      free(clean_str);
      return ESP_ERR_INVALID_ARG;
    }

    size_t byte_len = clean_len / 2;
    uint8_t *data = malloc(byte_len);
    if (!data) {
      free(clean_str);
      return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < clean_len; i += 2) {
      char byte_str[3] = {clean_str[i], clean_str[i + 1], '\0'};
      data[i / 2] = strtol(byte_str, NULL, 16);
    }
    free(clean_str);

    *out_buf = data;
    *out_len = byte_len;
    return ESP_OK;
  }

  uint8_t *ascii_buf = malloc(raw_len);
  if (!ascii_buf) {
    return ESP_ERR_NO_MEM;
  }
  memcpy(ascii_buf, instruction_str, raw_len);
  *out_buf = ascii_buf;
  *out_len = raw_len;
  return ESP_OK;
}

static esp_err_t process_uart_chunk(uint8_t *chunk_data, size_t chunk_len,
                                    int chunk_index, int chunk_total,
                                    const char *transfer_id) {
  if (!chunk_data || chunk_len == 0) {
    if (chunk_data) {
      free(chunk_data);
    }
    return ESP_ERR_INVALID_ARG;
  }

  bool chunk_mode = (transfer_id && chunk_total > 1 && chunk_index >= 0);
  if (!chunk_mode) {
    tx_tasks(chunk_data, chunk_len);
    free(chunk_data);
    return ESP_OK;
  }

  if (serial_chunk_mutex == NULL) {
    serial_chunk_mutex = xSemaphoreCreateMutex();
  }
  if (serial_chunk_mutex == NULL) {
    free(chunk_data);
    ESP_LOGE(TAG, "Failed to create chunk mutex");
    return ESP_ERR_NO_MEM;
  }

  if (xSemaphoreTake(serial_chunk_mutex, pdMS_TO_TICKS(200)) != pdTRUE) {
    free(chunk_data);
    ESP_LOGE(TAG, "Failed to lock chunk mutex");
    return ESP_ERR_TIMEOUT;
  }

  bool start_new =
      (chunk_index == 0) ||
      strncmp(serial_chunk_ctx.transfer_id, transfer_id,
              sizeof(serial_chunk_ctx.transfer_id) - 1) != 0;

  if (start_new) {
    serial_chunk_reset_locked();
    snprintf(serial_chunk_ctx.transfer_id,
             sizeof(serial_chunk_ctx.transfer_id), "%s", transfer_id);
    serial_chunk_ctx.expected_chunks = chunk_total;
    serial_chunk_ctx.received_chunks = 0;
    serial_chunk_ctx.length = 0;
    serial_chunk_ctx.capacity = 0;
  }

  if (chunk_total != serial_chunk_ctx.expected_chunks ||
      chunk_index != serial_chunk_ctx.received_chunks) {
    ESP_LOGW(TAG,
             "Unexpected chunk sequence: idx=%d expected=%d total=%d",
             chunk_index, serial_chunk_ctx.received_chunks,
             serial_chunk_ctx.expected_chunks);
    serial_chunk_reset_locked();
    xSemaphoreGive(serial_chunk_mutex);
    free(chunk_data);
    return ESP_ERR_INVALID_STATE;
  }

  size_t new_len = serial_chunk_ctx.length + chunk_len;
  if (!serial_chunk_reserve_locked(new_len)) {
    ESP_LOGE(TAG, "Failed to expand chunk buffer to %zu bytes", new_len);
    serial_chunk_reset_locked();
    xSemaphoreGive(serial_chunk_mutex);
    free(chunk_data);
    return ESP_ERR_NO_MEM;
  }

  memcpy(serial_chunk_ctx.buffer + serial_chunk_ctx.length, chunk_data, chunk_len);
  serial_chunk_ctx.length = new_len;
  serial_chunk_ctx.received_chunks++;

  uint8_t *final_buf = NULL;
  size_t final_len = 0;
  bool finished =
      serial_chunk_ctx.received_chunks >= serial_chunk_ctx.expected_chunks;
  if (finished) {
    final_buf = serial_chunk_ctx.buffer;
    final_len = serial_chunk_ctx.length;
    serial_chunk_ctx.buffer = NULL;
    serial_chunk_ctx.length = 0;
    serial_chunk_ctx.capacity = 0;
    serial_chunk_ctx.expected_chunks = 0;
    serial_chunk_ctx.received_chunks = 0;
    serial_chunk_ctx.transfer_id[0] = '\0';
  }
  xSemaphoreGive(serial_chunk_mutex);
  free(chunk_data);

  if (finished && final_buf && final_len > 0) {
    tx_tasks(final_buf, final_len);
    free(final_buf);
  }

  return ESP_OK;
}

static void ws_send_uart_ack(httpd_req_t *req, const char *transfer_id,
                             int chunk_index, int chunk_total, bool success,
                             const char *message) {
  cJSON *ack = cJSON_CreateObject();
  if (!ack) {
    return;
  }

  cJSON_AddStringToObject(ack, "type", "uart_ack");
  cJSON_AddBoolToObject(ack, "success", success);
  cJSON_AddNumberToObject(ack, "chunkIndex", chunk_index);
  cJSON_AddNumberToObject(ack, "chunkTotal", chunk_total);
  if (transfer_id) {
    cJSON_AddStringToObject(ack, "transferId", transfer_id);
  }
  if (message) {
    cJSON_AddStringToObject(ack, "message", message);
  }

  char *json_str = cJSON_PrintUnformatted(ack);
  cJSON_Delete(ack);
  if (!json_str) {
    return;
  }

  httpd_ws_frame_t ws_pkt = {
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = (uint8_t *)json_str,
      .len = strlen(json_str),
  };
  httpd_ws_send_frame(req, &ws_pkt);
  free(json_str);
}

// 安全的JSON响应发送函数（检查内存分配失败）
static esp_err_t safe_json_response(httpd_req_t *req, cJSON *root) {
  char *json_data = cJSON_Print(root);
  cJSON_Delete(root);
  
  if (!json_data) {
    ESP_LOGE(TAG, "Failed to create JSON (out of memory)");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  return ESP_OK;
}

// 系统健康检查函数
static bool system_health_check(void) {
  size_t free_heap = esp_get_free_heap_size();
  size_t min_heap = esp_get_minimum_free_heap_size();
  
  // 检查内存状况
  if (free_heap < 8192) { // 小于8KB认为危险
    ESP_LOGE(TAG, "系统内存不足: free=%d, min_ever=%d", free_heap, min_heap);
    return false;
  }
  
  // 检查任务栈使用情况
  UBaseType_t high_water_mark = uxTaskGetStackHighWaterMark(NULL);
  if (high_water_mark < 256) { // 栈剩余小于256字节
    ESP_LOGW(TAG, "任务栈使用率过高: 剩余=%d bytes", high_water_mark);
  }
  
  return true;
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

extern const char brand_config_js_start[] asm("_binary_brand_config_js_start");
extern const char brand_config_js_end[] asm("_binary_brand_config_js_end");

extern const char i18n_js_start[] asm("_binary_i18n_js_start");
extern const char i18n_js_end[] asm("_binary_i18n_js_end");

extern const char zh_cn_json_start[] asm("_binary_zh_CN_json_start");
extern const char zh_cn_json_end[] asm("_binary_zh_CN_json_end");

extern const char en_us_json_start[] asm("_binary_en_US_json_start");
extern const char en_us_json_end[] asm("_binary_en_US_json_end");

#define ESP_WIFI_SSID DEVICE_MODEL
#define ESP_WIFI_PASS "12345678"
#define MAX_STA_CONN 10

httpd_handle_t http_server = NULL;

// 添加WebSocket相关变量
// static httpd_handle_t websocket_server = NULL;
// static int client_session_id = 0;  // 暂时未使用
// 定义常量和全局变量
#define MAX_WS_CLIENTS 8        // 限制最大WebSocket客户端数量
#define WS_QUEUE_SIZE 16        // WebSocket消息队列大小
#define WS_MAX_MSG_LEN 1024     // 保持较小的消息体以避免任务栈溢出
#define WS_UART_CHUNK_SIZE 128  // 串口数据单帧分片字节数
#define WS_CLIENT_TAG_MAX 32    // WebSocket客户端标记长度上限

// 日志开关控制（默认关闭以节省资源）
static bool log_enabled = false;
static bool ws_queue_warn_logged = false;

typedef struct {
  int client_fd;
  bool in_use;
  char client_tag[WS_CLIENT_TAG_MAX];
} ws_client_t;

static ws_client_t ws_clients[MAX_WS_CLIENTS] = {0};
static QueueHandle_t ws_msg_queue = NULL;
static TaskHandle_t ws_task_handle = NULL;

static void ws_reset_client_slot(int index, const char *reason) {
  if (index < 0 || index >= MAX_WS_CLIENTS) {
    return;
  }
  if (ws_clients[index].in_use) {
    const char *tag =
        ws_clients[index].client_tag[0] ? ws_clients[index].client_tag : "-";
    ESP_LOGI(TAG,
             "WS client slot %d released%s%s (fd=%d, tag=%s)",
             index, reason ? ": " : "",
             reason ? reason : "", ws_clients[index].client_fd, tag);
  }
  ws_clients[index].client_fd = -1;
  ws_clients[index].in_use = false;
  ws_clients[index].client_tag[0] = '\0';
}

static int ws_find_client_by_fd(int client_fd) {
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (ws_clients[i].in_use && ws_clients[i].client_fd == client_fd) {
      return i;
    }
  }
  return -1;
}

static size_t ws_remove_client_by_tag(const char *tag, const char *reason) {
  size_t removed = 0;
  if (!tag || tag[0] == '\0') {
    return removed;
  }
  for (int i = 0; i < MAX_WS_CLIENTS; i++) {
    if (ws_clients[i].in_use &&
        strncmp(ws_clients[i].client_tag, tag, WS_CLIENT_TAG_MAX) == 0) {
      ESP_LOGW(TAG, "WS client tag '%s' closing slot %d (%s)", tag, i,
               reason ? reason : "manual");
      if (http_server) {
        httpd_sess_trigger_close(http_server, ws_clients[i].client_fd);
      }
      ws_reset_client_slot(i, reason ? reason : "manual");
      removed++;
    }
  }
  return removed;
}

static void ws_fill_client_tag(httpd_req_t *req, char *out_tag,
                               size_t out_len) {
  if (!out_tag || out_len == 0) {
    return;
  }
  out_tag[0] = '\0';

  size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len <= 0) {
    return;
  }

  char *query_str = malloc(query_len + 1);
  if (!query_str) {
    return;
  }

  if (httpd_req_get_url_query_str(req, query_str, query_len + 1) == ESP_OK) {
    char value[WS_CLIENT_TAG_MAX];
    if (httpd_query_key_value(query_str, "client_tag", value,
                              sizeof(value)) == ESP_OK) {
      value[WS_CLIENT_TAG_MAX - 1] = '\0';
      strncpy(out_tag, value, out_len - 1);
      out_tag[out_len - 1] = '\0';
    }
  }

  free(query_str);
}

static esp_err_t ws_disconnect_post_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 256) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "invalid payload");
  }

  char *buf = malloc(req->content_len + 1);
  if (!buf) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "no mem");
  }

  int received = httpd_req_recv(req, buf, req->content_len);
  if (received <= 0) {
    free(buf);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "receive error");
  }
  buf[received] = '\0';

  cJSON *root = cJSON_Parse(buf);
  free(buf);
  if (!root) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "invalid json");
  }

  cJSON *tag = cJSON_GetObjectItemCaseSensitive(root, "client_tag");
  if (!cJSON_IsString(tag) || !tag->valuestring || tag->valuestring[0] == '\0') {
    cJSON_Delete(root);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                               "client_tag required");
  }

  size_t removed = ws_remove_client_by_tag(tag->valuestring, "api disconnect");
  cJSON_Delete(root);

  cJSON *resp = cJSON_CreateObject();
  if (!resp) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "no mem");
  }

  if (removed > 0) {
    cJSON_AddNumberToObject(resp, "code", 200);
    cJSON_AddStringToObject(resp, "message", "disconnected");
    cJSON_AddNumberToObject(resp, "count", removed);
  } else {
    cJSON_AddNumberToObject(resp, "code", 404);
    cJSON_AddStringToObject(resp, "message", "tag not found");
    cJSON_AddNumberToObject(resp, "count", 0);
  }

  char *resp_str = cJSON_PrintUnformatted(resp);
  cJSON_Delete(resp);
  if (!resp_str) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "no mem");
  }

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, resp_str, HTTPD_RESP_USE_STRLEN);
  free(resp_str);
  return ESP_OK;
}



// static void disconnect_handler(void *arg, esp_event_base_t event_base,
//                                int32_t event_id, void *event_data);

// 添加工作模式配置结构体
typedef struct {
  char slave_addr[8];
  char function_code[4];
  char register_addr[8];
  char register_num[8];
  char timeout[8];
  char data_format[16];
  char interval_time[8];
  char report_format[8];
  char baud_rate[8];
  char data_bit[8];
  char stop_bit[8];
  char check_bit[8];
} modbus_item_config_t;

typedef struct {
  char work_mode[32];
  char poll_time[16];
  int modbus_items_count;
  modbus_item_config_t modbus_items[MODBUS_ITEM_MAX];
} work_mode_config_t;


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
  esp_read_mac(eth_mac, ESP_MAC_ETH);

  ESP_LOGI(TAG, "ESP_MAC_WIFI_STA MAC: %02x:%02x:%02x:%02x:%02x:%02x",
           sta_mac[0], sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4],
           sta_mac[5]);
  sprintf(device_mac, "%02X%02X%02X%02X%02X%02X", sta_mac[0], sta_mac[1],
          sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(device_mac_end, "%02X%02X", sta_mac[4], sta_mac[5]);

  // esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);

  sprintf(device_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0],
          sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);
  sprintf(device_eth_mac, "%02X:%02X:%02X:%02X:%02X:%02X", eth_mac[0],
          eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);

  char ap_ssid[80] = {0};
  strcpy(ap_ssid, ESP_WIFI_SSID);
  strcat(ap_ssid, "_");
  strcat(ap_ssid, device_mac_end);

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t ap_name, ap_password, ap_wait_time = 0;
  ret = nvs_get_str(nvs_handle, "ap_name", NULL, &ap_name);
  nvs_get_str(nvs_handle, "ap_password", NULL, &ap_password);
  nvs_get_str(nvs_handle, "ap_wait_time", NULL, &ap_wait_time);
  switch (ret) {
  case ESP_OK:
    printf("ap_name set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    printf("no ap_name set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_name", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_password", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_wait_time", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'ap_name': %s\n", esp_err_to_name(ret));
  } else {
    // 分配缓冲区
    char *nvs_ap_name = malloc(ap_name);
    char *nvs_ap_password = malloc(ap_password);
    char *nvs_ap_wait_time = malloc(ap_wait_time);
    if (nvs_ap_name == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "ap_name", nvs_ap_name, &ap_name);
      nvs_get_str(nvs_handle, "ap_password", nvs_ap_password, &ap_password);
      nvs_get_str(nvs_handle, "ap_wait_time", nvs_ap_wait_time, &ap_wait_time);
      if (ret == ESP_OK) {
        printf("Value for 'ap_name' is %s\n", nvs_ap_name);
        printf("Value for 'ap_password' is %s\n", nvs_ap_password);
        printf("Value for 'ap_wait_time' is %s\n", nvs_ap_wait_time);
        if (strlen(nvs_ap_name) == 0 && strlen(nvs_ap_password) == 0) {
          wifi_config_t wifi_config = {
              .ap = {.ssid = ESP_WIFI_SSID,
                     .ssid_len = strlen(ap_ssid),
                     .password = ESP_WIFI_PASS,
                     .max_connection = MAX_STA_CONN,
                     .authmode = WIFI_AUTH_WPA_WPA2_PSK},
          };

          strcpy((char *)wifi_config.ap.ssid, ap_ssid);
          wifi_config.ap.ssid_len = strlen((char *)wifi_config.sta.ssid);

          if (strlen(ESP_WIFI_PASS) == 0) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
          }

          ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
          ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
          ESP_ERROR_CHECK(esp_wifi_start());

          esp_netif_ip_info_t ip_info;
          esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                                &ip_info);

          char ip_addr[16];
          inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
          ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

          ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s' password:'%s'",
                   ESP_WIFI_SSID, ESP_WIFI_PASS);
        } else {
          wifi_config_t wifi_config = {
              .ap = {.ssid = ESP_WIFI_SSID,
                     .ssid_len = strlen(ap_ssid),
                     .password = ESP_WIFI_PASS,
                     .max_connection = MAX_STA_CONN,
                     .authmode = WIFI_AUTH_WPA_WPA2_PSK},
          };

          strcpy((char *)wifi_config.ap.ssid, nvs_ap_name);
          strcpy((char *)wifi_config.ap.password, nvs_ap_password);
          wifi_config.ap.ssid_len = strlen((char *)wifi_config.sta.ssid);
          if (strlen(nvs_ap_password) == 0) {
            wifi_config.ap.authmode = WIFI_AUTH_OPEN;
          }

          ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
          ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &wifi_config));
          ESP_ERROR_CHECK(esp_wifi_start());

          esp_netif_ip_info_t ip_info;
          esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_AP_DEF"),
                                &ip_info);

          char ip_addr[16];
          inet_ntoa_r(ip_info.ip.addr, ip_addr, 16);
          ESP_LOGI(TAG, "Set up softAP with IP: %s", ip_addr);

          ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s' password:'%s'",
                   nvs_ap_name, nvs_ap_password);
        }

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
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
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

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
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "以太网串口服务器"));
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
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", "1"));
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
        cJSON_AddItemToObject(root, "device_eth_mac",
                              cJSON_CreateString(device_eth_mac));

        cJSON_AddItemToObject(root, "netconn", cJSON_CreateString(nvs_netconn));
        // cJSON_AddItemToObject(root, "current_time",
        //                       cJSON_CreateString(strftime_buf));
        // printf("11111当前时间是: %s\n", strftime_buf);
        // printf("nvs_netconn: %s\n", nvs_netconn);
        if (atoi(nvs_netconn) == 2) {
          cJSON_AddItemToObject(root, "eth_ip", cJSON_CreateString("---"));
          cJSON_AddItemToObject(root, "sta_ip",
                                cJSON_CreateString(got_ip_addrs));
        } else {
          cJSON_AddItemToObject(root, "eth_ip",
                                cJSON_CreateString(got_ip_addrs));
          cJSON_AddItemToObject(root, "sta_ip", cJSON_CreateString("---"));
        }
        cJSON_AddItemToObject(root, "is_dhcp", cJSON_CreateString(nvs_is_dhcp));
        cJSON_AddItemToObject(root, "uptime", cJSON_CreateString(strftime_buf));
        // 添加外网连接状态，基于心跳请求的状态
        extern bool is_network_connected;
        extern bool is_intranet_mode;

        // 构建网络状态字符串
        char network_status[64];
        if (is_network_connected) {
            if (is_intranet_mode) {
                snprintf(network_status, sizeof(network_status), "已连接 (内网模式)");
            } else {
                snprintf(network_status, sizeof(network_status), "已连接 (外网模式)");
            }
        } else {
            snprintf(network_status, sizeof(network_status), "未连接");
        }

        cJSON_AddItemToObject(root, "internet_status",
                             cJSON_CreateString(network_status));
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
  size_t free_heap_size_bytes = xPortGetFreeHeapSize();
  size_t min_ever_free_heap_size_bytes = xPortGetMinimumEverFreeHeapSize();
  float free_heap_size_kb = free_heap_size_bytes / 1024.0;
  float min_ever_free_heap_size_kb = min_ever_free_heap_size_bytes / 1024.0;
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

  // 获取NVS使用率
  float nvs_usage = get_nvs_usage_percentage();

  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "chip_info_cpu_core",
                        cJSON_CreateNumber(chip_info.cores));
  cJSON_AddItemToObject(root, "wifi_frq", cJSON_CreateNumber(2.4));
  cJSON_AddItemToObject(root, "free_heap_size",
                        cJSON_CreateNumber(free_heap_size_kb));
  cJSON_AddItemToObject(root, "min_ever_free_heap_size",
                        cJSON_CreateNumber(min_ever_free_heap_size_kb));
  cJSON_AddItemToObject(root, "time_since_boot",
                        cJSON_CreateNumber(time_since_boot));
  cJSON_AddItemToObject(root, "res_reason",
                        cJSON_CreateString(reset_reason_to_string(res_reason)));
  cJSON_AddItemToObject(root, "version", cJSON_CreateString(VERSION));
  cJSON_AddItemToObject(root, "active_sockets",
                        cJSON_CreateNumber(socket_count));
  // 添加NVS使用率
  if (nvs_usage >= 0) {
    cJSON_AddItemToObject(root, "nvs_usage", cJSON_CreateNumber(nvs_usage));
  } else {
    cJSON_AddItemToObject(root, "nvs_usage", cJSON_CreateString("获取失败"));
  }
  char *json_data = cJSON_Print(root);
  cJSON_Delete(root);
  
  if (!json_data) {
    ESP_LOGE(TAG, "Failed to create JSON response (out of memory)");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));
  free(json_data);
  return ESP_OK;
}

static esp_err_t root_get_handler(httpd_req_t *req) {
  const uint32_t root_len = root_end - root_start;
  // ESP_LOGI(TAG, "Serve root");
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, root_start, root_len);
  return ESP_OK;
}

static esp_err_t get_status_get_handler(httpd_req_t *req) {
  const uint32_t web_len = web_end - web_start;
  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, web_start, web_len);
  return ESP_OK;
}

static esp_err_t js_get_handler(httpd_req_t *req) {
  const uint32_t js_len = js_end - js_start;
  ESP_LOGI(TAG, "Serve js");
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, js_start, js_len);
  return ESP_OK;
}
static esp_err_t css_get_handler(httpd_req_t *req) {
  const uint32_t css_len = css_end - css_start;
  httpd_resp_set_type(req, "text/css");
  httpd_resp_send(req, css_start, css_len);
  return ESP_OK;
}

// 品牌配置JS处理器
static esp_err_t brand_config_js_get_handler(httpd_req_t *req) {
  // 动态生成品牌配置JavaScript
  char brand_config[512];
  snprintf(brand_config, sizeof(brand_config),
    "// 品牌配置 - 编译时自动生成\n"
    "window.BRAND_CONFIG = {\n"
    "    brand: \"%s\",\n"
    "    brandNameCN: \"%s\",\n"
    "    brandNameEN: \"%s\",\n"
    "    model: \"%s\"\n"
    "};\n",
    BRAND_TYPE, BRAND_NAME_CN, BRAND_NAME_EN, DEVICE_MODEL);

  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, brand_config, strlen(brand_config));
  return ESP_OK;
}

// i18n.js处理器
static esp_err_t i18n_js_get_handler(httpd_req_t *req) {
  const uint32_t i18n_js_len = i18n_js_end - i18n_js_start;
  httpd_resp_set_type(req, "application/javascript");
  httpd_resp_send(req, i18n_js_start, i18n_js_len);
  return ESP_OK;
}

// 中文翻译文件处理器
static esp_err_t zh_cn_json_get_handler(httpd_req_t *req) {
  const uint32_t zh_cn_len = zh_cn_json_end - zh_cn_json_start;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, zh_cn_json_start, zh_cn_len);
  return ESP_OK;
}

// 英文翻译文件处理器
static esp_err_t en_us_json_get_handler(httpd_req_t *req) {
  const uint32_t en_us_len = en_us_json_end - en_us_json_start;
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, en_us_json_start, en_us_len);
  return ESP_OK;
}

static esp_err_t post_handler(httpd_req_t *req) {
  char buf[512];
  int ret, remaining = req->content_len;
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
  printf("%s\n", buf);
  ESP_ERROR_CHECK(nvs_init());
  save_to_nvs(buf);
  //   save_form_data(buf);
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

static esp_err_t get_mqtt_post_handler(httpd_req_t *req) {
  post_handler(req);
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

static esp_err_t get_tcp_post_handler(httpd_req_t *req) {
  post_handler(req);
  return ESP_OK;
}



static esp_err_t get_http_post_handler(httpd_req_t *req) {
  post_handler(req);
  return ESP_OK;
}

static esp_err_t get_ap_set_post_handler(httpd_req_t *req) {
  post_handler(req);
  return ESP_OK;
}

static esp_err_t get_serial_set_handler(httpd_req_t *req) {

  char buf[512];
  int ret, remaining = req->content_len;
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

  printf("%s\n", buf);
  ESP_ERROR_CHECK(nvs_init());
  save_to_nvs(buf);


  esp_err_t rets = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t baud_rate, data_bit, check_bit, stop_bit, frame_time, frame_len = 0;
  rets = nvs_get_str(nvs_handle, "baud_rate", NULL, &baud_rate);
  nvs_get_str(nvs_handle, "data_bit", NULL, &data_bit);
  nvs_get_str(nvs_handle, "check_bit", NULL, &check_bit);
  nvs_get_str(nvs_handle, "stop_bit", NULL, &stop_bit);
  nvs_get_str(nvs_handle, "frame_time", NULL, &frame_time);
  nvs_get_str(nvs_handle, "frame_len", NULL, &frame_len);
  // nvs_get_str(nvs_handle, "flow_ctl", NULL, &flow_ctl);
  switch (rets) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "frame_len", "512"));
    // ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "flow_ctl", "0"));
    break;
  default:
    break;
  }
  if (rets != ESP_OK) {
    printf("Error getting size of 'SERIAL': %s\n", esp_err_to_name(rets));
  } else {
    char *nvs_baud_rate = malloc(baud_rate);
    char *nvs_data_bit = malloc(data_bit);
    char *nvs_check_bit = malloc(check_bit);
    char *nvs_stop_bit = malloc(stop_bit);
    char *nvs_frame_time = malloc(frame_time);
    char *nvs_frame_len = malloc(frame_len);
    // char *nvs_flow_ctl = malloc(flow_ctl);

    if (nvs_baud_rate == NULL) {
      printf("Memory allocation failed\n");
    } else {
      rets = nvs_get_str(nvs_handle, "baud_rate", nvs_baud_rate, &baud_rate);
      nvs_get_str(nvs_handle, "data_bit", nvs_data_bit, &data_bit);
      nvs_get_str(nvs_handle, "check_bit", nvs_check_bit, &check_bit);
      nvs_get_str(nvs_handle, "stop_bit", nvs_stop_bit, &stop_bit);
      nvs_get_str(nvs_handle, "frame_time", nvs_frame_time, &frame_time);
      nvs_get_str(nvs_handle, "frame_len", nvs_frame_len, &frame_len);
      // nvs_get_str(nvs_handle, "flow_ctl", nvs_flow_ctl, &flow_ctl);
      if (rets == ESP_OK) {
        printf("Value for 'baud_rate' is %s\n", nvs_baud_rate);
        printf("Value for 'data_bit' is %s\n", nvs_data_bit);
        printf("Value for 'check_bit' is %s\n", nvs_check_bit);
        printf("Value for 'stop_bit' is %s\n", nvs_stop_bit);
        printf("Value for 'frame_time' is %s\n", nvs_frame_time);
        printf("Value for 'frame_len' is %s\n", nvs_frame_len);
        uart_parity_t parity = 0;
        if (nvs_check_bit != NULL) {
          if (strcmp(nvs_check_bit, "None") == 0) {
            parity = UART_PARITY_DISABLE;
          } else if (strcmp(nvs_check_bit, "Odd") == 0) {
            parity = UART_PARITY_ODD;
          } else if (strcmp(nvs_check_bit, "Even") == 0) {
            parity = UART_PARITY_EVEN;
          }
        }
        uart_reinit(atoi(nvs_baud_rate), atoi(nvs_data_bit), parity,
                    atof(nvs_stop_bit), atoi(nvs_frame_time),
                    atoi(nvs_frame_len), false);

      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(rets));
      }
      free(nvs_baud_rate);
      free(nvs_data_bit);
      free(nvs_check_bit);
      free(nvs_stop_bit);
      free(nvs_frame_time);
      free(nvs_frame_len);
      // free(nvs_flow_ctl);
    }
  }
  nvs_close(nvs_handle);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "msg", cJSON_CreateString("success"));
  cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(200));
  return safe_json_response(req, root);

  // cJSON_Delete(json);
  // post_handler(req);
  return ESP_OK;
}

static esp_err_t get_serial_ctl_handler(httpd_req_t *req) {
  size_t total_len = req->content_len;
  if (total_len == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    return ESP_FAIL;
  }

  // 限制单次请求体，防止异常占用内存（默认 32KB，可按需调整）
  const size_t max_body_len = 32 * 1024;
  if (total_len > max_body_len) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body too large");
    return ESP_FAIL;
  }

  char *body_buf = malloc(total_len + 1);
  if (!body_buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no memory for body");
    return ESP_FAIL;
  }

  size_t received = 0;
  while (received < total_len) {
    size_t to_read = total_len - received;
    if (to_read > 1024) {
      to_read = 1024;
    }
    int ret = httpd_req_recv(req, body_buf + received, to_read);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      free(body_buf);
      return ESP_FAIL;
    }
    received += ret;
  }
  body_buf[received] = '\0';

  cJSON *json = cJSON_Parse(body_buf);
  if (json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      fprintf(stderr, "Error before: %s\n", error_ptr);
    }
    free(body_buf);
    return ESP_FAIL;
  }

  cJSON *instruction = cJSON_GetObjectItemCaseSensitive(json, "instruction");
  cJSON *sendType = cJSON_GetObjectItemCaseSensitive(json, "sendType");
  cJSON *chunkIndex = cJSON_GetObjectItemCaseSensitive(json, "chunkIndex");
  cJSON *chunkTotal = cJSON_GetObjectItemCaseSensitive(json, "chunkTotal");
  cJSON *transferId = cJSON_GetObjectItemCaseSensitive(json, "transferId");

  int chunk_index_val = -1;
  int chunk_total_val = -1;
  if (cJSON_IsNumber(chunkIndex)) {
    chunk_index_val = chunkIndex->valueint;
  }
  if (cJSON_IsNumber(chunkTotal)) {
    chunk_total_val = chunkTotal->valueint;
  }
  const char *transfer_id_str =
      (cJSON_IsString(transferId) && transferId->valuestring &&
       transferId->valuestring[0] != '\0')
          ? transferId->valuestring
          : NULL;

  bool is_hex_mode =
      (cJSON_IsString(sendType) && sendType->valuestring &&
       strcmp(sendType->valuestring, "hex") == 0);

  uint8_t *chunk_data = NULL;
  size_t chunk_len = 0;

  if (!cJSON_IsString(instruction) || instruction->valuestring == NULL) {
    cJSON_Delete(json);
    free(body_buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "instruction missing");
    return ESP_FAIL;
  }

  if (chunk_total_val > 0) {
    ESP_LOGI(TAG, "UART chunk %d/%d, payload len=%zu", chunk_index_val + 1,
             chunk_total_val, strlen(instruction->valuestring));
  }

  esp_err_t decode_ret = decode_instruction_payload(
      instruction->valuestring, is_hex_mode, &chunk_data, &chunk_len);
  if (decode_ret == ESP_ERR_NO_MEM) {
    cJSON_Delete(json);
    free(body_buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "no memory for instruction");
    return ESP_FAIL;
  } else if (decode_ret != ESP_OK) {
    cJSON_Delete(json);
    free(body_buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid instruction");
    return ESP_FAIL;
  }

  if (chunk_total_val > 1 && transfer_id_str == NULL) {
    free(chunk_data);
    cJSON_Delete(json);
    free(body_buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing transferId");
    return ESP_FAIL;
  }

  esp_err_t chunk_ret = process_uart_chunk(chunk_data, chunk_len,
                                           chunk_index_val, chunk_total_val,
                                           transfer_id_str);
  if (chunk_ret == ESP_ERR_NO_MEM) {
    cJSON_Delete(json);
    free(body_buf);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "chunk buffer no memory");
    return ESP_FAIL;
  } else if (chunk_ret != ESP_OK) {
    cJSON_Delete(json);
    free(body_buf);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "chunk process failed");
    return ESP_FAIL;
  }

  // 返回响应
  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(200));
  char *json_data = cJSON_Print(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));

  free(json_data);
  cJSON_Delete(root);
  cJSON_Delete(json);
  free(body_buf);

  return ESP_OK;
}

static esp_err_t get_uart_response_handler(httpd_req_t *req) {
    // 现在数据通过WebSocket实时发送，这个接口只返回空数据以避免重复
    // 设置响应类型
    httpd_resp_set_type(req, "application/json");
    
    // 返回空的JSON对象
    httpd_resp_sendstr(req, "{}");
    
    return ESP_OK;
}


// 添加全局变量来存储OTA进度
static int ota_progress = 0;
static char ota_status[64] = "准备中...";

static void ota_update_status(bool is_error, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(ota_status, sizeof(ota_status), fmt, args);
  va_end(args);

  if (is_error) {
    ESP_LOGE(TAG, "OTA status: %s", ota_status);
  } else {
    ESP_LOGI(TAG, "OTA status: %s", ota_status);
  }
}

// 添加新的处理函数来获取OTA进度
static esp_err_t get_ota_progress_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "progress", ota_progress);
  cJSON_AddStringToObject(root, "status", ota_status);

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

static esp_err_t perform_https_ota(const char *url) {
  ESP_LOGI(TAG, "Starting HTTPS OTA from %s", url);
  ota_update_status(false, "连接服务器...");

  esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 5000,
      .keep_alive_enable = true,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .skip_cert_common_name_check = false,
  };

  esp_https_ota_config_t ota_config = {.http_config = &config};
  esp_https_ota_handle_t https_ota_handle = NULL;

  ESP_LOGI(TAG, "HTTPS OTA: initializing connection and TLS handshake");
  esp_err_t err = esp_https_ota_begin(&ota_config, &https_ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTPS OTA begin failed: %s", esp_err_to_name(err));
    ota_update_status(true, "OTA开始失败: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "HTTPS OTA: connection established, start transferring image");

  int last_logged = -5;
  while (1) {
    err = esp_https_ota_perform(https_ota_handle);
    if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
      break;
    }

    int image_size = esp_https_ota_get_image_size(https_ota_handle);
    int image_done = esp_https_ota_get_image_len_read(https_ota_handle);
    if (image_size > 0 && image_done >= 0) {
      ota_progress = (image_done * 100) / image_size;
      if (ota_progress - last_logged >= 5) {
        ESP_LOGI(TAG, "HTTPS OTA progress: %d%% (%d/%d bytes)", ota_progress,
                 image_done, image_size);
        last_logged = ota_progress;
      }
      ota_update_status(false, "升级中 %d%%", ota_progress);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTPS OTA perform failed: %s", esp_err_to_name(err));
    ota_update_status(true, "升级失败: %s", esp_err_to_name(err));
    if (https_ota_handle) {
      esp_https_ota_abort(https_ota_handle);
    }
    return err;
  }

  ESP_LOGI(TAG, "HTTPS OTA: download finished with err=%s, verifying image",
           esp_err_to_name(err));
  err = esp_https_ota_finish(https_ota_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTPS OTA finish failed: %s", esp_err_to_name(err));
    ota_update_status(true, "固件验证失败: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "HTTPS OTA image verified successfully");
    ota_update_status(false, "HTTPS固件验证通过");
  }
  return err;
}

static esp_err_t perform_http_ota(const char *url) {
  ESP_LOGI(TAG, "Starting HTTP OTA from %s", url);
  ota_update_status(false, "连接服务器...");

  esp_http_client_config_t config = {
      .url = url,
      .timeout_ms = 5000,
      .keep_alive_enable = true,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to create HTTP client");
    ota_update_status(true, "HTTP客户端创建失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "HTTP OTA: opening connection...");
  esp_err_t err = esp_http_client_open(client, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "HTTP connection failed: %s", esp_err_to_name(err));
    ota_update_status(true, "HTTP连接失败: %s", esp_err_to_name(err));
    esp_http_client_cleanup(client);
    return err;
  }
  ESP_LOGI(TAG, "HTTP OTA: connection established, requesting headers");

  int64_t content_length = esp_http_client_fetch_headers(client);
  int status_code = esp_http_client_get_status_code(client);
  ESP_LOGI(TAG, "HTTP OTA status=%d, content_length=%lld", status_code,
           (long long)content_length);
  if (status_code > 0 && (status_code < 200 || status_code >= 300)) {
    ota_update_status(true, "HTTP响应异常: %d", status_code);
    ESP_LOGE(TAG, "Unexpected HTTP status code %d", status_code);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  int total_size = esp_http_client_get_content_length(client);
  if (total_size <= 0) {
    ESP_LOGW(TAG, "HTTP OTA server did not return content length (chunked?)");
    ota_update_status(false, "服务器未提供长度，继续下载...");
  }

  const esp_partition_t *update_partition =
      esp_ota_get_next_update_partition(NULL);
  if (update_partition == NULL) {
    ESP_LOGE(TAG, "No OTA partition available");
    ota_update_status(true, "未找到可用分区");
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  ESP_LOGI(TAG,
           "HTTP OTA: selected partition type=%d subtype=%d offset=0x%08x size=%d",
           update_partition->type, update_partition->subtype,
           update_partition->address, update_partition->size);

  esp_ota_handle_t update_handle = 0;
  err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    ota_update_status(true, "OTA初始化失败: %s", esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
  }
  ESP_LOGI(TAG, "HTTP OTA: OTA write session started successfully");

  uint8_t ota_buf[OTA_HTTP_BUFFER_SIZE];
  int data_read = 0;
  int image_done = 0;
  int last_logged = -5;
  size_t last_logged_bytes = 0;
  while ((data_read = esp_http_client_read(client, (char *)ota_buf,
                                           sizeof(ota_buf))) > 0) {
    err = esp_ota_write(update_handle, (const void *)ota_buf, data_read);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
      ota_update_status(true, "OTA写入失败: %s", esp_err_to_name(err));
      esp_ota_end(update_handle);
      esp_http_client_close(client);
      esp_http_client_cleanup(client);
      return err;
    }

    image_done += data_read;
    if (total_size > 0) {
      ota_progress = (image_done * 100) / total_size;
      if (ota_progress - last_logged >= 5) {
        ESP_LOGI(TAG, "HTTP OTA progress: %d%% (%d/%d bytes)", ota_progress,
                 image_done, total_size);
        last_logged = ota_progress;
      }
      ota_update_status(false, "升级中 %d%%", ota_progress);
    } else if ((image_done - last_logged_bytes) >= (32 * 1024)) {
      last_logged_bytes = image_done;
      ESP_LOGI(TAG, "HTTP OTA downloaded %dKB (chunked transfer)",
               image_done / 1024);
      ota_update_status(false, "已下载 %dKB", image_done / 1024);
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }

  if (data_read < 0) {
    ESP_LOGE(TAG, "HTTP OTA read failed");
    ota_update_status(true, "OTA读取失败");
    esp_ota_end(update_handle);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ESP_FAIL;
  }

  err = esp_ota_end(update_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
    ota_update_status(true, "OTA结束失败: %s", esp_err_to_name(err));
  } else {
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
               esp_err_to_name(err));
      ota_update_status(true, "设置启动分区失败: %s", esp_err_to_name(err));
    } else {
      ota_progress = 100;
      ota_update_status(false, "升级包校验完成");
      ESP_LOGI(TAG, "HTTP OTA image written to partition at 0x%08x",
               update_partition->address);
    }
  }

  ESP_LOGI(TAG, "HTTP OTA: closing connection");
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return err;
}

// 添加OTA任务函数
static void ota_task(void *pvParameter) {
  char *url = (char *)pvParameter;
  esp_err_t err = ESP_ERR_INVALID_ARG;

  if (url == NULL) {
    ESP_LOGE(TAG, "OTA task received NULL URL");
    ota_update_status(true, "无效的URL");
  } else if (strncasecmp(url, "https://", 8) == 0) {
    err = perform_https_ota(url);
  } else if (strncasecmp(url, "http://", 7) == 0) {
    err = perform_http_ota(url);
  } else {
    ESP_LOGE(TAG, "Unsupported OTA scheme in URL: %s", url);
    ota_update_status(true, "无效的URL协议");
  }

  if (url) {
    free(url);
    url = NULL;
  }

  if (err == ESP_OK) {
    ota_progress = 100;
    ota_update_status(false, "升级成功，准备重启...");
    ESP_LOGI(TAG, "OTA completed successfully, rebooting...");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
  } else {
    ESP_LOGW(TAG, "OTA task ended with error: %s", esp_err_to_name(err));
  }

  vTaskDelete(NULL);
}

static esp_err_t get_ota_post_handler(httpd_req_t *req) {
  char buf[1024 + 1];
  int ret, remaining = req->content_len;

  ESP_LOGI(TAG, "OTA POST handler invoked, content length=%d",
           req->content_len);

  // 读取POST请求数据
  while (remaining > 0) {
    if ((ret = httpd_req_recv(req, buf,
                              MIN(remaining, sizeof(buf) - 1))) <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      return ESP_FAIL;
    }
    remaining -= ret;
    buf[ret] = '\0';
    ESP_LOGI(TAG, "%.*s", ret, buf);
  }

  printf("ota post :%s\n", buf);

  // 解析JSON数据
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL) {
      fprintf(stderr, "Error before: %s\n", error_ptr);
      ota_update_status(true, "JSON解析失败");
    }
    return ESP_FAIL;
  }

  // 获取OTA URL
  cJSON *ota_url = cJSON_GetObjectItemCaseSensitive(json, "ota_url");
  if (!cJSON_IsString(ota_url) || (ota_url->valuestring == NULL)) {
    ota_update_status(true, "无效的URL");
    cJSON_Delete(json);
    return ESP_FAIL;
  }

  printf("ota_url: %s\n", ota_url->valuestring);
  ESP_LOGI(TAG, "OTA request URL parsed: %s", ota_url->valuestring);

  // 重置OTA状态
  ota_progress = 0;
  ota_update_status(false, "准备中...");

  // 发送初始响应
  cJSON *root = cJSON_CreateObject();
  cJSON_AddItemToObject(root, "msg", cJSON_CreateString("success"));
  cJSON_AddItemToObject(root, "code", cJSON_CreateNumber(200));
  esp_err_t resp_status = safe_json_response(req, root);
  ESP_LOGI(TAG, "OTA POST response sent, status=%s",
           resp_status == ESP_OK ? "OK" : "ERR");

  // 在新任务中执行OTA更新
  char *url_copy = strdup(ota_url->valuestring); // 复制URL字符串
  if (url_copy != NULL) {
    TaskHandle_t task_handle;
    if (xTaskCreate(ota_task, "ota_task", 8192, (void *)url_copy, 5,
                    &task_handle) != pdPASS) {
      free(url_copy);
      ota_update_status(true, "创建OTA任务失败");
      ESP_LOGE(TAG, "Failed to create ota_task");
    } else {
      ESP_LOGI(TAG, "ota_task created successfully");
    }
  } else {
    ota_update_status(true, "内存分配失败");
    ESP_LOGE(TAG, "Failed to allocate memory for OTA URL copy");
  }

  cJSON_Delete(json);
  return resp_status;
}

static esp_err_t get_operate_get_handler(httpd_req_t *req) {
  // 在重启前确保工作模式已设置，但不强制设为默认模式
  nvs_handle_t storage_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &storage_handle);
  if (err == ESP_OK) {
    // 检查是否已设置工作模式
    size_t required_size;
    err = nvs_get_str(storage_handle, "w_mode", NULL, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      // 如果没有设置，才设置默认值为mqtt_tcp
      nvs_set_str(storage_handle, "w_mode", "mqtt_tcp");
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
  httpd_resp_send(req, "{\"restore\":\"success\"}", HTTPD_RESP_USE_STRLEN);
  vTaskDelay(pdMS_TO_TICKS(100));
  perform_factory_reset(true);
  return ESP_OK;
}

esp_err_t http_404_error_handler(httpd_req_t *req, httpd_err_code_t err) {
  httpd_resp_set_status(req, "302 Temporary Redirect");
  httpd_resp_set_hdr(req, "Location", "/");
  httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);
  // ESP_LOGI(TAG, "Redirecting to root");
  return ESP_OK;
}

// SERIAL GET Handler
static esp_err_t get_serial_set_info_get_handler(httpd_req_t *req) {
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t baud_rate, data_bit, check_bit, stop_bit, frame_time, frame_len = 0;
  ret = nvs_get_str(nvs_handle, "baud_rate", NULL, &baud_rate);
  nvs_get_str(nvs_handle, "data_bit", NULL, &data_bit);
  nvs_get_str(nvs_handle, "check_bit", NULL, &check_bit);
  nvs_get_str(nvs_handle, "stop_bit", NULL, &stop_bit);
  nvs_get_str(nvs_handle, "frame_time", NULL, &frame_time);
  nvs_get_str(nvs_handle, "frame_len", NULL, &frame_len);

  // nvs_get_str(nvs_handle, "flow_ctl", NULL, &flow_ctl);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "frame_len", "512"));
    // ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "flow_ctl", "0"));
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
    // char *nvs_flow_ctl = malloc(flow_ctl);

    if (nvs_baud_rate == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "baud_rate", nvs_baud_rate, &baud_rate);
      nvs_get_str(nvs_handle, "data_bit", nvs_data_bit, &data_bit);
      nvs_get_str(nvs_handle, "check_bit", nvs_check_bit, &check_bit);
      nvs_get_str(nvs_handle, "stop_bit", nvs_stop_bit, &stop_bit);
      nvs_get_str(nvs_handle, "frame_time", nvs_frame_time, &frame_time);
      nvs_get_str(nvs_handle, "frame_len", nvs_frame_len, &frame_len);
      // nvs_get_str(nvs_handle, "flow_ctl", nvs_flow_ctl, &flow_ctl);
      if (ret == ESP_OK) {
        printf("Value for 'baud_rate' is %s\n", nvs_baud_rate);
        printf("Value for 'data_bit' is %s\n", nvs_data_bit);
        printf("Value for 'check_bit' is %s\n", nvs_check_bit);
        printf("Value for 'stop_bit' is %s\n", nvs_stop_bit);
        printf("Value for 'frame_time' is %s\n", nvs_frame_time);
        printf("Value for 'frame_len' is %s\n", nvs_frame_len);
        // printf("Value for 'flow_ctl' is %s\n", nvs_flow_ctl);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "baud_rate",
                              cJSON_CreateString(nvs_baud_rate));
        cJSON_AddItemToObject(root, "data_bit",
                              cJSON_CreateString(nvs_data_bit));
        cJSON_AddItemToObject(root, "check_bit",
                              cJSON_CreateString(nvs_check_bit));
        cJSON_AddItemToObject(root, "stop_bit",
                              cJSON_CreateString(nvs_stop_bit));
        cJSON_AddItemToObject(root, "frame_time",
                              cJSON_CreateString(nvs_frame_time));
        cJSON_AddItemToObject(root, "frame_len",
                              cJSON_CreateString(nvs_frame_len));
        // cJSON_AddItemToObject(root, "flow_ctl",
        //                       cJSON_CreateString(nvs_flow_ctl));
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));
        free(json_data);
        cJSON_Delete(root);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_baud_rate);
      free(nvs_data_bit);
      free(nvs_check_bit);
      free(nvs_stop_bit);
      free(nvs_frame_time);
      free(nvs_frame_len);
      // free(nvs_flow_ctl);
    }
  }
  nvs_close(nvs_handle);
  return ESP_OK;
}

// 协议配置检查函数
static bool check_protocol_enabled(const char *protocol) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS for protocol check: %s", esp_err_to_name(err));
    return false;
  }
  
  char key[32];
  snprintf(key, sizeof(key), "use_%s", protocol);
  
  size_t required_size = 0;
  err = nvs_get_str(nvs_handle, key, NULL, &required_size);
  if (err != ESP_OK || required_size == 0) {
    nvs_close(nvs_handle);
    return false;
  }
  
  char *value = malloc(required_size);
  if (!value) {
    nvs_close(nvs_handle);
    return false;
  }
  
  err = nvs_get_str(nvs_handle, key, value, &required_size);
  nvs_close(nvs_handle);
  
  bool enabled = false;
  if (err == ESP_OK && value && strlen(value) > 0) {
    // 只有明确设置为"1"时才认为协议已启用，空字符串表示未配置
    if (strcmp(value, "1") == 0) {
      enabled = true;
    }
  }
  
  free(value);
  return enabled;
}

// 添加工作模式设置处理函数
static esp_err_t get_work_mode_set_handler(httpd_req_t *req) {
  ESP_LOGI(TAG, "Received work mode set request");
  
  // 系统健康检查
  if (!system_health_check()) {
    ESP_LOGE(TAG, "系统健康检查失败，拒绝配置修改请求");
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, 
                        "系统资源不足，请稍后重试");
    return ESP_FAIL;
  }

  // 读取POST数据
  if (req->content_len <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "empty body");
    return ESP_FAIL;
  }

  if (req->content_len > WORK_MODE_BODY_MAX) {
    ESP_LOGW(TAG, "work_mode_set body too large: %d", req->content_len);
    httpd_resp_send_err(req, HTTPD_413_CONTENT_TOO_LARGE, "body too large");
    return ESP_FAIL;
  }

  char *buf =
      heap_caps_malloc(req->content_len + 1,
                       MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
  if (!buf) {
    buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_8BIT);
  }
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "内存分配失败");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "work_mode_set body length: %d, free heap: %u",
           req->content_len, (unsigned)esp_get_free_heap_size());

  int remaining = req->content_len;
  int received = 0;
  while (remaining > 0) {
    int ret = httpd_req_recv(req, buf + received, remaining);
    if (ret <= 0) {
      if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
        continue;
      }
      free(buf);
      return ESP_FAIL;
    }
    received += ret;
    remaining -= ret;
  }
  buf[req->content_len] = '\0';

  ESP_LOGI(TAG, "Received data len=%d", req->content_len);
  if (req->content_len > 0) {
    int preview_len = req->content_len > 160 ? 160 : req->content_len;
    ESP_LOGD(TAG, "Body preview: %.*s", preview_len, buf);
  }

  // 解析JSON数据
  cJSON *root = cJSON_Parse(buf);
  free(buf);

  if (!root) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON解析失败");
    return ESP_FAIL;
  }

  // 获取NVS互斥锁保护
  if (nvs_mutex == NULL || xSemaphoreTake(nvs_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGE(TAG, "Failed to acquire NVS mutex");
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "系统忙，请稍后重试");
    return ESP_FAIL;
  }

  // 打开NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    xSemaphoreGive(nvs_mutex);
    cJSON_Delete(root);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS打开失败");
    return ESP_FAIL;
  }

  // 不再清除旧的配置，只更新需要的字段
  // nvs_erase_all(nvs_handle);

  // 获取当前工作模式，在切换前先保存现有配置
  char current_work_mode[32] = {0};
  size_t current_mode_len = sizeof(current_work_mode);
  esp_err_t current_mode_err = nvs_get_str(nvs_handle, "w_mode", current_work_mode, &current_mode_len);
  
  // 保存工作模式
  cJSON *work_mode = cJSON_GetObjectItem(root, "work_mode");
  if (work_mode && work_mode->valuestring) {
    // 如果当前是modbus_rtu模式，且要切换到其他模式，先保存当前modbus配置
    if (current_mode_err == ESP_OK && strcmp(current_work_mode, "modbus_rtu") == 0 && 
        strcmp(work_mode->valuestring, "modbus_rtu") != 0) {
      ESP_LOGI(TAG, "从Modbus-RTU模式切换，保持现有配置");
      // 不清除modbus配置，只更新工作模式
    }
    nvs_set_str(nvs_handle, "w_mode", work_mode->valuestring);
  }

  // 处理轮询间隔时间，包含验证逻辑
  cJSON *poll_time = cJSON_GetObjectItem(root, "poll_time");
  if (poll_time && poll_time->valuestring) {
    int poll_time_ms = atoi(poll_time->valuestring);

    // 验证轮询间隔时间的合理性
    if (poll_time_ms < 1000) {
      ESP_LOGW(TAG, "轮询间隔时间 %d ms 太小，调整为最小值 1000 ms (1秒)", poll_time_ms);
      poll_time_ms = 1000;
    } else if (poll_time_ms > 3600000) {
      ESP_LOGW(TAG, "轮询间隔时间 %d ms 太大，调整为最大值 3600000 ms (1小时)", poll_time_ms);
      poll_time_ms = 3600000;
    }

    // 将调整后的值转换为字符串并保存
    char adjusted_poll_time[16];
    snprintf(adjusted_poll_time, sizeof(adjusted_poll_time), "%d", poll_time_ms);
    nvs_set_str(nvs_handle, "poll_time", adjusted_poll_time);

    ESP_LOGI(TAG, "轮询间隔时间设置为: %d ms (%.1f 秒)", poll_time_ms, poll_time_ms / 1000.0);
  }

  // 立即提交工作模式和轮询时间的更改
  nvs_commit(nvs_handle);
  ESP_LOGI(TAG, "工作模式和轮询时间已保存并提交");

  int modbus_items_offset = 0;
  int modbus_items_total = -1;
  bool modbus_items_chunked = false;
  cJSON *modbus_items = cJSON_GetObjectItem(root, "modbus_items");
  cJSON *items_offset_json = cJSON_GetObjectItem(root, "modbus_items_offset");
  cJSON *items_total_json = cJSON_GetObjectItem(root, "modbus_items_total");
  if (cJSON_IsNumber(items_offset_json) && cJSON_IsNumber(items_total_json)) {
    modbus_items_chunked = true;
    modbus_items_offset = items_offset_json->valueint;
    modbus_items_total = items_total_json->valueint;
    if (modbus_items_offset < 0) {
      modbus_items_offset = 0;
    }
    if (modbus_items_total < 0) {
      modbus_items_total = 0;
    }
    if (modbus_items_total > MODBUS_ITEM_MAX) {
      ESP_LOGW(TAG, "modbus_items_total超过%d，按上限处理", MODBUS_ITEM_MAX);
      modbus_items_total = MODBUS_ITEM_MAX;
    }
  }

  // 处理modbus配置项 - 无论什么工作模式都要保存配置，确保切换时不丢失
  if (modbus_items && cJSON_IsArray(modbus_items)) {
    int items_count = cJSON_GetArraySize(modbus_items);
    int write_offset = modbus_items_chunked ? modbus_items_offset : 0;
    int total_items = modbus_items_chunked ? modbus_items_total : items_count;

    if (!modbus_items_chunked) {
      if (items_count > MODBUS_ITEM_MAX) {
        ESP_LOGW(TAG, "modbus_items超过%d，已截断", MODBUS_ITEM_MAX);
        items_count = MODBUS_ITEM_MAX;
      }
      total_items = items_count;
    } else {
      if (total_items < 0) {
        total_items = items_count;
      }
      if (write_offset >= total_items) {
        ESP_LOGW(TAG, "modbus_items_offset超出范围: %d/%d", write_offset,
                 total_items);
        items_count = 0;
      } else if (write_offset + items_count > total_items) {
        items_count = total_items - write_offset;
      }
    }

    if (total_items > MODBUS_ITEM_MAX) {
      total_items = MODBUS_ITEM_MAX;
    }

    nvs_set_i32(nvs_handle, "m_count", total_items);
    ESP_LOGI(TAG, "保存Modbus配置项 %d 个 (offset=%d, total=%d)", items_count,
             write_offset, total_items);

    // 保存每个配置项到NVS
    for (int i = 0; i < items_count; i++) {
      int idx = write_offset + i;
      if (idx >= MODBUS_ITEM_MAX) {
        break;
      }
      cJSON *item = cJSON_GetArrayItem(modbus_items, i);
      if (!item) continue;

      // 获取并保存启用状态
      cJSON *enabled = cJSON_GetObjectItem(item, "enabled");
      bool is_enabled = (enabled && cJSON_IsTrue(enabled));

      char enabled_key[16];
      snprintf(enabled_key, sizeof(enabled_key), "m%d_en", idx);
      nvs_set_u8(nvs_handle, enabled_key, is_enabled ? 1 : 0);

      // 保存配置到NVS
      const struct {
        const char *json_key;
        const char *nvs_key;
      } fields[] = {
          {"slave_addr", "s_addr"},
          {"function_code", "f_code"},
          {"register_addr", "r_addr"},
          {"register_num", "r_num"},
          {"timeout", "timeout"},
          {"data_format", "d_fmt"},
          {"interval_time", "i_time"},
          {"report_format", "r_fmt"},
          {"baud_rate", "baud_rate"},
          {"data_bit", "data_bit"},
          {"stop_bit", "stop_bit"},
          {"check_bit", "check_bit"},
      };

      for (int j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
        cJSON *field = cJSON_GetObjectItem(item, fields[j].json_key);
        if (field && field->valuestring) {
          char nvs_key[16];
          snprintf(nvs_key, sizeof(nvs_key), "m%d%s", idx, fields[j].nvs_key);
          nvs_set_str(nvs_handle, nvs_key, field->valuestring);
        }
      }
    }
  }
  
  bool start_modbus_after_save = false;
  bool modbus_items_last_chunk = false;

  bool target_modbus_rtu = false;
  if (work_mode && work_mode->valuestring) {
    target_modbus_rtu = (strcmp(work_mode->valuestring, "modbus_rtu") == 0);
  } else if (current_mode_err == ESP_OK) {
    target_modbus_rtu = (strcmp(current_work_mode, "modbus_rtu") == 0);
  }

  // 处理modbus任务管理
  if (target_modbus_rtu) {
      // 当前切换到modbus_rtu模式，启动任务
    // 检查Modbus-RTU网关模式下的协议配置
    if (modbus_items && cJSON_IsArray(modbus_items)) {
      int items_count = cJSON_GetArraySize(modbus_items);
      if (items_count > MODBUS_ITEM_MAX) {
        ESP_LOGW(TAG, "modbus_items超过%d，按上限处理", MODBUS_ITEM_MAX);
        items_count = MODBUS_ITEM_MAX;
      }
      if (modbus_items_chunked) {
        int total_items = (modbus_items_total > 0) ? modbus_items_total : items_count;
        if (total_items > MODBUS_ITEM_MAX) {
          total_items = MODBUS_ITEM_MAX;
        }
        modbus_items_last_chunk =
            (modbus_items_offset + items_count >= total_items);
      } else {
        modbus_items_last_chunk = true;
      }
      
      // 收集所有使用的上报方式
      bool has_mqtt = false, has_tcp = false, has_http = false;
      bool mqtt_enabled = false, tcp_enabled = false, http_enabled = false;
      
      for (int i = 0; i < items_count; i++) {
        cJSON *item = cJSON_GetArrayItem(modbus_items, i);
        if (!item) continue;
        
        cJSON *report_format = cJSON_GetObjectItem(item, "report_format");
        if (report_format && report_format->valuestring) {
          if (strcmp(report_format->valuestring, "MQTT") == 0) {
            has_mqtt = true;
          } else if (strcmp(report_format->valuestring, "TCP") == 0) {
            has_tcp = true;
          } else if (strcmp(report_format->valuestring, "HTTP") == 0) {
            has_http = true;
          }
        }
      }
      
      // 检查协议是否启用
      if (has_mqtt) mqtt_enabled = check_protocol_enabled("mqtt");
      if (has_tcp) tcp_enabled = check_protocol_enabled("tcp");
      if (has_http) http_enabled = check_protocol_enabled("http");
      
      // 如果有使用的协议未启用，返回错误
      if ((has_mqtt && !mqtt_enabled) || (has_tcp && !tcp_enabled) || (has_http && !http_enabled)) {
        nvs_close(nvs_handle);
        xSemaphoreGive(nvs_mutex);  // 释放互斥锁
        cJSON_Delete(root);
        
        // 构建错误响应
        cJSON *error_response = cJSON_CreateObject();
        cJSON_AddNumberToObject(error_response, "code", 400);
        
        char error_msg[256] = "请先在协议管理中启用并配置以下协议: ";
        bool first = true;
        if (has_mqtt && !mqtt_enabled) {
          if (!first) strcat(error_msg, ", ");
          strcat(error_msg, "MQTT");
          first = false;
        }
        if (has_tcp && !tcp_enabled) {
          if (!first) strcat(error_msg, ", ");
          strcat(error_msg, "TCP");
          first = false;
        }
        if (has_http && !http_enabled) {
          if (!first) strcat(error_msg, ", ");
          strcat(error_msg, "HTTP");
          first = false;
        }
        
        cJSON_AddStringToObject(error_response, "msg", error_msg);
        
        char *json_error = cJSON_Print(error_response);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json_error);
        
        free(json_error);
        cJSON_Delete(error_response);
        return ESP_OK; // 正常返回，但带有错误信息
      }
    }
    
    stop_modbus_tasks(); // 停止现有任务
    start_modbus_after_save = modbus_items_last_chunk;
  } else if (work_mode) {
    // 切换到非modbus_rtu模式，停止modbus任务
    ESP_LOGI(TAG, "切换到非Modbus-RTU模式，停止Modbus任务");
    stop_modbus_tasks();
  }

  // 提交NVS更改
  nvs_commit(nvs_handle);
  nvs_close(nvs_handle);
  
  // 释放NVS互斥锁
  xSemaphoreGive(nvs_mutex);

  // 发送响应
  cJSON *response = cJSON_CreateObject();
  cJSON_AddNumberToObject(response, "code", 200);
  cJSON_AddStringToObject(response, "msg", "success");

  char *json_response = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, json_response);

  free(json_response);
  cJSON_Delete(response);
  cJSON_Delete(root);

  if (start_modbus_after_save) {
    esp_err_t modbus_err = init_modbus_auto_polling();
    if (modbus_err != ESP_OK) {
      ESP_LOGW(TAG, "init_modbus_auto_polling failed: %s",
               esp_err_to_name(modbus_err));
    }
  }

  return ESP_OK;
}

// 添加工作模式信息获取处理函数
static esp_err_t get_work_mode_info_handler(httpd_req_t *req) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    // 处理 NVS 打开失败
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Failed to open NVS");
    return ESP_FAIL;
  }

  int items_offset = 0;
  int items_limit = WORK_MODE_INFO_PAGE_SIZE;
  bool limit_specified = false;
  char query_buf[128];
  if (httpd_req_get_url_query_str(req, query_buf, sizeof(query_buf)) == ESP_OK) {
    char param[16];
    if (httpd_query_key_value(query_buf, "items_offset", param,
                              sizeof(param)) == ESP_OK) {
      items_offset = atoi(param);
      if (items_offset < 0) items_offset = 0;
    }
    if (httpd_query_key_value(query_buf, "items_limit", param,
                              sizeof(param)) == ESP_OK) {
      items_limit = atoi(param);
      limit_specified = true;
    }
  }
  if (!limit_specified) {
    items_limit = WORK_MODE_INFO_PAGE_SIZE;
  }
  if (items_limit < 0) items_limit = 0;
  if (items_limit > MODBUS_ITEM_MAX) items_limit = MODBUS_ITEM_MAX;

  cJSON *response = cJSON_CreateObject();
  if (response == NULL) {
    nvs_close(nvs_handle);
    char err_msg[160];
    snprintf(err_msg, sizeof(err_msg),
             "Failed to create JSON object (heap=%u min=%u) (main/sx_web_server.c)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)esp_get_minimum_free_heap_size());
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
    return ESP_FAIL;
  }

  // 获取工作模式
  char work_mode[32] = "mqtt_tcp";
  size_t len = sizeof(work_mode);
  err = nvs_get_str(nvs_handle, "w_mode", work_mode, &len);
  if (err != ESP_ERR_NVS_NOT_FOUND) { // 如果键不存在使用默认值
    cJSON_AddStringToObject(response, "work_mode", work_mode);
  } else {
    // 如果w_mode不存在，添加默认值mqtt_tcp
    cJSON_AddStringToObject(response, "work_mode", "mqtt_tcp");
  }
  // 获取轮询间隔时间
  char poll_time[32] = "30000"; // 默认值30000ms（30秒）
  size_t poll_time_len = sizeof(poll_time);
  err = nvs_get_str(nvs_handle, "poll_time", poll_time, &poll_time_len);
  if (err != ESP_ERR_NVS_NOT_FOUND) {
    cJSON_AddStringToObject(response, "poll_time", poll_time);
  } else {
    cJSON_AddStringToObject(response, "poll_time",
                            "30000"); // 如果未设置则返回默认值30秒
  }

  // 无论什么工作模式都返回modbus配置，确保配置不丢失
  int32_t items_count = 0;
  err = nvs_get_i32(nvs_handle, "m_count", &items_count);
  ESP_LOGI(TAG, "读取Modbus配置项数量: %" PRId32 " (err: %s)", items_count, esp_err_to_name(err));

  if (err == ESP_OK && items_count > 0) {
    if (items_count > MODBUS_ITEM_MAX) {
      ESP_LOGW(TAG, "m_count超过%d，按上限处理", MODBUS_ITEM_MAX);
      items_count = MODBUS_ITEM_MAX;
    }
  } else {
    items_count = 0;
  }

  cJSON_AddNumberToObject(response, "modbus_items_total", items_count);
  cJSON_AddNumberToObject(response, "modbus_items_offset", items_offset);
  cJSON_AddNumberToObject(response, "modbus_items_limit", items_limit);

  if (items_limit > 0 && items_count > 0) {
    int start = items_offset;
    if (start < 0) start = 0;
    if (start > items_count) start = items_count;
    int end = start + items_limit;
    if (end > items_count) end = items_count;

    cJSON *modbus_items = cJSON_AddArrayToObject(response, "modbus_items");
    if (modbus_items == NULL) {
      cJSON_Delete(response);
      nvs_close(nvs_handle);
      char err_msg[200];
      snprintf(err_msg, sizeof(err_msg),
               "Failed to create modbus items array (count=%" PRId32
               " heap=%u min=%u) (main/sx_web_server.c)",
               items_count, (unsigned)esp_get_free_heap_size(),
               (unsigned)esp_get_minimum_free_heap_size());
      httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
      return ESP_FAIL;
    }

    for (int i = start; i < end; i++) {
      ESP_LOGI(TAG, "处理Modbus配置项 %d", i);
      cJSON *item = cJSON_CreateObject();
      if (item == NULL) {
        ESP_LOGE(TAG, "无法创建配置项 %d 的JSON对象", i);
        cJSON_Delete(response);
        nvs_close(nvs_handle);
        char err_msg[200];
        snprintf(err_msg, sizeof(err_msg),
                 "Failed to create item object idx=%d count=%" PRId32
                 " heap=%u min=%u (main/sx_web_server.c)",
                 i, items_count, (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, err_msg);
        return ESP_FAIL;
      }

      // 读取启用状态
      uint8_t enabled = 0;
      char enabled_key[16];
      snprintf(enabled_key, sizeof(enabled_key), "m%d_en", i);
      esp_err_t enabled_err = nvs_get_u8(nvs_handle, enabled_key, &enabled);
      ESP_LOGI(TAG, "配置项 %d 启用状态: %d (err: %s)", i, enabled,
               esp_err_to_name(enabled_err));
      cJSON_AddBoolToObject(item, "enabled", enabled == 1);

      // 读取其他配置项
      char key[16];
      char value[32];
      size_t value_size = sizeof(value);

      const char *fields[] = {"s_addr",   "f_code",   "r_addr",
                              "r_num",    "timeout",  "d_fmt",
                              "i_time",   "r_fmt",    "baud_rate",
                              "data_bit", "stop_bit", "check_bit"};
      const char *json_fields[] = {
          "slave_addr", "function_code", "register_addr", "register_num",
          "timeout",    "data_format",   "interval_time", "report_format",
          "baud_rate",  "data_bit",      "stop_bit",      "check_bit"};

      for (int j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
        value_size = sizeof(value); // 重置大小
        snprintf(key, sizeof(key), "m%d%s", i, fields[j]);
        esp_err_t field_err = nvs_get_str(nvs_handle, key, value, &value_size);
        if (field_err == ESP_OK) {
          cJSON_AddStringToObject(item, json_fields[j], value);
          ESP_LOGD(TAG, "配置项 %d.%s = %s", i, json_fields[j], value);
        } else {
          ESP_LOGW(TAG, "无法读取配置项 %d.%s: %s", i, json_fields[j],
                   esp_err_to_name(field_err));
          // 添加默认值，防止前端显示异常
          cJSON_AddStringToObject(item, json_fields[j], "");
        }
      }

      cJSON_AddItemToArray(modbus_items, item);
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
}

static esp_err_t get_mqtt_info_get_handler(httpd_req_t *req) {
  esp_err_t ret = nvs_init();
  if (ret != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS init failed");
    return ESP_FAIL;
  }

  nvs_handle_t nvs_handle;
  ret = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (ret != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
    return ESP_FAIL;
  }

  char nvs_use_mqtt[8] = "0";
  char nvs_mqtt_type[8] = "0";
  char nvs_mqtt_server[128] = "";
  char nvs_mqtt_port[16] = "";
  char nvs_mqtt_username[128] = "";
  char nvs_mqtt_password[128] = "";
  char nvs_mqtt_clientid[128] = "";
  char nvs_mqtt_sub_topic[128] = "";
  char nvs_mqtt_pub_topic[128] = "";
  char nvs_qos[8] = "0";
  char nvs_retain[8] = "0";
  char nvs_mqtt_send[8] = "0";
  char nvs_mqtt_time[16] = "5";
  char nvs_mqttconn[8] = "0";

#define READ_NVS_STR(KEY, BUF, FALLBACK)                                      \
  do {                                                                         \
    size_t _len = sizeof(BUF);                                                 \
    if (nvs_get_str(nvs_handle, KEY, BUF, &_len) != ESP_OK) {                 \
      snprintf(BUF, sizeof(BUF), "%s", FALLBACK);                             \
    }                                                                          \
  } while (0)

  READ_NVS_STR("use_mqtt", nvs_use_mqtt, "0");
  READ_NVS_STR("mqtt_type", nvs_mqtt_type, "0");
  READ_NVS_STR("mqtt_server", nvs_mqtt_server, "");
  READ_NVS_STR("mqtt_port", nvs_mqtt_port, "");
  READ_NVS_STR("mqtt_username", nvs_mqtt_username, "");
  READ_NVS_STR("mqtt_password", nvs_mqtt_password, "");
  READ_NVS_STR("mqtt_clientid", nvs_mqtt_clientid, "");
  READ_NVS_STR("mqtt_sub_topic", nvs_mqtt_sub_topic, "");
  READ_NVS_STR("mqtt_pub_topic", nvs_mqtt_pub_topic, "");
  READ_NVS_STR("qos", nvs_qos, "0");
  READ_NVS_STR("retain", nvs_retain, "0");
  READ_NVS_STR("mqtt_send", nvs_mqtt_send, "0");
  READ_NVS_STR("mqtt_time", nvs_mqtt_time, "5");
  READ_NVS_STR("mqttconn", nvs_mqttconn, "0");

#undef READ_NVS_STR

  extern bool get_mqtt_connection_status(void);
  bool mqtt_real_status = get_mqtt_connection_status();
  if (strcmp(nvs_use_mqtt, "1") != 0) {
    mqtt_real_status = false;
  }

  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    nvs_close(nvs_handle);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Create JSON failed");
    return ESP_FAIL;
  }

  cJSON_AddStringToObject(root, "use_mqtt", nvs_use_mqtt);
  cJSON_AddStringToObject(root, "mqtt_type", nvs_mqtt_type);
  cJSON_AddStringToObject(root, "mqtt_server", nvs_mqtt_server);
  cJSON_AddStringToObject(root, "mqtt_port", nvs_mqtt_port);
  cJSON_AddStringToObject(root, "mqtt_username", nvs_mqtt_username);
  cJSON_AddStringToObject(root, "mqtt_password", nvs_mqtt_password);
  cJSON_AddStringToObject(root, "mqtt_clientid", nvs_mqtt_clientid);
  cJSON_AddStringToObject(root, "mqtt_sub_topic", nvs_mqtt_sub_topic);
  cJSON_AddStringToObject(root, "mqtt_pub_topic", nvs_mqtt_pub_topic);
  cJSON_AddStringToObject(root, "qos", nvs_qos);
  cJSON_AddStringToObject(root, "retain", nvs_retain);
  cJSON_AddStringToObject(root, "mqtt_send", nvs_mqtt_send);
  cJSON_AddStringToObject(root, "mqtt_time", nvs_mqtt_time);
  cJSON_AddStringToObject(root, "mqttconn", mqtt_real_status ? "1" : "0");

  httpd_resp_set_hdr(req, "Connection", "close");
  httpd_resp_set_type(req, "application/json");
  char *json_data = cJSON_Print(root);
  if (json_data == NULL) {
    cJSON_Delete(root);
    nvs_close(nvs_handle);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Print JSON failed");
    return ESP_FAIL;
  }

  httpd_resp_send(req, json_data, strlen(json_data));

  free(json_data);
  cJSON_Delete(root);
  nvs_close(nvs_handle);
  return ESP_OK;
}

static esp_err_t get_tcp_info_get_handler(httpd_req_t *req) {
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t use_tcp, tcpconn, tcp_server, tcp_port = 0;
  size_t reg_packet = 0, heart_packet = 0, reg_format = 0, heart_format = 0, heart_interval = 0;
  ret = nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp);
  nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn);
  nvs_get_str(nvs_handle, "tcp_server", NULL, &tcp_server);
  nvs_get_str(nvs_handle, "tcp_port", NULL, &tcp_port);
  nvs_get_str(nvs_handle, "reg_packet", NULL, &reg_packet);
  nvs_get_str(nvs_handle, "heart_packet", NULL, &heart_packet);
  nvs_get_str(nvs_handle, "reg_format", NULL, &reg_format);
  nvs_get_str(nvs_handle, "heart_format", NULL, &heart_format);
  nvs_get_str(nvs_handle, "heart_interval", NULL, &heart_interval);
  switch (ret) {
  case ESP_OK:
    printf("TCP set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    printf("no TCP set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_tcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcpconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_server", "192.168.1.100"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_port", "8888"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "reg_packet", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "heart_packet", device_mac)); // 默认WiFi MAC地址（无冒号格式）
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "reg_format", "hex"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "heart_format", "hex"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "heart_interval", "30"));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_use_tcp = malloc(use_tcp);
    char *nvs_tcpconn = malloc(tcpconn);
    char *nvs_tcp_server = malloc(tcp_server);
    char *nvs_tcp_port = malloc(tcp_port);
    char *nvs_reg_packet = reg_packet > 0 ? malloc(reg_packet) : NULL;
    char *nvs_heart_packet = heart_packet > 0 ? malloc(heart_packet) : NULL;
    char *nvs_reg_format = reg_format > 0 ? malloc(reg_format) : NULL;
    char *nvs_heart_format = heart_format > 0 ? malloc(heart_format) : NULL;
    char *nvs_heart_interval = heart_interval > 0 ? malloc(heart_interval) : NULL;

    if (nvs_tcp_port == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "use_tcp", nvs_use_tcp, &use_tcp);
      nvs_get_str(nvs_handle, "tcpconn", nvs_tcpconn, &tcpconn);
      nvs_get_str(nvs_handle, "tcp_server", nvs_tcp_server, &tcp_server);
      nvs_get_str(nvs_handle, "tcp_port", nvs_tcp_port, &tcp_port);
      if (nvs_reg_packet) nvs_get_str(nvs_handle, "reg_packet", nvs_reg_packet, &reg_packet);
      if (nvs_heart_packet) nvs_get_str(nvs_handle, "heart_packet", nvs_heart_packet, &heart_packet);
      if (nvs_reg_format) nvs_get_str(nvs_handle, "reg_format", nvs_reg_format, &reg_format);
      if (nvs_heart_format) nvs_get_str(nvs_handle, "heart_format", nvs_heart_format, &heart_format);
      if (nvs_heart_interval) nvs_get_str(nvs_handle, "heart_interval", nvs_heart_interval, &heart_interval);

      int tcp_mode = 0;
      if (nvs_tcpconn && nvs_tcpconn[0] != '\0') {
        tcp_mode = atoi(nvs_tcpconn);
      }

      if (ret == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "use_tcp", cJSON_CreateString(nvs_use_tcp));
        cJSON_AddItemToObject(root, "tcpconn", cJSON_CreateString(nvs_tcpconn));
        cJSON_AddItemToObject(root, "tcp_server",
                              cJSON_CreateString(nvs_tcp_server));
        const char *tcp_port_value =
            (nvs_tcp_port && nvs_tcp_port[0] != '\0')
                ? nvs_tcp_port
                : (TCP_MODE_IS_MODBUS(tcp_mode) ? "502" : "8888");
        cJSON_AddItemToObject(root, "tcp_port",
                              cJSON_CreateString(tcp_port_value));
        cJSON_AddItemToObject(root, "tcp_client_count",
                              cJSON_CreateNumber(tcp_client_count));
        cJSON_AddItemToObject(root, "tcp_server_conn",
                              cJSON_CreateNumber(global_tcp_server));

        // 添加注册包和心跳包信息
        if (nvs_reg_packet) {
          cJSON_AddItemToObject(root, "reg_packet", cJSON_CreateString(nvs_reg_packet));
        } else {
          cJSON_AddItemToObject(root, "reg_packet", cJSON_CreateString(""));
        }

        if (nvs_heart_packet) {
          cJSON_AddItemToObject(root, "heart_packet", cJSON_CreateString(nvs_heart_packet));
        } else if (TCP_MODE_IS_MODBUS_CLIENT(tcp_mode)) {
          cJSON_AddItemToObject(root, "heart_packet", cJSON_CreateString(""));
        } else {
          cJSON_AddItemToObject(root, "heart_packet", cJSON_CreateString(device_mac));
        }

        if (nvs_reg_format) {
          cJSON_AddItemToObject(root, "reg_format", cJSON_CreateString(nvs_reg_format));
        } else {
          cJSON_AddItemToObject(root, "reg_format", cJSON_CreateString("hex"));
        }

        if (nvs_heart_format) {
          cJSON_AddItemToObject(root, "heart_format", cJSON_CreateString(nvs_heart_format));
        } else if (TCP_MODE_IS_MODBUS_CLIENT(tcp_mode)) {
          cJSON_AddItemToObject(root, "heart_format", cJSON_CreateString("none"));
        } else {
          cJSON_AddItemToObject(root, "heart_format", cJSON_CreateString("hex"));
        }

        if (nvs_heart_interval) {
          cJSON_AddItemToObject(root, "heart_interval", cJSON_CreateString(nvs_heart_interval));
        } else {
          cJSON_AddItemToObject(root, "heart_interval", cJSON_CreateString("30"));
        }

        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));
        free(json_data);
        cJSON_Delete(root);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_tcp);
      free(nvs_tcpconn);
      free(nvs_tcp_server);
      free(nvs_tcp_port);
      if (nvs_reg_packet) free(nvs_reg_packet);
      if (nvs_heart_packet) free(nvs_heart_packet);
      if (nvs_reg_format) free(nvs_reg_format);
      if (nvs_heart_format) free(nvs_heart_format);
      if (nvs_heart_interval) free(nvs_heart_interval);
    }
  }
  nvs_close(nvs_handle);
  return ESP_OK;
}



static esp_err_t get_http_info_get_handler(httpd_req_t *req) {
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t use_http, http_port, httpconn, http_url, http_header, http_time = 0;
  ret = nvs_get_str(nvs_handle, "use_http", NULL, &use_http);
  nvs_get_str(nvs_handle, "http_port", NULL, &http_port);
  nvs_get_str(nvs_handle, "httpconn", NULL, &httpconn);
  nvs_get_str(nvs_handle, "http_url", NULL, &http_url);
  nvs_get_str(nvs_handle, "http_header", NULL, &http_header);
  nvs_get_str(nvs_handle, "http_time", NULL, &http_time);
  switch (ret) {
  case ESP_OK:
    printf("HTTP set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_port", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "httpconn", ""));
    ESP_ERROR_CHECK(
        nvs_set_str(nvs_handle, "http_url", "http://demo.sxiot.com"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_header", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_time", "5"));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'HTTP': %s\n", esp_err_to_name(ret));
  } else {
    char *nvs_use_http = malloc(use_http);
    char *nvs_http_port = malloc(http_port);
    char *nvs_httpconn = malloc(httpconn);
    char *nvs_http_url = malloc(http_url);
    char *nvs_http_header = malloc(http_header);
    char *nvs_http_time = malloc(http_time);

    if (nvs_use_http == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "use_http", nvs_use_http, &use_http);
      nvs_get_str(nvs_handle, "http_port", nvs_http_port, &http_port);
      nvs_get_str(nvs_handle, "httpconn", nvs_httpconn, &httpconn);
      nvs_get_str(nvs_handle, "http_url", nvs_http_url, &http_url);
      nvs_get_str(nvs_handle, "http_header", nvs_http_header, &http_header);
      nvs_get_str(nvs_handle, "http_time", nvs_http_time, &http_time);

      if (ret == ESP_OK) {
        printf("Value for 'use_http' is %s\n", nvs_use_http);
        printf("Value for 'http_port' is %s\n", nvs_http_port);
        printf("Value for 'httpconn' is %s\n", nvs_httpconn);
        printf("Value for 'http_url' is %s\n", nvs_http_url);
        printf("Value for 'http_header' is %s\n", nvs_http_header);
        printf("Value for 'http_time' is %s\n", nvs_http_time);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "use_http",
                              cJSON_CreateString(nvs_use_http));
        cJSON_AddItemToObject(root, "http_port",
                              cJSON_CreateString(nvs_http_port));
        cJSON_AddItemToObject(root, "httpconn",
                              cJSON_CreateString(nvs_httpconn));
        cJSON_AddItemToObject(root, "http_url",
                              cJSON_CreateString(nvs_http_url));
        cJSON_AddItemToObject(root, "http_header",
                              cJSON_CreateString(nvs_http_header));
        cJSON_AddItemToObject(root, "http_time",
                              cJSON_CreateString(nvs_http_time));
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));
        free(json_data);
        cJSON_Delete(root);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_use_http);
      free(nvs_http_port);
      free(nvs_httpconn);
      free(nvs_http_url);
      free(nvs_http_header);
      free(nvs_http_time);
    }
  }
  nvs_close(nvs_handle);
  return ESP_OK;
}

// WIFI GET Handler
static esp_err_t get_find_wifi_get_handler(httpd_req_t *req) {

  WifiValueList data = wifiSearchValue();
  ESP_LOGI(TAG, "Total APs scanned = %u", data.ap_count);
  cJSON *root = cJSON_CreateObject();

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
                              "当前无法进行WiFi扫描，请稍后再试");
      cJSON_AddNumberToObject(root, "wait_time", 3000); // 默认等待5秒
      break;
    case 3: // 其他错误
    default:
      ESP_LOGI(TAG, "WiFi scan failed: other error");
      cJSON_AddStringToObject(root, "error", "WiFi扫描失败，请稍后再试");
      cJSON_AddNumberToObject(root, "wait_time", 3000); // 默认等待3秒
      break;
    }
  } else if (data.ap_count == 0) {
    // 扫描成功但未找到WiFi
    ESP_LOGI(TAG, "WiFi scan completed but no networks found");
    cJSON_AddStringToObject(root, "message", "未找到任何WiFi网络");
  } else {
    // 扫描成功，返回AP列表
    //   cJSON *wifiArray = cJSON_CreateArray();

    // 确保不会超出数组边界
    int count = (data.ap_count < DEFAULT_SCAN_LIST_SIZE)
                    ? data.ap_count
                    : DEFAULT_SCAN_LIST_SIZE;
    for (int i = 0; i < count; i++) {
      ESP_LOGI(TAG, "SSID \t\t%s", data.ap_info[i].ssid);
      ESP_LOGI(TAG, "RSSI \t\t%d", data.ap_info[i].rssi);
      ESP_LOGI(TAG, "Channel \t\t%d\n", data.ap_info[i].primary);
      // cJSON_AddStringToObject(root, "ssid", );
      cJSON_AddNumberToObject(root, (char *)data.ap_info[i].ssid,
                              data.ap_info[i].rssi);
    }
  }

  return safe_json_response(req, root);

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
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

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
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", ""));
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

static esp_err_t get_module_set_info_get_handler(httpd_req_t *req) {

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  size_t host_names, ntp_server, lgname, lgpwd = 0;

  ret = nvs_get_str(nvs_handle, "lgname", NULL, &lgname);
  nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd);
  nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server);

  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "以太网串口服务器"));
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

static esp_err_t get_ap_set_info_get_handler(httpd_req_t *req) {

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));
  size_t ap_name, ap_password, ap_wait_time = 0;
  ret = nvs_get_str(nvs_handle, "ap_name", NULL, &ap_name);
  nvs_get_str(nvs_handle, "ap_password", NULL, &ap_password);
  nvs_get_str(nvs_handle, "ap_wait_time", NULL, &ap_wait_time);
  switch (ret) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_name", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_password", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_wait_time", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'ap_name': %s\n", esp_err_to_name(ret));
  } else {
    // 分配缓冲区
    char *nvs_ap_name = malloc(ap_name);
    char *nvs_ap_password = malloc(ap_password);
    char *nvs_ap_wait_time = malloc(ap_wait_time);
    if (nvs_ap_name == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "ap_name", nvs_ap_name, &ap_name);
      nvs_get_str(nvs_handle, "ap_password", nvs_ap_password, &ap_password);
      nvs_get_str(nvs_handle, "ap_wait_time", nvs_ap_wait_time, &ap_wait_time);
      if (ret == ESP_OK) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddItemToObject(root, "ap_name", cJSON_CreateString(nvs_ap_name));
        cJSON_AddItemToObject(root, "ap_password",
                              cJSON_CreateString(nvs_ap_password));
        cJSON_AddItemToObject(root, "ap_wait_time",
                              cJSON_CreateString(nvs_ap_wait_time));
        httpd_resp_set_hdr(req, "Connection", "close");
        httpd_resp_set_type(req, "application/json");
        char *json_data = cJSON_Print(root);
        httpd_resp_send(req, json_data, strlen(json_data));
        free(json_data);
        cJSON_Delete(root);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }
      free(nvs_ap_name);
      free(nvs_ap_password);
      free(nvs_ap_wait_time);
    }
  }

  nvs_close(nvs_handle);
  return ESP_OK;
}


// WebSocket消息发送任务
static void ws_send_task(void *pvParameters) {
  static char msg_buffer[WS_MAX_MSG_LEN];
  httpd_ws_frame_t ws_pkt = {
      .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)msg_buffer, .len = 0};

  while (1) {
    if (xQueueReceive(ws_msg_queue, msg_buffer, portMAX_DELAY)) {
      ws_pkt.len = strlen(msg_buffer);

      // 遍历所有客户端发送消息
      for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (ws_clients[i].in_use) {
          esp_err_t ret = httpd_ws_send_frame_async(
              http_server, ws_clients[i].client_fd, &ws_pkt);
          if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to send WS message to client %d: %d", i, ret);
            if (http_server) {
              httpd_sess_trigger_close(http_server, ws_clients[i].client_fd);
            }
            // 如果发送失败，移除客户端
            ws_reset_client_slot(i, "send frame failed");
          }
        }
      }
    }
    // 添加小延时避免过度消耗CPU
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// 初始化WebSocket系统
static void init_websocket(void) {
  // 创建消息队列
  ws_msg_queue = xQueueCreate(WS_QUEUE_SIZE, WS_MAX_MSG_LEN);
  if (ws_msg_queue == NULL) {
    ESP_LOGE(TAG, "Failed to create WS message queue");
    return;
  }

  // 创建WebSocket发送任务
  BaseType_t ret = xTaskCreate(ws_send_task, "ws_send_task",
                               5120, // 栈大小调整为5KB，进一步节省堆
                               NULL,
                               5, // 较高优先级
                               &ws_task_handle);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create WS task");
    vQueueDelete(ws_msg_queue);
    ws_msg_queue = NULL;
  }
}

// WebSocket处理函数
static esp_err_t ws_handler(httpd_req_t *req) {
  if (req->method == HTTP_GET) {
    char client_tag[WS_CLIENT_TAG_MAX] = {0};
    ws_fill_client_tag(req, client_tag, sizeof(client_tag));
    ESP_LOGI(TAG, "Incoming WS handshake (tag=%s)",
             client_tag[0] ? client_tag : "-");
    if (client_tag[0] != '\0' &&
        ws_remove_client_by_tag(client_tag, "duplicate tag")) {
      ESP_LOGW(TAG,
               "WS tag %s conflict detected, dropping new handshake and letting clients reconnect",
               client_tag);
      if (http_server) {
        httpd_sess_trigger_close(http_server, httpd_req_to_sockfd(req));
      }
      return ESP_FAIL;
    }

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
      if (client_tag[0] != '\0') {
        strncpy(ws_clients[client_slot].client_tag, client_tag,
                sizeof(ws_clients[client_slot].client_tag) - 1);
        ws_clients[client_slot]
            .client_tag[sizeof(ws_clients[client_slot].client_tag) - 1] = '\0';
        ESP_LOGI(TAG, "New WS client connected, slot: %d, tag: %s",
                 client_slot, ws_clients[client_slot].client_tag);
      } else {
        ws_clients[client_slot].client_tag[0] = '\0';
        ESP_LOGI(TAG, "New WS client connected, slot: %d", client_slot);
      }
    } else {
      ESP_LOGW(TAG, "No free WS client slots");
      return ESP_FAIL;
    }
    return ESP_OK;
  }

  httpd_ws_frame_t frame = {
      .type = HTTPD_WS_TYPE_TEXT,
      .payload = NULL,
      .len = 0,
  };

  int client_fd = httpd_req_to_sockfd(req);
  esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
  if (ret != ESP_OK) {
    int idx = ws_find_client_by_fd(client_fd);
    if (idx >= 0) {
      ESP_LOGW(TAG, "WS recv frame failed (fd=%d, err=%s)", client_fd,
               esp_err_to_name(ret));
      ws_reset_client_slot(idx, "recv frame error");
    }
    return ret;
  }

  if (frame.len > 0) {
    frame.payload = malloc(frame.len + 1);
    if (!frame.payload) {
      return ESP_ERR_NO_MEM;
    }
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret != ESP_OK) {
      free(frame.payload);
      return ret;
    }
    frame.payload[frame.len] = '\0';
  }

  if (frame.type == HTTPD_WS_TYPE_CLOSE) {
    int idx = ws_find_client_by_fd(client_fd);
    if (idx >= 0) {
      ESP_LOGI(TAG, "WS client requested close (fd=%d)", client_fd);
      ws_reset_client_slot(idx, "close frame");
    }
    if (frame.payload) {
      free(frame.payload);
    }
    return ESP_OK;
  }

  if (frame.len == 0 || frame.type != HTTPD_WS_TYPE_TEXT) {
    if (frame.payload) {
      free(frame.payload);
    }
    return ESP_OK;
  }

  cJSON *json = cJSON_Parse((char *)frame.payload);
  free(frame.payload);
  if (!json) {
    ws_send_uart_ack(req, NULL, 0, 0, false, "invalid json");
    return ESP_FAIL;
  }

  cJSON *msg_type = cJSON_GetObjectItemCaseSensitive(json, "type");
  bool is_uart_tx = !msg_type ||
                    (cJSON_IsString(msg_type) &&
                     strcmp(msg_type->valuestring, "uart_tx") == 0);
  if (!is_uart_tx) {
    cJSON_Delete(json);
    return ESP_OK;
  }

  cJSON *instruction = cJSON_GetObjectItemCaseSensitive(json, "instruction");
  cJSON *sendType = cJSON_GetObjectItemCaseSensitive(json, "sendType");
  cJSON *chunkIndex = cJSON_GetObjectItemCaseSensitive(json, "chunkIndex");
  cJSON *chunkTotal = cJSON_GetObjectItemCaseSensitive(json, "chunkTotal");
  cJSON *transferId = cJSON_GetObjectItemCaseSensitive(json, "transferId");

  int chunk_index_val = cJSON_IsNumber(chunkIndex) ? chunkIndex->valueint : 0;
  int chunk_total_val = cJSON_IsNumber(chunkTotal) ? chunkTotal->valueint : 1;
  const char *transfer_id_str =
      (cJSON_IsString(transferId) && transferId->valuestring &&
       transferId->valuestring[0] != '\0')
          ? transferId->valuestring
          : NULL;
  bool is_hex_mode =
      (cJSON_IsString(sendType) && sendType->valuestring &&
       strcmp(sendType->valuestring, "hex") == 0);

  if (!cJSON_IsString(instruction) || !instruction->valuestring) {
    ws_send_uart_ack(req, transfer_id_str, chunk_index_val, chunk_total_val,
                     false, "instruction missing");
    cJSON_Delete(json);
    return ESP_FAIL;
  }

  if (chunk_total_val > 1 && transfer_id_str == NULL) {
    ws_send_uart_ack(req, NULL, chunk_index_val, chunk_total_val, false,
                     "missing transferId");
    cJSON_Delete(json);
    return ESP_FAIL;
  }

  uint8_t *chunk_data = NULL;
  size_t chunk_len = 0;
  esp_err_t decode_ret = decode_instruction_payload(instruction->valuestring,
                                                    is_hex_mode, &chunk_data,
                                                    &chunk_len);
  if (decode_ret != ESP_OK) {
    const char *msg =
        (decode_ret == ESP_ERR_NO_MEM) ? "no memory" : "invalid payload";
    ws_send_uart_ack(req, transfer_id_str, chunk_index_val, chunk_total_val,
                     false, msg);
    cJSON_Delete(json);
    if (decode_ret == ESP_ERR_NO_MEM) {
      return ESP_ERR_NO_MEM;
    }
    return ESP_FAIL;
  }

  esp_err_t chunk_ret = process_uart_chunk(chunk_data, chunk_len,
                                           chunk_index_val, chunk_total_val,
                                           transfer_id_str);
  if (chunk_ret != ESP_OK) {
    const char *msg = (chunk_ret == ESP_ERR_NO_MEM)
                          ? "chunk buffer no memory"
                          : "chunk process failed";
    ws_send_uart_ack(req, transfer_id_str, chunk_index_val, chunk_total_val,
                     false, msg);
    cJSON_Delete(json);
    if (chunk_ret == ESP_ERR_NO_MEM) {
      return ESP_ERR_NO_MEM;
    }
    return ESP_FAIL;
  }

  ws_send_uart_ack(req, transfer_id_str, chunk_index_val, chunk_total_val, true,
                   "ok");
  cJSON_Delete(json);

  return ESP_OK;
}

// 发送日志到WebSocket的函数
void send_log_to_websocket(const char *log_msg) {
  // 如果日志开关关闭，直接返回
  if (!log_enabled) {
    return;
  }

  if (!ws_msg_queue) {
    return;
  }

  // 创建临时缓冲区并复制消息
  char buffer[WS_MAX_MSG_LEN];
  strncpy(buffer, log_msg, WS_MAX_MSG_LEN - 1);
  buffer[WS_MAX_MSG_LEN - 1] = '\0';

  // 使用超时时间发送到队列
  if (xQueueSend(ws_msg_queue, buffer, pdMS_TO_TICKS(100)) != pdPASS) {
    if (!ws_queue_warn_logged) {
      ws_queue_warn_logged = true;
      ESP_EARLY_LOGW(TAG, "WS message queue full");
    }
  } else if (ws_queue_warn_logged) {
    ws_queue_warn_logged = false;
  }
}

// 发送串口数据到WebSocket的函数
void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx) {
  if (!ws_msg_queue) {
    return;
  }

  const size_t chunk_size = WS_UART_CHUNK_SIZE;
  uint64_t frame_id = esp_timer_get_time();

  size_t chunk_total = (len == 0) ? 1 : ((len + chunk_size - 1) / chunk_size);
  for (size_t chunk_index = 0; chunk_index < chunk_total; chunk_index++) {
    size_t offset = (len == 0) ? 0 : chunk_index * chunk_size;
    size_t chunk_len = (len == 0)
                           ? 0
                           : ((offset + chunk_size) > len ? (len - offset)
                                                          : chunk_size);
    const uint8_t *chunk_ptr = (len == 0) ? NULL : (data + offset);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
      ESP_LOGE(TAG, "Failed to create JSON object for UART data");
      return;
    }

    cJSON_AddStringToObject(root, "type", "uart_data");
    cJSON_AddBoolToObject(root, "is_tx", is_tx);
    cJSON_AddNumberToObject(root, "timestamp", (double)frame_id);
    cJSON_AddNumberToObject(root, "frameId", (double)frame_id);
    cJSON_AddNumberToObject(root, "len", len);
    cJSON_AddNumberToObject(root, "preview", chunk_len);
    cJSON_AddNumberToObject(root, "chunkIndex", chunk_index);
    cJSON_AddNumberToObject(root, "chunkTotal", chunk_total);
    cJSON_AddNumberToObject(root, "chunkLen", chunk_len);
    cJSON_AddNumberToObject(root, "offset", offset);

    char hex_str[WS_UART_CHUNK_SIZE * 3 + 16];
    size_t hex_pos = 0;
    for (size_t i = 0; i < chunk_len && hex_pos < sizeof(hex_str) - 4; i++) {
      hex_pos += snprintf(hex_str + hex_pos, sizeof(hex_str) - hex_pos, "%02X ",
                          chunk_ptr ? chunk_ptr[i] : 0);
    }
    hex_str[hex_pos] = '\0';
    cJSON_AddStringToObject(root, "hex", hex_str);

    char ascii_str[WS_UART_CHUNK_SIZE + 1];
    size_t ascii_len = chunk_len < WS_UART_CHUNK_SIZE ? chunk_len : WS_UART_CHUNK_SIZE;
    for (size_t i = 0; i < ascii_len; i++) {
      ascii_str[i] = (chunk_ptr && isprint(chunk_ptr[i])) ? chunk_ptr[i] : '.';
    }
    ascii_str[ascii_len] = '\0';
    cJSON_AddStringToObject(root, "ascii", ascii_str);

    char *json_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_str) {
      ESP_LOGE(TAG, "Failed to serialize UART JSON payload");
      return;
    }

    size_t json_len = strlen(json_str);
    if (json_len >= WS_MAX_MSG_LEN) {
      ESP_LOGE(TAG, "UART chunk %zu/%zu payload too large (%zu bytes)",
               chunk_index + 1, chunk_total, json_len);
      free(json_str);
      return;
    }

    char buffer[WS_MAX_MSG_LEN];
    strncpy(buffer, json_str, WS_MAX_MSG_LEN - 1);
    buffer[WS_MAX_MSG_LEN - 1] = '\0';
    free(json_str);

    if (xQueueSend(ws_msg_queue, buffer, 0) != pdPASS) {
      ESP_LOGW(TAG, "WS queue full, dropping UART chunk %zu/%zu", chunk_index + 1,
               chunk_total);
    }
  }
}

/* 暂时不使用的HTTP日志API（已被WebSocket替代）
static esp_err_t get_log_handler(httpd_req_t *req) {
  esp_err_t ret = ESP_OK;
  char *log_content = NULL;
  char *json_data = NULL;

  // 创建JSON根对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "Failed to create JSON object");
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                               "Failed to create JSON object");
  }

  // 根据URL参数决定返回哪种日志
  char param[32] = {0};
  if (httpd_req_get_url_query_str(req, param, sizeof(param)) == ESP_OK) {
    char type[8] = {0};
    if (httpd_query_key_value(param, "type", type, sizeof(type)) == ESP_OK) {
      if (strcmp(type, "simple") == 0) {
        log_content = sx_log_get_simple_buffer();
        ESP_LOGI(TAG, "Getting simple log buffer");
      } else {
        log_content = sx_log_get_buffer();
        ESP_LOGI(TAG, "Getting full log buffer");
      }
    }
  } else {
    // 默认返回详细日志
    log_content = sx_log_get_buffer();
    ESP_LOGI(TAG, "Getting default full log buffer");
  }

  // 添加日志内容到JSON对象
  if (log_content != NULL) {
    if (!cJSON_AddStringToObject(root, "content", log_content)) {
      ESP_LOGE(TAG, "Failed to add log content to JSON");
      ret = ESP_FAIL;
      goto cleanup;
    }
    free(log_content);
    log_content = NULL;
  } else {
    if (!cJSON_AddStringToObject(root, "content", "暂无日志")) {
      ESP_LOGE(TAG, "Failed to add empty log message to JSON");
      ret = ESP_FAIL;
      goto cleanup;
    }
  }

  // 生成JSON字符串
  json_data = cJSON_Print(root);
  if (!json_data) {
    ESP_LOGE(TAG, "Failed to generate JSON string");
    ret = ESP_FAIL;
    goto cleanup;
  }

  // 设置响应类型和发送响应
  httpd_resp_set_type(req, "application/json");
  ret = httpd_resp_send(req, json_data, strlen(json_data));
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to send HTTP response: %d", ret);
  }

cleanup:
  // 清理资源
  if (log_content) {
    free(log_content);
  }
  if (json_data) {
    free(json_data);
  }
  if (root) {
    cJSON_Delete(root);
  }

  return ret;
}

static esp_err_t clear_log_handler(httpd_req_t *req) {
  sx_log_clear();

  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "code", 200);

  char *json_data = cJSON_Print(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));

  free(json_data);
  cJSON_Delete(root);
  return ESP_OK;
}
*/

// 获取日志开关状态
static esp_err_t get_log_switch_handler(httpd_req_t *req) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "code", 200);
  cJSON_AddBoolToObject(root, "log_enabled", log_enabled);

  char *json_data = cJSON_Print(root);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));

  free(json_data);
  cJSON_Delete(root);
  return ESP_OK;
}

// 设置日志开关状态
static esp_err_t set_log_switch_handler(httpd_req_t *req) {
  char buf[128];
  int ret = 0;
  int remaining = req->content_len;

  if (remaining >= sizeof(buf)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
    return ESP_FAIL;
  }

  if (remaining == 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty content");
    return ESP_FAIL;
  }

  // 读取POST数据
  ret = httpd_req_recv(req, buf, remaining);
  if (ret <= 0) {
    if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
      httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request timeout");
    }
    return ESP_FAIL;
  }
  buf[ret] = '\0';

  // 解析JSON
  cJSON *json = cJSON_Parse(buf);
  if (json == NULL) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    return ESP_FAIL;
  }

  cJSON *enabled = cJSON_GetObjectItem(json, "log_enabled");
  if (enabled == NULL || !cJSON_IsBool(enabled)) {
    cJSON_Delete(json);
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing log_enabled field");
    return ESP_FAIL;
  }

  // 更新全局变量
  log_enabled = cJSON_IsTrue(enabled);

  // 保存到NVS
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    nvs_set_u8(nvs_handle, "log_enabled", log_enabled ? 1 : 0);
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "日志开关已%s", log_enabled ? "打开" : "关闭");
  } else {
    ESP_LOGE(TAG, "保存日志开关状态失败");
  }

  cJSON_Delete(json);

  // 返回成功响应
  cJSON *response = cJSON_CreateObject();
  cJSON_AddNumberToObject(response, "code", 200);
  cJSON_AddBoolToObject(response, "log_enabled", log_enabled);

  char *json_data = cJSON_Print(response);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, json_data, strlen(json_data));

  free(json_data);
  cJSON_Delete(response);
  return ESP_OK;
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
    cJSON_Delete(json);
    return 0;
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
  ESP_ERROR_CHECK(nvs_open("nvs_namespace", NVS_READWRITE, &nvs_handle));

  size_t host_names, ntp_server, lgname, lgpwd = 0;

  rets = nvs_get_str(nvs_handle, "lgname", NULL, &lgname);
  nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd);
  nvs_get_str(nvs_handle, "host_names", NULL, &host_names);
  nvs_get_str(nvs_handle, "ntp_server", NULL, &ntp_server);

  switch (rets) {
  case ESP_OK:
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "以太网串口服务器"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ntp_server", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgname", "admin"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgpwd", "12345678"));
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
        if (strcmp(username->valuestring, nvs_lgname) == 0 &&
            strcmp(password->valuestring, nvs_lgpwd) == 0) {
          printf("login success\n");
          httpd_resp_send(req, "success", HTTPD_RESP_USE_STRLEN);
        } else {
          printf("login fail\n");
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
    .uri = "/status", .method = HTTP_POST, .handler = get_status_get_handler};

static const httpd_uri_t js_uri = {
    .uri = "/web.js", .method = HTTP_GET, .handler = js_get_handler};

static const httpd_uri_t css_uri = {
    .uri = "/web.css", .method = HTTP_GET, .handler = css_get_handler};

static const httpd_uri_t brand_config_js_uri = {
    .uri = "/brand-config.js", .method = HTTP_GET, .handler = brand_config_js_get_handler};

static const httpd_uri_t i18n_js_uri = {
    .uri = "/i18n.js", .method = HTTP_GET, .handler = i18n_js_get_handler};

static const httpd_uri_t zh_cn_json_uri = {
    .uri = "/i18n/zh-CN.json", .method = HTTP_GET, .handler = zh_cn_json_get_handler};

static const httpd_uri_t en_us_json_uri = {
    .uri = "/i18n/en-US.json", .method = HTTP_GET, .handler = en_us_json_get_handler};

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

static const httpd_uri_t get_mqtt = {
    .uri = "/mqtt", .method = HTTP_POST, .handler = get_mqtt_post_handler};

static const httpd_uri_t get_net_set = {.uri = "/net_set",
                                        .method = HTTP_POST,
                                        .handler = get_net_set_post_handler};

static const httpd_uri_t get_net_set_info = {.uri = "/net_set_info",
                                             .method = HTTP_GET,
                                             .handler =
                                                 get_net_set_info_get_handler};

static const httpd_uri_t get_mqtt_info = {.uri = "/mqtt_info",
                                          .method = HTTP_GET,
                                          .handler = get_mqtt_info_get_handler};
static const httpd_uri_t get_tcp_info = {.uri = "/tcp_info",
                                         .method = HTTP_GET,
                                         .handler = get_tcp_info_get_handler};

static const httpd_uri_t get_http_info = {.uri = "/http_info",
                                          .method = HTTP_GET,
                                          .handler = get_http_info_get_handler};
static const httpd_uri_t get_tcp = {
    .uri = "/tcp", .method = HTTP_POST, .handler = get_tcp_post_handler};


static const httpd_uri_t get_http = {
    .uri = "/http", .method = HTTP_POST, .handler = get_http_post_handler};

static const httpd_uri_t get_update = {
    .uri = "/update", .method = HTTP_POST, .handler = get_update_post_handler};

static const httpd_uri_t get_ota = {
    .uri = "/ota", .method = HTTP_POST, .handler = get_ota_post_handler};

static const httpd_uri_t get_find_wifi = {.uri = "/find_wifi",
                                          .method = HTTP_GET,
                                          .handler = get_find_wifi_get_handler};

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

static const httpd_uri_t work_mode_set = {.uri = "/work_mode_set",
                                          .method = HTTP_POST,
                                          .handler = get_work_mode_set_handler};

static const httpd_uri_t work_mode_info = {.uri = "/work_mode_info",
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

static const httpd_uri_t get_ap_set_info = {.uri = "/ap_set_info",
                                            .method = HTTP_GET,
                                            .handler =
                                                get_ap_set_info_get_handler};

static const httpd_uri_t log_ws = {.uri = "/ws/log",
                               .method = HTTP_GET,
                               .handler = ws_handler,
                               .user_ctx = NULL,
                               .is_websocket = true};

static const httpd_uri_t ws_disconnect_uri = {
    .uri = "/ws/disconnect",
    .method = HTTP_POST,
    .handler = ws_disconnect_post_handler,
    .user_ctx = NULL};

static const httpd_uri_t web_login = {
    .uri = "/login", .method = HTTP_POST, .handler = web_login_handler};

static const httpd_uri_t get_ota_progress = {.uri = "/ota_progress",
                                             .method = HTTP_GET,
                                             .handler =
                                                 get_ota_progress_handler,
                                             .user_ctx = NULL};

static const httpd_uri_t get_log_switch = {.uri = "/api/log_switch",
                                           .method = HTTP_GET,
                                           .handler = get_log_switch_handler,
                                           .user_ctx = NULL};

static const httpd_uri_t set_log_switch = {.uri = "/api/log_switch",
                                           .method = HTTP_POST,
                                           .handler = set_log_switch_handler,
                                           .user_ctx = NULL};

static httpd_handle_t start_webserver(void) {
  // 从NVS读取日志开关状态
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &nvs_handle);
  if (err == ESP_OK) {
    uint8_t log_state = 0;
    err = nvs_get_u8(nvs_handle, "log_enabled", &log_state);
    if (err == ESP_OK) {
      log_enabled = (log_state != 0);
      ESP_LOGI(TAG, "从NVS加载日志开关状态: %s", log_enabled ? "打开" : "关闭");
    } else {
      ESP_LOGI(TAG, "NVS中未找到日志开关配置，使用默认值: 关闭");
    }
    nvs_close(nvs_handle);
  } else {
    ESP_LOGW(TAG, "无法打开NVS读取日志开关状态");
  }

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.stack_size = 8192;     // 增加栈大小到8KB以避免看门狗超时（原6KB导致问题）
  config.max_open_sockets = 13; // 减少最大连接数到13以节省内存
  config.max_uri_handlers = 48;
  config.task_priority = 5;     // 使用默认优先级5，确保任务调度正常
  config.recv_wait_timeout = 10; // 增加接收超时避免阻塞（原2秒）
  config.send_wait_timeout = 10; // 增加发送超时避免阻塞（原2秒）

  config.keep_alive_enable = true;
  config.keep_alive_idle = 5; // 增加空闲时间，减少CPU占用
  config.keep_alive_interval = 3;
  config.keep_alive_count = 3;
  config.lru_purge_enable = true;
  config.enable_so_linger = true;
  config.linger_timeout = 1;

  ESP_LOGI(TAG, "正在端口 '%d' 上启动http_server，允许最大 %d 个连接",
           config.server_port, config.max_open_sockets);

  if (httpd_start(&http_server, &config) == ESP_OK) {
    ESP_LOGI(TAG, "Registering URI handlers");
    init_websocket();
    httpd_register_uri_handler(http_server, &root);
    httpd_register_uri_handler(http_server, &get_status);
    httpd_register_uri_handler(http_server, &js_uri);
    httpd_register_uri_handler(http_server, &css_uri);
    httpd_register_uri_handler(http_server, &brand_config_js_uri);
    httpd_register_uri_handler(http_server, &i18n_js_uri);
    httpd_register_uri_handler(http_server, &zh_cn_json_uri);
    httpd_register_uri_handler(http_server, &en_us_json_uri);
    httpd_register_uri_handler(http_server, &get_devinfo);
    httpd_register_uri_handler(http_server, &get_sys);
    httpd_register_uri_handler(http_server, &get_module_set);
    httpd_register_uri_handler(http_server, &get_module_set_info);
    httpd_register_uri_handler(http_server, &get_net_set);
    httpd_register_uri_handler(http_server, &get_net_set_info);
    httpd_register_uri_handler(http_server, &get_mqtt);
    httpd_register_uri_handler(http_server, &get_mqtt_info);
    httpd_register_uri_handler(http_server, &get_tcp);
    httpd_register_uri_handler(http_server, &get_tcp_info);
    httpd_register_uri_handler(http_server, &log_ws);
    httpd_register_uri_handler(http_server, &ws_disconnect_uri);

    httpd_register_uri_handler(http_server, &get_http);
    httpd_register_uri_handler(http_server, &get_http_info);
    httpd_register_uri_handler(http_server, &get_operate);
    httpd_register_uri_handler(http_server, &get_restore);
    httpd_register_uri_handler(http_server, &get_update);
    httpd_register_uri_handler(http_server, &get_ota);
    httpd_register_uri_handler(http_server, &get_find_wifi);
    httpd_register_uri_handler(http_server, &get_exit_ap);
    httpd_register_uri_handler(http_server, &get_ap_set);
    httpd_register_uri_handler(http_server, &get_ap_set_info);

    httpd_register_uri_handler(http_server, &web_login);
    httpd_register_uri_handler(http_server, &get_serial_set);
    httpd_register_uri_handler(http_server, &get_serial_set_info);
    httpd_register_uri_handler(http_server, &get_serial_ctl);
    httpd_register_uri_handler(http_server, &uart_response_uri);
    httpd_register_uri_handler(http_server, &work_mode_set);
    httpd_register_uri_handler(http_server, &work_mode_info);
    httpd_register_uri_handler(http_server, &get_ota_progress);
    httpd_register_uri_handler(http_server, &get_log_switch);
    httpd_register_uri_handler(http_server, &set_log_switch);

    httpd_register_err_handler(http_server, HTTPD_404_NOT_FOUND,
                               http_404_error_handler);
  }
  return http_server;
}



void http_server_init(void) {
  // 初始化NVS互斥锁
  if (nvs_mutex == NULL) {
    nvs_mutex = xSemaphoreCreateMutex();
    if (nvs_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create NVS mutex");
    } else {
      ESP_LOGI(TAG, "NVS mutex initialized successfully");
    }
  }
  esp_log_level_set("httpd_sess", ESP_LOG_ERROR);
  // 首先初始化日志系统
  sx_log_init();
  // ESP_ERROR_CHECK(esp_netif_init());
  // ESP_ERROR_CHECK(esp_event_loop_create_default());
  // wifi_init_sta();
  wifi_init_softap();
  start_webserver();
  start_dns_server();
  // sx_log_init();
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
