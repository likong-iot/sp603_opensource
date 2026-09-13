/*
 * @Author: Orion
 * @Date: 2024-01-23 16:11:42
 * @LastEditors: Orion
 * @LastEditTime: 2025-03-06 22:43:35
 * @FilePath: \SERIIAL_SERVER\main\sx_http_client.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "math.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>

// 如果没有MIN宏定义，添加一个
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define MAX_HTTP_RECV_BUFFER 1024
#define MAX_HTTP_OUTPUT_BUFFER 2048
static const char *TAG = "HTTP_CLIENT";

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// 引用定时器模块中定义的HTTP互斥量
extern SemaphoreHandle_t http_mutex;

// 标记是否正在进行客户端HTTP请求
volatile bool client_http_busy = false;

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

esp_err_t _http_event_handler(esp_http_client_event_t *evt) {
  static char *output_buffer; // Buffer to store response of http request from
                              // event handler
  static int output_len;      // Stores number of bytes read
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
             evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    /*
     *  Check for chunked encoding is added as the URL for chunked encoding used
     * in this example returns binary data. However, event handler can also be
     * used in case chunked encoding is used.
     */
    if (!esp_http_client_is_chunked_response(evt->client)) {
      // If user_data buffer is configured, copy the response into the buffer
      int copy_len = 0;
      if (evt->user_data) {
        copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - output_len));
        if (copy_len) {
          memcpy(evt->user_data + output_len, evt->data, copy_len);
        }
      } else {
        const int buffer_len = esp_http_client_get_content_length(evt->client);
        if (output_buffer == NULL) {
          output_buffer = (char *)malloc(buffer_len);
          output_len = 0;
          if (output_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
            return ESP_FAIL;
          }
        }
        copy_len = MIN(evt->data_len, (buffer_len - output_len));
        if (copy_len) {
          memcpy(output_buffer + output_len, evt->data, copy_len);
        }
      }
      output_len += copy_len;
    }

    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
    if (output_buffer != NULL) {
      // Response is accumulated in output_buffer. Uncomment the below line to
      // print the accumulated response ESP_LOG_BUFFER_HEX(TAG, output_buffer,
      // output_len);
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    int mbedtls_err = 0;
    esp_err_t err = esp_tls_get_and_clear_last_error(
        (esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
    if (err != 0) {
      ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
      ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
    }
    if (output_buffer != NULL) {
      free(output_buffer);
      output_buffer = NULL;
    }
    output_len = 0;
    break;
  case HTTP_EVENT_REDIRECT:
    ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
    esp_http_client_set_header(evt->client, "From", "user@example.com");
    esp_http_client_set_header(evt->client, "Accept", "text/html");
    esp_http_client_set_redirection(evt->client);
    break;
  }
  return ESP_OK;
}
esp_err_t get_http_url(char *url, size_t url_len) {
  nvs_handle_t nvs_handle;
  esp_err_t ret = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS namespace");
    return ret;
  }

  ret = nvs_get_str(nvs_handle, "http_url", url, &url_len);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get HTTP URL from NVS");
  }

  nvs_close(nvs_handle);
  return ret;
}

void http_rest_with_url(char *url, char *header) {
  char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
  printf("url: %s\n", url);
  if (url == NULL) {
    printf("url is NULL\n");
    url = "http://47.101.140.34:8080/";
  }

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = _http_event_handler,
      .user_data =
          local_response_buffer, // Pass address of local buffer to get response
      .disable_auto_redirect = true,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "messageid", messageid);
  cJSON_AddNumberToObject(root, "code", 200);
  cJSON_AddStringToObject(root, "type", DEVICE_TYPE);

  uint64_t time_since_boot = esp_timer_get_time();
  uint64_t time_in_s = time_since_boot / 1000000;
  cJSON *data = cJSON_CreateObject();
  cJSON *net = cJSON_CreateObject();
  cJSON *sys = cJSON_CreateObject();
  cJSON *protocol = cJSON_CreateObject();

  cJSON_AddItemToObject(root, "data", data);
  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  size_t use_tcp, use_udp, use_http, tcpconn, udpconn, netconn,
      wifi_ssid, is_dhcp = 0;
  ret = nvs_get_str(nvs_handle, "netconn", NULL, &netconn);
  nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);

  nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp);
  nvs_get_str(nvs_handle, "use_udp", NULL, &use_udp);
  nvs_get_str(nvs_handle, "use_http", NULL, &use_http);
  nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn);
  nvs_get_str(nvs_handle, "udpconn", NULL, &udpconn);
  nvs_get_str(nvs_handle, "wifi_ssid", NULL, &wifi_ssid);
  switch (ret) {
  case ESP_OK:
    printf("protocol set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_tcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_udp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcpconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "udpconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK) {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  } else {

    char *nvs_use_tcp = malloc(use_tcp);
    char *nvs_use_udp = malloc(use_udp);
    char *nvs_use_http = malloc(use_http);
    char *nvs_tcpconn = malloc(tcpconn);
    char *nvs_udpconn = malloc(udpconn);
    char *nvs_netconn = malloc(netconn);
    char *nvs_wifi_ssid = malloc(wifi_ssid);
    char *nvs_is_dhcp = malloc(is_dhcp);
    if (nvs_netconn == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);

      nvs_get_str(nvs_handle, "use_tcp", nvs_use_tcp, &use_tcp);
      nvs_get_str(nvs_handle, "use_udp", nvs_use_udp, &use_udp);
      nvs_get_str(nvs_handle, "use_http", nvs_use_http, &use_http);
      nvs_get_str(nvs_handle, "tcpconn", nvs_tcpconn, &tcpconn);
      nvs_get_str(nvs_handle, "udpconn", nvs_udpconn, &udpconn);
      nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      if (ret == ESP_OK) {
        if (atoi(nvs_netconn) == 2) {
          cJSON_AddStringToObject(net, "connmethed", "wifi");
          cJSON_AddStringToObject(net, "ssid", nvs_wifi_ssid);
          if (atoi(nvs_is_dhcp) == 2) {
            cJSON_AddNumberToObject(net, "dhcp", 0);
          } else {
            cJSON_AddNumberToObject(net, "dhcp", 1);
          }
        } else {
          cJSON_AddStringToObject(net, "connmethed", "eth");
          cJSON_AddStringToObject(net, "ssid", "---");
          cJSON_AddNumberToObject(net, "dhcp", 1);
        }
        cJSON_AddStringToObject(net, "ip", got_ip_addrs);
        cJSON_AddItemToObject(root, "net", net);
        cJSON_AddStringToObject(sys, "version", VERSION);
        cJSON_AddNumberToObject(sys, "runtime", time_in_s);
    
        cJSON_AddStringToObject(sys, "sta_mac", device_sta_mac);
        cJSON_AddItemToObject(root, "sys", sys);

        cJSON_AddNumberToObject(protocol, "http", atoi(nvs_use_http));
        if (atoi(nvs_use_tcp) == 1) {
          if (atoi(nvs_tcpconn) == 0) {
            cJSON_AddNumberToObject(protocol, "tcpserver", 1);
            cJSON_AddNumberToObject(protocol, "tcpclient", 0);
          } else {
            cJSON_AddNumberToObject(protocol, "tcpserver", 0);
            cJSON_AddNumberToObject(protocol, "tcpclient", 1);
          }
        } else {
          cJSON_AddNumberToObject(protocol, "tcpserver", 0);
          cJSON_AddNumberToObject(protocol, "tcpclient", 0);
        }
        if (atoi(nvs_use_udp) == 1) {
          if (atoi(nvs_udpconn) == 0) {
            cJSON_AddNumberToObject(protocol, "udpserver", 1);
            cJSON_AddNumberToObject(protocol, "udpclient", 0);
          } else {
            cJSON_AddNumberToObject(protocol, "udpserver", 0);
            cJSON_AddNumberToObject(protocol, "udpclient", 1);
          }
        } else {
          cJSON_AddNumberToObject(protocol, "udpserver", 0);
          cJSON_AddNumberToObject(protocol, "udpclient", 0);
        }
        cJSON_AddItemToObject(root, "protocol", protocol);
      } else {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }

      free(nvs_use_tcp);
      free(nvs_use_udp);
      free(nvs_use_http);
      free(nvs_tcpconn);
      free(nvs_udpconn);
      free(nvs_netconn);
      free(nvs_wifi_ssid);
      free(nvs_is_dhcp);
    }
  }
  nvs_close(nvs_handle);

  char *jsonString = cJSON_Print(root);

  // char *post_data = "{\"field1\":\"value1\"}";
  //   esp_http_client_set_url(client, "http://47.101.140.34:8080");
  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, jsonString, strlen(jsonString));
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %" PRIu64,
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
  }
  free(jsonString);
  cJSON_Delete(root);
  esp_http_client_cleanup(client);
}

void http_test_task(void *pvParameters) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  char task_http_url[64] = {0};
  char task_http_header[64] = {0};
  int task_http_time = 1;
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  size_t use_http = 0;
  size_t http_port = 0;
  size_t httpconn = 0;
  size_t http_url = 0;
  size_t http_header = 0;
  size_t http_time = 0;
  ret = nvs_get_str(nvs_handle, "use_http", NULL, &use_http);
  esp_err_t port_size_ret =
      nvs_get_str(nvs_handle, "http_port", NULL, &http_port);
  esp_err_t conn_size_ret =
      nvs_get_str(nvs_handle, "httpconn", NULL, &httpconn);
  esp_err_t url_size_ret =
      nvs_get_str(nvs_handle, "http_url", NULL, &http_url);
  esp_err_t header_size_ret =
      nvs_get_str(nvs_handle, "http_header", NULL, &http_header);
  esp_err_t time_size_ret =
      nvs_get_str(nvs_handle, "http_time", NULL, &http_time);
  switch (ret) {
  case ESP_OK:
    printf("HTTP set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    printf("no HTTP set");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_port", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "httpconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_url", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_header", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_time", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK || port_size_ret != ESP_OK || conn_size_ret != ESP_OK ||
      url_size_ret != ESP_OK || header_size_ret != ESP_OK ||
      time_size_ret != ESP_OK) {
    ESP_LOGE(TAG,
             "Error getting HTTP config size: use=%s port=%s conn=%s url=%s "
             "header=%s time=%s",
             esp_err_to_name(ret), esp_err_to_name(port_size_ret),
             esp_err_to_name(conn_size_ret), esp_err_to_name(url_size_ret),
             esp_err_to_name(header_size_ret), esp_err_to_name(time_size_ret));
  } else {
    char *nvs_use_http = malloc(use_http);
    char *nvs_http_port = malloc(http_port);
    char *nvs_httpconn = malloc(httpconn);
    char *nvs_http_url = malloc(http_url);
    char *nvs_http_header = malloc(http_header);
    char *nvs_http_time = malloc(http_time);

    if (nvs_use_http == NULL || nvs_http_port == NULL ||
        nvs_httpconn == NULL || nvs_http_url == NULL ||
        nvs_http_header == NULL || nvs_http_time == NULL) {
      printf("Memory allocation failed\n");
    } else {
      ret = nvs_get_str(nvs_handle, "use_http", nvs_use_http, &use_http);
      esp_err_t port_ret =
          nvs_get_str(nvs_handle, "http_port", nvs_http_port, &http_port);
      esp_err_t conn_ret =
          nvs_get_str(nvs_handle, "httpconn", nvs_httpconn, &httpconn);
      esp_err_t url_ret =
          nvs_get_str(nvs_handle, "http_url", nvs_http_url, &http_url);
      esp_err_t header_ret = nvs_get_str(nvs_handle, "http_header",
                                         nvs_http_header, &http_header);
      esp_err_t time_ret =
          nvs_get_str(nvs_handle, "http_time", nvs_http_time, &http_time);

      if (ret == ESP_OK && port_ret == ESP_OK && conn_ret == ESP_OK &&
          url_ret == ESP_OK && header_ret == ESP_OK && time_ret == ESP_OK) {
        printf("Value for 'use_http' is %s\n", nvs_use_http);
        printf("Value for 'http_port' is %s\n", nvs_http_port);
        printf("Value for 'httpconn' is %s\n", nvs_httpconn);
        printf("Value for 'http_url' is %s\n", nvs_http_url);
        printf("Value for 'http_header' is %s\n", nvs_http_header);
        printf("Value for 'http_time' is %s\n", nvs_http_time);
        if (strlen(nvs_http_url) >= sizeof(task_http_url) ||
            strlen(nvs_http_header) >= sizeof(task_http_header)) {
          ESP_LOGE(TAG, "HTTP config too long, url_len=%zu header_len=%zu",
                   strlen(nvs_http_url), strlen(nvs_http_header));
        } else {
          snprintf(task_http_url, sizeof(task_http_url), "%s", nvs_http_url);
          snprintf(task_http_header, sizeof(task_http_header), "%s",
                   nvs_http_header);
          task_http_time = atoi(nvs_http_time);
        }

      } else {
        ESP_LOGE(TAG,
                 "Error reading HTTP config: use=%s port=%s conn=%s url=%s "
                 "header=%s time=%s",
                 esp_err_to_name(ret), esp_err_to_name(port_ret),
                 esp_err_to_name(conn_ret), esp_err_to_name(url_ret),
                 esp_err_to_name(header_ret), esp_err_to_name(time_ret));
      }
    }
    free(nvs_use_http);
    free(nvs_http_port);
    free(nvs_httpconn);
    free(nvs_http_url);
    free(nvs_http_header);
    free(nvs_http_time);
  }
  nvs_close(nvs_handle);
  while (1) {
    printf("task_http_time: %d\n", task_http_time);
    printf("task_http_url: %s\n", task_http_url);
    printf("task_http_header: %s\n", task_http_header);
    vTaskDelay(1000 * task_http_time / portTICK_PERIOD_MS);
    // vTaskDelay(100 / portTICK_PERIOD_MS);

    http_rest_with_url(task_http_url, task_http_header);
    // free(task_http_time);
    // free(task_http_url);
    // free(task_http_header);
  }
}
void http_report_data(uint8_t *data, int len, int template_index) {
  static const char *TAG = "HTTP_REPORT";
  ESP_LOGI(TAG, "Processing HTTP report for template %d", template_index);

  char http_url[128] = {0};
  if (get_http_url(http_url, sizeof(http_url)) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get HTTP URL");
    return;
  }

  // 创建 JSON 根对象
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    ESP_LOGE(TAG, "Failed to create JSON object");
    return;
  }

  // 添加基本字段
  cJSON_AddBoolToObject(root, "enabled", true);
  cJSON_AddNumberToObject(root, "command_index", template_index);

  // 从 storage 命名空间读取指定模板的配置
  nvs_handle_t nvs_handle_storage;
  if (nvs_open("storage", NVS_READWRITE, &nvs_handle_storage) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open storage namespace");
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
  if (!(data[1] & 0x80)) {  // 检查是否为错误响应
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
      int data_length = data[2];
      int reg_count = data_length / 2;

      // 存储寄存器值
      uint16_t registers[32];
      for (int j = 0; j < reg_count && j < 32; j++) {
        registers[j] = (data[data_start + j*2] << 8) | data[data_start + j*2 + 1];
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

          if (strcmp(data_format, "HEX") == 0) {
            char hex_value[16];
            snprintf(hex_value, sizeof(hex_value), "0x%04X", registers[j]);
            cJSON_AddStringToObject(data_obj, data_key, hex_value);
          } else {
            char *formatted_value = convert_multi_register_data(
                &registers[j], regs_per_value, data_format);
            cJSON_AddStringToObject(data_obj, data_key, formatted_value);
          }
        }
      }

      cJSON_AddItemToArray(response_array, data_obj);
      cJSON_AddItemToObject(root, "response_data", response_array);
    } else {
      if (response_array) cJSON_Delete(response_array);
      if (data_obj) cJSON_Delete(data_obj);
    }
  }

  // 转换为JSON字符串
  char *json_string = cJSON_Print(root);
  if (json_string) {
    ESP_LOGI(TAG, "Sending HTTP POST to URL: %s", http_url);
    ESP_LOGI(TAG, "Payload: %s", json_string);

    // 配置 HTTP 客户端
    esp_http_client_config_t config = {
      .url = http_url,
      .event_handler = _http_event_handler,
      .timeout_ms = 5000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client) {
      esp_http_client_set_method(client, HTTP_METHOD_POST);
      esp_http_client_set_header(client, "Content-Type", "application/json");
      esp_http_client_set_post_field(client, json_string, strlen(json_string));

      // 添加重试机制
      int retry_count = 0;
      esp_err_t err;
      do {
        err = esp_http_client_perform(client);
        if (err != ESP_OK) {
          ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
          vTaskDelay(pdMS_TO_TICKS(500));
          retry_count++;
        }
      } while (err != ESP_OK && retry_count < 3);

      if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP POST Status = %d", status_code);
      }

      esp_http_client_cleanup(client);
    }
    free(json_string);
  }

  nvs_close(nvs_handle_storage);
  cJSON_Delete(root);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
