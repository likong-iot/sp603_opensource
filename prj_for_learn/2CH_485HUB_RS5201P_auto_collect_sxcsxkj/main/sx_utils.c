/*
 * @Author: Orion
 * @Date: 2024-01-23 16:08:51
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-21 13:57:35
 * @FilePath: \SERIIAL_SERVER_P\main\sx_utils.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "cJSON.h"
#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "math.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_gpio.h"

#include "sx_auto_collect.h"

#include "sx_task.h"

#include "esp_mac.h"
#include "esp_attr.h"
#include "sx_async_uart.h"

#include "sx_timer_tasks.h"
#include "sx_web_server.h"
#include "esp_heap_caps.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 添加包含自己的头文件
#include "sx_utils.h"

// 函数前向声明
char *get_ip_addr(void);
char *get_gateway(void);
char *get_netmask(void);

char *get_wifi_mac(void);
char *get_version(void);
char *get_device_type(void);
char *get_device_name(void);
char *get_is_dhcp_value(void);
char *get_primary_dns(void);
char *get_secondary_dns(void);

TaskHandle_t xHandleTask1, xHandleTask2, xHandleTask3 = NULL;
EXT_RAM_BSS_ATTR char messageid[48];
// 添加全局变量，用于判断网络连接状态
bool is_network_connected = false;

// 全局变量声明
extern char got_ip_addrs[16];
extern char got_ip_netmask[16];
extern char got_ip_gw[16];
extern char device_mac[30];

extern char device_sta_mac[24];
char nvs_netconn[10] = "2"; // 默认为WiFi模式
extern char device_mac_end[20];
extern uint8_t mac_addr[6];

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

// 函数用于将十六进制字符转换为对应的十进制数
int hexCharToInt(char c)
{
  if (c >= '0' && c <= '9')
  {
    return c - '0';
  }
  if (c >= 'A' && c <= 'F')
  {
    return c - 'A' + 10;
  }
  if (c >= 'a' && c <= 'f')
  {
    return c - 'a' + 10;
  }
  return 0;
}

// URL 解码函数
void urlDecode(char *src, char *dest)
{
  char *pSrc = src;
  char *pDest = dest;

  while (*pSrc)
  {
    if (*pSrc == '%')
    {
      if (pSrc[1] && pSrc[2])
      {
        *pDest++ = hexCharToInt(pSrc[1]) * 16 + hexCharToInt(pSrc[2]);
        pSrc += 3;
      }
    }
    else if (*pSrc == '+')
    { // 处理空格（空格有时被编码为+）
      *pDest++ = ' ';
      pSrc++;
    }
    else
    {
      *pDest++ = *pSrc++;
    }
  }

  *pDest = '\0';
}

void save_form_data(const char *query)
{

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
  // 打开NVS命名空间
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  char *params = strdup(query); // 为了不修改原字符串
  char *param;

  // 按 & 分割字符串
  param = strtok(params, "&");
  while (param != NULL)
  {
    // 找到键和值的分隔符 '='
    char *delimiter = strchr(param, '=');
    if (delimiter != NULL)
    {
      // 分割键和值
      *delimiter = '\0';
      char *key = param;
      char *value = delimiter + 1;
      // 打印键和值
      printf("Key: %s, Value: %s\n", key, value);
      ESP_ERROR_CHECK(nvs_set_str(nvs_handle, key, value));
    }
    // 继续获取下一个参数
    param = strtok(NULL, "&");
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
  }

  nvs_close(nvs_handle);

  free(params); // 释放复制的字符串
}

char *reset_reason_to_string(esp_reset_reason_t reason)
{
  switch (reason)
  {
  case ESP_RST_UNKNOWN:
    return "Unknown";
  case ESP_RST_POWERON:
    return "Power-on reset";
  case ESP_RST_EXT:
    return "External reset";
  case ESP_RST_SW:
    return "Software reset";
  case ESP_RST_PANIC:
    return "Exception/panic reset";
  case ESP_RST_INT_WDT:
    return "Watchdog reset";
  case ESP_RST_TASK_WDT:
    return "Task watchdog reset";
  case ESP_RST_WDT:
    return "Other watchdog reset";
  case ESP_RST_DEEPSLEEP:
    return "Exited deep sleep";
  case ESP_RST_BROWNOUT:
    return "Brownout reset";
  case ESP_RST_SDIO:
    return "SDIO reset";
  default:
    return "Unknown";
  }
}

uint16_t ModbusCRC16(uint8_t *data, uint16_t length)
{
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < length; pos++)
  {
    crc ^= (uint16_t)data[pos]; // XOR byte into least sig. byte of crc
    for (int i = 8; i != 0; i--)
    { // Loop over each bit
      if ((crc & 0x0001) != 0)
      {            // If the LSB is set
        crc >>= 1; // Shift right and XOR 0xA001
        crc ^= 0xA001;
      }
      else         // Else LSB is not set
        crc >>= 1; // Just shift right
    }
  }
  return crc;
}

// 初始化 NVS
esp_err_t nvs_init()
{
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  return ret;
}
// 添加安全重置 NVS 的函数实现
esp_err_t nvs_safe_reset(void)
{
  const char *TAG = "nvs_safe_reset";
  esp_err_t err;

  // 检查NVS是否已初始化
  nvs_stats_t nvs_stats;
  err = nvs_get_stats(NULL, &nvs_stats);
  if (err == ESP_ERR_NVS_NOT_INITIALIZED)
  {
    ESP_LOGI(TAG, "NVS not initialized, initializing first");
    err = nvs_flash_init();
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
      if (err != ESP_ERR_NVS_NO_FREE_PAGES &&
          err != ESP_ERR_NVS_NEW_VERSION_FOUND)
      {
        return err;
      }

      // 如果是特定错误，尝试擦除后重新初始化
      ESP_LOGI(TAG, "Erasing NVS flash...");
      err = nvs_flash_erase();
      if (err != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to erase NVS: %s", esp_err_to_name(err));
        return err;
      }

      err = nvs_flash_init();
      if (err != ESP_OK)
      {
        ESP_LOGE(TAG, "Failed to initialize NVS after erase: %s",
                 esp_err_to_name(err));
        return err;
      }
    }
  }
  else if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Failed to get NVS stats: %s", esp_err_to_name(err));
    return err;
  }

  // 打开 NVS 存储
  nvs_handle_t nvs_handle;
  err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(err));
    return err;
  }

  // 读取需要保留的值
  char device_sn[32] = {0};
  char device_type[32] = {0};
  size_t sn_len = sizeof(device_sn);
  size_t type_len = sizeof(device_type);

  err = nvs_get_str(nvs_handle, "device_sn", device_sn, &sn_len);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "No device_sn found in NVS");
  }

  err = nvs_get_str(nvs_handle, "device_type", device_type, &type_len);
  if (err != ESP_OK)
  {
    ESP_LOGW(TAG, "No device_type found in NVS");
  }

  // 关闭 NVS 句柄
  nvs_close(nvs_handle);

  // 擦除 NVS
  err = nvs_flash_erase();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error erasing NVS: %s", esp_err_to_name(err));
    return err;
  }

  // 重新初始化 NVS
  err = nvs_flash_init();
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error initializing NVS: %s", esp_err_to_name(err));
    return err;
  }

  // 如果之前有保存的值，则恢复它们
  if (device_sn[0] != '\0' || device_type[0] != '\0')
  {
    err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error opening NVS handle after reset: %s",
               esp_err_to_name(err));
      return err;
    }

    if (device_sn[0] != '\0')
    {
      err = nvs_set_str(nvs_handle, "device_sn", device_sn);
      if (err != ESP_OK)
      {
        ESP_LOGE(TAG, "Error restoring device_sn: %s", esp_err_to_name(err));
      }
    }

    if (device_type[0] != '\0')
    {
      err = nvs_set_str(nvs_handle, "device_type", device_type);
      if (err != ESP_OK)
      {
        ESP_LOGE(TAG, "Error restoring device_type: %s", esp_err_to_name(err));
      }
    }

    err = nvs_commit(nvs_handle);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    }

    nvs_close(nvs_handle);
  }

  ESP_LOGI(TAG, "NVS reset completed successfully");
  return ESP_OK;
}
esp_err_t save_to_nvs(const char *json_string)
{
  nvs_handle_t nvs_handle;
  esp_err_t err;
  // 打开 NVS
  err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
  {
    printf("Error (%s) opening NVS handle!\n", esp_err_to_name(err));
    return err;
  }
  else
  {
    printf("Opened NVS handle\n");
  }

  // 解析 JSON
  cJSON *json = cJSON_Parse(json_string);
  if (json == NULL)
  {
    const char *error_ptr = cJSON_GetErrorPtr();
    if (error_ptr != NULL)
    {
      fprintf(stderr, "Error before: %s\n", error_ptr);
    }
    nvs_close(nvs_handle);
    return ESP_ERR_INVALID_ARG;
  }

  // 检查是否为串口配置数据
  cJSON *serial_port = cJSON_GetObjectItem(json, "serial_port");
  if (cJSON_IsNumber(serial_port) && serial_port->valueint >= 1 && serial_port->valueint <= 3)
  {
    // 这是串口配置数据，需要转换为新的键名格式
    int port = serial_port->valueint;
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "ch%d", port);

    printf("Processing serial config for port %d with prefix %s\n", port, prefix);

    // 遍历 JSON 对象并转换键名
    cJSON *current_element = NULL;
    cJSON_ArrayForEach(current_element, json)
    {
      if (cJSON_IsString(current_element) && current_element->valuestring != NULL)
      {
        // 跳过控制字段，因为它们不是持久化配置参数
        if (strcmp(current_element->string, "serial_port") == 0 ||
            strcmp(current_element->string, "defer_apply") == 0)
        {
          continue;
        }

        // 构建新的键名
        char new_key[32];
        if (strcmp(current_element->string, "reply_timeout") == 0)
        {
          // 对于reply_timeout，使用简化的键名
          snprintf(new_key, sizeof(new_key), "%s_timeout", prefix);
        }
        else
        {
          snprintf(new_key, sizeof(new_key), "%s_%s", prefix, current_element->string);
        }

        // 保存字符串
        err = nvs_set_str(nvs_handle, new_key, current_element->valuestring);
        if (err != ESP_OK)
        {
          printf("Failed to write %s to NVS\n", new_key);
        }
        else
        {
          printf("Saved %s = %s\n", new_key, current_element->valuestring);
        }
      }
      else if (cJSON_IsNumber(current_element))
      {
        // 跳过控制字段
        if (strcmp(current_element->string, "serial_port") == 0 ||
            strcmp(current_element->string, "defer_apply") == 0)
        {
          continue;
        }

        // 构建新的键名
        char new_key[32];
        if (strcmp(current_element->string, "reply_timeout") == 0)
        {
          // 对于reply_timeout，使用简化的键名
          snprintf(new_key, sizeof(new_key), "%s_timeout", prefix);
        }
        else
        {
          snprintf(new_key, sizeof(new_key), "%s_%s", prefix, current_element->string);
        }

        // 保存整数
        err = nvs_set_i32(nvs_handle, new_key, current_element->valueint);
        if (err != ESP_OK)
        {
          printf("Failed to write %s to NVS\n", new_key);
        }
        else
        {
          printf("Saved %s = %ld\n", new_key, (long)current_element->valueint);
        }
      }
    }
  }
  else
  {
    // 这是其他配置数据，使用原来的处理方式
    printf("Processing general config data\n");

    // 遍历 JSON 对象
    cJSON *current_element = NULL;
    cJSON_ArrayForEach(current_element, json)
    {
      if (cJSON_IsString(current_element) &&
          (current_element->valuestring != NULL))
      {
        // 保存字符串
        err = nvs_set_str(nvs_handle, current_element->string,
                          current_element->valuestring);
        if (err != ESP_OK)
        {
          printf("Failed to write %s to NVS\n", current_element->string);
        }
      }
      else if (cJSON_IsNumber(current_element))
      {
        // 保存整数
        err = nvs_set_i32(nvs_handle, current_element->string,
                          current_element->valueint);
        if (err != ESP_OK)
        {
          printf("Failed to write %s to NVS\n", current_element->string);
        }
      }
    }
  }

  // 提交写入操作
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK)
  {
    printf("Failed to commit changes!\n");
  }

  // 清理
  cJSON_Delete(json);

  nvs_close(nvs_handle);
  return err;
}

char *send_device_info(uint8_t *u_data, int idx)
{
  // 创建根对象
  cJSON *root = cJSON_CreateObject();
  // 添加基础键值对
  cJSON_AddStringToObject(root, "messageid", messageid);
  cJSON_AddNumberToObject(root, "code", 200);
  cJSON_AddStringToObject(root, "type", DEVICE_TYPE);

  uint64_t time_since_boot = esp_timer_get_time();
  uint64_t time_in_s = time_since_boot / 1000000;
  // 添加 "data" 对象
  cJSON *data = cJSON_CreateObject();
  cJSON *net = cJSON_CreateObject();
  cJSON *sys = cJSON_CreateObject();
  cJSON *protocol = cJSON_CreateObject();
  cJSON *serial = cJSON_CreateObject();

  // char *response = tx_task(u_data, idx);
  // char *response = "";
  tx_tasks_to_channel(u_data, idx, 3);
  // cJSON_AddStringToObject(root, "resopnse", device_response);

  cJSON_AddItemToObject(root, "data", data);
  // free(response);

  esp_err_t ret = nvs_init();
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));
  size_t use_tcp, use_http, tcpconn, netconn, wifi_ssid, is_dhcp,
      baud_rate, data_bit, check_bit, stop_bit = 0;
  ret = nvs_get_str(nvs_handle, "netconn", NULL, &netconn);
  nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp);

  nvs_get_str(nvs_handle, "use_tcp", NULL, &use_tcp);

  nvs_get_str(nvs_handle, "use_http", NULL, &use_http);
  nvs_get_str(nvs_handle, "tcpconn", NULL, &tcpconn);

  nvs_get_str(nvs_handle, "wifi_ssid", NULL, &wifi_ssid);
  nvs_get_str(nvs_handle, "baud_rate", NULL, &baud_rate);
  nvs_get_str(nvs_handle, "data_bit", NULL, &data_bit);
  nvs_get_str(nvs_handle, "check_bit", NULL, &check_bit);
  nvs_get_str(nvs_handle, "stop_bit", NULL, &stop_bit);
  switch (ret)
  {
  case ESP_OK:
    printf("protocol set success");
    break;
  case ESP_ERR_NVS_NOT_FOUND:
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_mqtt", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_tcp", ""));

    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcpconn", ""));

    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "baud_rate", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "data_bit", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "check_bit", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "stop_bit", ""));
    break;
  default:
    break;
  }
  if (ret != ESP_OK)
  {
    printf("Error getting size of 'my_key': %s\n", esp_err_to_name(ret));
  }
  else
  {

    char *nvs_use_tcp = malloc(use_tcp);

    char *nvs_use_http = malloc(use_http);
    char *nvs_tcpconn = malloc(tcpconn);

    char *nvs_netconn = malloc(netconn);
    char *nvs_wifi_ssid = malloc(wifi_ssid);
    char *nvs_is_dhcp = malloc(is_dhcp);
    char *nvs_baud_rate = malloc(baud_rate);
    char *nvs_data_bit = malloc(data_bit);
    char *nvs_check_bit = malloc(check_bit);
    char *nvs_stop_bit = malloc(stop_bit);

    if (nvs_netconn == NULL)
    {
      printf("Memory allocation failed\n");
    }
    else
    {
      ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);

      nvs_get_str(nvs_handle, "use_tcp", nvs_use_tcp, &use_tcp);

      nvs_get_str(nvs_handle, "use_http", nvs_use_http, &use_http);
      nvs_get_str(nvs_handle, "tcpconn", nvs_tcpconn, &tcpconn);

      nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
      nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
      nvs_get_str(nvs_handle, "baud_rate", nvs_baud_rate, &baud_rate);
      nvs_get_str(nvs_handle, "data_bit", nvs_data_bit, &data_bit);
      nvs_get_str(nvs_handle, "check_bit", nvs_check_bit, &check_bit);
      nvs_get_str(nvs_handle, "stop_bit", nvs_stop_bit, &stop_bit);

      if (ret == ESP_OK)
      {
        // 添加 "net" 对象
        cJSON_AddNumberToObject(serial, "baud_rate", atoi(nvs_baud_rate));
        cJSON_AddNumberToObject(serial, "data_bit", atoi(nvs_data_bit));

        cJSON_AddNumberToObject(serial, "stop_bit", atof(nvs_stop_bit));
        cJSON_AddStringToObject(serial, "check_bit", nvs_check_bit);
        cJSON_AddItemToObject(root, "serial", serial);

        if (atoi(nvs_netconn) == 2)
        {
          cJSON_AddStringToObject(net, "connmethed", "wifi");
          cJSON_AddStringToObject(net, "ssid", nvs_wifi_ssid);
          if (atoi(nvs_is_dhcp) == 2)
          {
            cJSON_AddNumberToObject(net, "dhcp", 0);
          }
          else
          {
            cJSON_AddNumberToObject(net, "dhcp", 1);
          }
        }
        else
        {

          cJSON_AddStringToObject(net, "ssid", "---");
          cJSON_AddNumberToObject(net, "dhcp", 1);
        }
        cJSON_AddStringToObject(net, "ip", got_ip_addrs);
        cJSON_AddStringToObject(net, "netmask", got_ip_netmask);
        cJSON_AddStringToObject(net, "mask", got_ip_netmask);
        cJSON_AddStringToObject(net, "gateway", got_ip_gw);
        cJSON_AddItemToObject(root, "net", net);

        // 添加 "sys" 对象

        cJSON_AddStringToObject(sys, "version", VERSION);

        cJSON_AddNumberToObject(sys, "runtime", time_in_s);

        cJSON_AddStringToObject(sys, "sta_mac", device_sta_mac);
        cJSON_AddItemToObject(root, "sys", sys);

        // 添加 "protocol" 对象

        cJSON_AddNumberToObject(protocol, "http", atoi(nvs_use_http));
        if (atoi(nvs_use_tcp) == 1)
        {
          if (atoi(nvs_tcpconn) == 0)
          {
            cJSON_AddNumberToObject(protocol, "tcpserver", 1);
            cJSON_AddNumberToObject(protocol, "tcpclient", 0);
          }
          else
          {
            cJSON_AddNumberToObject(protocol, "tcpserver", 0);
            cJSON_AddNumberToObject(protocol, "tcpclient", 1);
          }
        }
        else
        {
          cJSON_AddNumberToObject(protocol, "tcpserver", 0);
          cJSON_AddNumberToObject(protocol, "tcpclient", 0);
        }

        cJSON_AddItemToObject(root, "protocol", protocol);
      }
      else
      {
        printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
      }

      free(nvs_use_tcp);

      free(nvs_use_http);
      free(nvs_tcpconn);

      free(nvs_netconn);
      free(nvs_wifi_ssid);
      free(nvs_is_dhcp);
      free(nvs_baud_rate);
      free(nvs_data_bit);
      free(nvs_check_bit);
      free(nvs_stop_bit);
    }
  }
  nvs_close(nvs_handle);
  char *jsonString = cJSON_Print(root);
  char *result = strdup(jsonString);
  free(jsonString);
  cJSON_Delete(root);
  return result;
}

char *heartbeat_config_json(void)
{
  cJSON *root = cJSON_CreateObject();
  if (root == NULL)
  {
    printf("Failed to create JSON root object\n");
    return NULL;
  }

  uint8_t sta_mac[6] = {0};
  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);

  char d_sta_mac[24];
  sprintf(d_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0], sta_mac[1],
          sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

  cJSON_AddStringToObject(root, "mac_ap", d_sta_mac);
  char *jsonString = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (jsonString == NULL)
  {
    printf("Failed to serialize JSON\n");
    return NULL;
  }

  return jsonString;
}

char *send_device_ip(void)
{
  cJSON *root = cJSON_CreateObject();
  if (!root)
    return NULL;

  // 获取各种设备信息
  char *version = get_version();
  char *type = get_device_type();
  char *device_name = get_device_name();

  char *sta_mac = get_wifi_mac();
  char *ip = get_ip_addr();
  char *gateway = get_gateway();
  char *netmask = get_netmask();
  char *is_dhcp = get_is_dhcp_value();
  char *primary_dns = get_primary_dns();
  char *secondary_dns = get_secondary_dns();

  // 添加设备信息到JSON对象
  cJSON_AddStringToObject(root, "version", version ? version : "");
  cJSON_AddStringToObject(root, "type", type ? type : "");
  cJSON_AddStringToObject(root, "name", device_name ? device_name : "");

  cJSON_AddStringToObject(root, "sta_mac", sta_mac ? sta_mac : "");
  cJSON_AddStringToObject(root, "ip", ip ? ip : "");
  cJSON_AddStringToObject(root, "gateway", gateway ? gateway : "");
  cJSON_AddStringToObject(root, "netmask", netmask ? netmask : "");
  cJSON_AddStringToObject(root, "is_dhcp", is_dhcp ? is_dhcp : "1"); // 默认为动态IP
  cJSON_AddStringToObject(root, "primary_dns", primary_dns ? primary_dns : "8.8.8.8");
  cJSON_AddStringToObject(root, "secondary_dns", secondary_dns ? secondary_dns : "114.114.114.114");

  // 释放临时分配的内存
  if (version)
    free(version);
  if (type)
    free(type);
  if (device_name)
    free(device_name);

  if (sta_mac)
    free(sta_mac);
  if (ip)
    free(ip);
  if (gateway)
    free(gateway);
  if (netmask)
    free(netmask);
  if (is_dhcp)
    free(is_dhcp);
  if (primary_dns)
    free(primary_dns);
  if (secondary_dns)
    free(secondary_dns);

  // 生成JSON字符串并返回
  char *resp = cJSON_Print(root);
  cJSON_Delete(root);
  return resp;
}

char *send_device_info_err(void)
{
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "messageid", messageid);
  cJSON_AddNumberToObject(root, "code", 204);
  cJSON_AddStringToObject(root, "msg", "type not found");
  char *jsonString = cJSON_Print(root);
  cJSON_Delete(root);
  return jsonString;
}

// 函数用于重置并清除所有NVS存储
esp_err_t nvs_reset_clear()
{
  const char *TAG = "nvs_reset_clear";
  esp_err_t err;
  err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_LOGI(TAG, "Erasing NVS flash");
    err = nvs_flash_erase();
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "NVS flash erase failed: %s", esp_err_to_name(err));
      return err;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "NVS flash init failed: %s", esp_err_to_name(err));
    return err;
  }
  ESP_LOGI(TAG, "NVS flash reset and cleared");
  return ESP_OK;
}

static const char *RESET_TAG = "factory_reset";

static void delete_task_by_name_safe(const char *task_name)
{
  if (task_name == NULL || task_name[0] == '\0')
  {
    return;
  }

  char truncated_name[16] = {0};
  strncpy(truncated_name, task_name, sizeof(truncated_name) - 1);
  truncated_name[sizeof(truncated_name) - 1] = '\0';

  TaskHandle_t task_handle = xTaskGetHandle(truncated_name);
  if (task_handle == NULL)
  {
    ESP_LOGD(RESET_TAG, "任务不存在或已停止: %s", truncated_name);
    return;
  }

  if (task_handle == xTaskGetCurrentTaskHandle())
  {
    ESP_LOGD(RESET_TAG, "跳过删除当前任务: %s", truncated_name);
    return;
  }

  ESP_LOGI(RESET_TAG, "删除任务: %s", truncated_name);
  delete_app_task_with_caps(task_handle);
}

static esp_err_t stop_runtime_tasks_for_reset(void)
{
  ESP_LOGI(RESET_TAG,
           "关闭工作模式任务，清理资源");

  stop_all_uart_tasks();

  esp_err_t first_err = ESP_OK;

  esp_err_t err = sx_auto_collect_stop();
  if (err != ESP_OK && first_err == ESP_OK)
  {
    first_err = err;
  }
  delete_task_by_name_safe("ac_poll_ch2");
  delete_task_by_name_safe("ac_poll_ch3");
  delete_task_by_name_safe("ac_resp_ch1");
  delete_task_by_name_safe("key_task");
  deinit_all_uart_drivers_for_ota();

  vTaskDelay(pdMS_TO_TICKS(200));

  size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  ESP_LOGI(RESET_TAG,
           "堆内存: 总可用(非连续)=%u 字节, 最大连续可用块=%u 字节",
           (unsigned)free_8bit, (unsigned)largest_8bit);

  return first_err;
}

esp_err_t perform_factory_reset(const char *work_mode_to_restore)
{
  ESP_LOGI(RESET_TAG, "开始执行恢复出厂设置");

  esp_err_t err = stop_runtime_tasks_for_reset();
  if (err != ESP_OK)
  {
    ESP_LOGW(RESET_TAG, "恢复出厂前停止运行任务异常: %s", esp_err_to_name(err));
  }

  // 1. 擦除整个 NVS 分区，清除所有持久化数据
  ESP_LOGI(RESET_TAG, "擦除NVS");
  err = nvs_flash_erase();
  if (err != ESP_OK)
  {
    ESP_LOGE(RESET_TAG, "擦除 NVS 失败: %s", esp_err_to_name(err));
    return err;
  }

  // 2. 执行 LED 闪烁提示序列，指示复位操作完成
  ESP_LOGI(RESET_TAG, "执行 LED 闪烁序列以指示完成");
  for (int cycle = 0; cycle < 3; ++cycle)
  {
    // 点亮所有通道的 TX LED
    uart_tx_led_on(1);              // CH1
    uart_tx_led_on(2);              // CH2
    uart_tx_led_on(3);              // CH3
    vTaskDelay(pdMS_TO_TICKS(500)); // 保持亮 半 秒
    // 熄灭所有通道的 TX LED
    uart_tx_led_off(1);
    uart_tx_led_off(2);
    uart_tx_led_off(3);
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  return ESP_OK;
}

// 处理多寄存器数据的转换函数
char *convert_multi_register_data(uint16_t *registers, int reg_count,
                                  const char *format)
{
  static char result[32];
  union
  {
    uint32_t long_val;
    float float_val;
    double double_val;
    uint16_t uint16_array[4]; // 最多支持4个寄存器(64位)
  } data_converter;
  memset(&data_converter, 0, sizeof(data_converter));

  bool is_inverse = (strstr(format, "Inverse") != NULL);

  if (strstr(format, "Long") != NULL)
  {
    // Long类型需要2个寄存器
    if (reg_count >= 2)
    {
      if (is_inverse)
      {
        data_converter.uint16_array[0] = registers[1];
        data_converter.uint16_array[1] = registers[0];
      }
      else
      {
        data_converter.uint16_array[0] = registers[0];
        data_converter.uint16_array[1] = registers[1];
      }
      snprintf(result, sizeof(result), "%ld", (long)data_converter.long_val);
    }
  }
  else if (strstr(format, "Float") != NULL)
  {
    // Float类型需要2个寄存器
    if (reg_count >= 2)
    {
      if (is_inverse)
      {
        data_converter.uint16_array[0] = registers[1];
        data_converter.uint16_array[1] = registers[0];
      }
      else
      {
        data_converter.uint16_array[0] = registers[0];
        data_converter.uint16_array[1] = registers[1];
      }
      snprintf(result, sizeof(result), "%.6f", data_converter.float_val);
    }
  }
  else if (strstr(format, "Double") != NULL)
  {
    // Double类型需要4个寄存器
    if (reg_count >= 4)
    {
      if (is_inverse)
      {
        for (int i = 0; i < 4; i++)
        {
          data_converter.uint16_array[i] = registers[3 - i];
        }
      }
      else
      {
        for (int i = 0; i < 4; i++)
        {
          data_converter.uint16_array[i] = registers[i];
        }
      }
      snprintf(result, sizeof(result), "%.12lf", data_converter.double_val);
    }
  }
  else if (strcmp(format, "Signed") == 0)
  {
    snprintf(result, sizeof(result), "%d", (int16_t)registers[0]);
  }
  else if (strcmp(format, "Unsigned") == 0)
  {
    snprintf(result, sizeof(result), "%u", registers[0]);
  }
  else if (strcmp(format, "Binary") == 0)
  {
    char binary[17] = {0};
    for (int i = 15; i >= 0; i--)
    {
      binary[15 - i] = ((registers[0] >> i) & 1) ? '1' : '0';
    }
    snprintf(result, sizeof(result), "%s", binary);
  }
  else
  { // HEX 或其他格式
    snprintf(result, sizeof(result), "0x%04X", registers[0]);
  }

  return result;
}

void protocol_select()
{

  is_network_connected = true;

  // simple_task();
  // xHandleTask3led();
}

void simple_task()
{
  // 启动一些简单的任务
  init_timer_tasks();
  create_app_task_psram(key_task, "key_task",
                        6144,
                        NULL, 1, NULL, SX_AUX_CORE_ID);
}

// 验证用户名和密码
bool verify_credentials(const char *username, const char *password)
{
  const char *TAG = "verify_credentials";
  nvs_handle_t nvs_handle;
  esp_err_t err;
  bool result = false;

  // 打开NVS
  err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
    return false;
  }

  // 获取存储的用户名和密码
  size_t lgname_size = 0;
  size_t lgpwd_size = 0;

  // 获取存储的用户名大小
  err = nvs_get_str(nvs_handle, "lgname", NULL, &lgname_size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error getting username size: %s", esp_err_to_name(err));
    // 如果没有找到lgname，尝试使用默认值
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
      ESP_LOGI(TAG, "lgname not found, using default credentials");
      if (strcmp(username, "public") == 0 &&
          strcmp(password, "Aa123456") == 0)
      {
        ESP_LOGI(TAG, "Default credentials verified successfully");
        nvs_close(nvs_handle);
        return true;
      }
    }
    nvs_close(nvs_handle);
    return false;
  }

  // 获取存储的密码大小
  err = nvs_get_str(nvs_handle, "lgpwd", NULL, &lgpwd_size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error getting password size: %s", esp_err_to_name(err));
    nvs_close(nvs_handle);
    return false;
  }

  // 分配内存
  char *stored_username = malloc(lgname_size);
  char *stored_password = malloc(lgpwd_size);

  if (!stored_username || !stored_password)
  {
    ESP_LOGE(TAG, "Memory allocation failed");
    if (stored_username)
      free(stored_username);
    if (stored_password)
      free(stored_password);
    nvs_close(nvs_handle);
    return false;
  }

  // 获取存储的用户名
  err = nvs_get_str(nvs_handle, "lgname", stored_username, &lgname_size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error getting username: %s", esp_err_to_name(err));
    free(stored_username);
    free(stored_password);
    nvs_close(nvs_handle);
    return false;
  }

  // 获取存储的密码
  err = nvs_get_str(nvs_handle, "lgpwd", stored_password, &lgpwd_size);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error getting password: %s", esp_err_to_name(err));
    free(stored_username);
    free(stored_password);
    nvs_close(nvs_handle);
    return false;
  }

  // 验证用户名和密码
  if (strcmp(username, stored_username) == 0 &&
      strcmp(password, stored_password) == 0)
  {
    ESP_LOGI(TAG, "Credentials verified successfully");
    result = true;
  }
  else
  {
    ESP_LOGE(TAG, "Invalid credentials");
    result = false;
  }

  // 释放资源
  free(stored_username);
  free(stored_password);
  nvs_close(nvs_handle);

  return result;
}

// 更新设备设置
bool update_device_settings(cJSON *parm)
{
  const char *TAG = "update_device_settings";
  nvs_handle_t nvs_handle;
  esp_err_t err;
  bool result = true;

  // 声明外部函数
  extern void reset_device_name_cache(void);

  // 打印完整的JSON参数，用于调试
  char *json_str = cJSON_Print(parm);
  if (json_str)
  {
    ESP_LOGI(TAG, "Incoming parameters: %s", json_str);

    // 为了调试，检查mask和netmask字段是否存在
    cJSON *mask_check = cJSON_GetObjectItem(parm, "mask");
    cJSON *netmask_check = cJSON_GetObjectItem(parm, "netmask");

    ESP_LOGI(TAG, "Field check - mask exists: %d, netmask exists: %d",
             (mask_check != NULL), (netmask_check != NULL));

    if (mask_check)
    {
      ESP_LOGI(TAG, "mask value: %s",
               cJSON_IsString(mask_check) ? mask_check->valuestring
                                          : "NOT_STRING");
    }

    if (netmask_check)
    {
      ESP_LOGI(TAG, "netmask value: %s",
               cJSON_IsString(netmask_check) ? netmask_check->valuestring
                                             : "NOT_STRING");
    }

    free(json_str);
  }
  else
  {
    ESP_LOGI(TAG, "Failed to print incoming parameters");
  }

  // 打开NVS
  err = nvs_open("storage", NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
    return false;
  }

  // 检查并设置网络模式（DHCP或静态IP）
  cJSON *dhcp_mode_obj = cJSON_GetObjectItem(parm, "is_dhcp");
  if (cJSON_IsString(dhcp_mode_obj) && dhcp_mode_obj->valuestring != NULL)
  {
    const char *dhcp_mode = dhcp_mode_obj->valuestring;
    ESP_LOGI(TAG, "Setting network mode (is_dhcp = %s) - %s", dhcp_mode,
             strcmp(dhcp_mode, "1") == 0 ? "DHCP Mode" : "Static IP Mode");

    err = nvs_set_str(nvs_handle, "is_dhcp", dhcp_mode);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error setting is_dhcp: %s", esp_err_to_name(err));
      result = false;
    }
  }
  else
  {
    // 默认设置为静态IP模式
    ESP_LOGI(TAG, "No is_dhcp parameter found, defaulting to static IP mode "
                  "(is_dhcp = 2)");
    err = nvs_set_str(nvs_handle, "is_dhcp", "2");
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error setting default is_dhcp: %s", esp_err_to_name(err));
      result = false;
    }
  }

  // 检查并更新设备名称 - 使用host_names字段
  cJSON *name = cJSON_GetObjectItem(parm, "name");
  if (cJSON_IsString(name) && name->valuestring != NULL)
  {
    ESP_LOGI(TAG, "Updating device name to: %s", name->valuestring);
    err = nvs_set_str(nvs_handle, "host_names", name->valuestring);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error setting host_names: %s", esp_err_to_name(err));
      result = false;
    }
    else
    {
      // 重置设备名称缓存
      reset_device_name_cache();
    }
  }

  // 检查并更新IP地址 - 使用static_ip字段
  cJSON *ip = cJSON_GetObjectItem(parm, "ip");
  if (cJSON_IsString(ip) && ip->valuestring != NULL)
  {
    ESP_LOGI(TAG, "Updating IP address to: %s", ip->valuestring);
    err = nvs_set_str(nvs_handle, "static_ip", ip->valuestring);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error setting static_ip: %s", esp_err_to_name(err));
      result = false;
    }
  }

  // 检查并更新子网掩码 - 使用static_netmask字段
  cJSON *mask = cJSON_GetObjectItem(parm, "mask");
  if (cJSON_IsString(mask) && mask->valuestring != NULL)
  {
    ESP_LOGI(TAG, "Updating subnet mask to: %s", mask->valuestring);
    err = nvs_set_str(nvs_handle, "static_netmask", mask->valuestring);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error setting static_netmask: %s", esp_err_to_name(err));
      result = false;
    }
  }
  else
  {
    // 兼容检查客户端可能使用的"netmask"字段
    cJSON *netmask = cJSON_GetObjectItem(parm, "netmask");
    if (cJSON_IsString(netmask) && netmask->valuestring != NULL)
    {
      ESP_LOGI(TAG, "Updating subnet mask to: %s (from netmask param)",
               netmask->valuestring);
      err = nvs_set_str(nvs_handle, "static_netmask", netmask->valuestring);
      if (err != ESP_OK)
      {
        ESP_LOGE(TAG, "Error setting static_netmask: %s", esp_err_to_name(err));
        result = false;
      }
    }
  }

  // 检查并更新网关 - 使用static_gateway字段
  cJSON *gateway = cJSON_GetObjectItem(parm, "gateway");
  if (cJSON_IsString(gateway) && gateway->valuestring != NULL)
  {
    ESP_LOGI(TAG, "Updating gateway to: %s", gateway->valuestring);
    err = nvs_set_str(nvs_handle, "static_gateway", gateway->valuestring);
    if (err != ESP_OK)
    {
      ESP_LOGE(TAG, "Error setting static_gateway: %s", esp_err_to_name(err));
      result = false;
    }
  }

  // 打印出所有最终的网络设置
  char ip_str[32] = {0};
  char netmask_str[32] = {0};
  char gateway_str[32] = {0};
  char hostname[64] = {0};
  char nvs_is_dhcp[8] = {0}; // 重命名以避免冲突
  size_t len;

  len = sizeof(nvs_is_dhcp);
  nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &len);

  len = sizeof(ip_str);
  nvs_get_str(nvs_handle, "static_ip", ip_str, &len);

  len = sizeof(netmask_str);
  nvs_get_str(nvs_handle, "static_netmask", netmask_str, &len);

  len = sizeof(gateway_str);
  nvs_get_str(nvs_handle, "static_gateway", gateway_str, &len);

  len = sizeof(hostname);
  nvs_get_str(nvs_handle, "host_names", hostname, &len);

  // 更新日志信息，区分DHCP和静态IP模式
  if (strcmp(nvs_is_dhcp, "1") == 0)
  {
    ESP_LOGI(TAG,
             "Final network settings - DHCP Mode: Enabled (is_dhcp=%s), "
             "Hostname: %s",
             nvs_is_dhcp, hostname);
  }
  else
  {
    ESP_LOGI(TAG,
             "Final network settings - Static IP Mode (is_dhcp=%s), Hostname: "
             "%s, IP: %s, Netmask: %s, Gateway: %s",
             nvs_is_dhcp, hostname, ip_str, netmask_str, gateway_str);
  }

  // 提交更改
  err = nvs_commit(nvs_handle);
  if (err != ESP_OK)
  {
    ESP_LOGE(TAG, "Error committing NVS changes: %s", esp_err_to_name(err));
    result = false;
  }

  // 关闭NVS
  nvs_close(nvs_handle);

  return result;
}

// 获取当前IP地址
char *get_ip_addr(void)
{
  return strdup(got_ip_addrs);
}

// 获取当前网关地址
char *get_gateway(void)
{
  return strdup(got_ip_gw);
}

// 获取当前子网掩码
char *get_netmask(void)
{
  return strdup(got_ip_netmask);
}

// 获取以太网MAC地址

// 获取WiFi MAC地址
char *get_wifi_mac(void)
{
  return strdup(device_sta_mac);
}

// 获取版本号
char *get_version(void)
{
  return strdup(VERSION);
}

// 获取设备类型
char *get_device_type(void)
{
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t device_type_len = 0;
  esp_err_t ret = nvs_get_str(nvs_handle, "device_type", NULL, &device_type_len);
  if (ret != ESP_OK || device_type_len == 0)
  {
    // 如果没有找到设备类型或长度为0，设置默认值
    nvs_close(nvs_handle);
    return strdup("RS5201");
  }

  char *device_type = malloc(device_type_len);
  if (device_type == NULL)
  {
    nvs_close(nvs_handle);
    return strdup("RS5201");
  }

  ret = nvs_get_str(nvs_handle, "device_type", device_type, &device_type_len);
  if (ret != ESP_OK)
  {
    free(device_type);
    nvs_close(nvs_handle);
    return strdup("RS5201");
  }

  nvs_close(nvs_handle);
  return device_type;
}

// 获取设备名称
char *get_device_name(void)
{
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t host_names_len = 0;
  esp_err_t ret = nvs_get_str(nvs_handle, "host_names", NULL, &host_names_len);
  if (ret != ESP_OK || host_names_len == 0)
  {
    // 如果没有找到设备名称或长度为0，设置默认值
    nvs_close(nvs_handle);
    return strdup("Unknown");
  }

  char *host_names = malloc(host_names_len);
  if (host_names == NULL)
  {
    nvs_close(nvs_handle);
    return strdup("Unknown");
  }

  ret = nvs_get_str(nvs_handle, "host_names", host_names, &host_names_len);
  if (ret != ESP_OK)
  {
    free(host_names);
    nvs_close(nvs_handle);
    return strdup("Unknown");
  }

  nvs_close(nvs_handle);
  return host_names;
}

// 获取网络类型（是否DHCP）
char *get_is_dhcp_value(void)
{
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t is_dhcp_len = 0;
  esp_err_t ret = nvs_get_str(nvs_handle, "is_dhcp", NULL, &is_dhcp_len);
  if (ret != ESP_OK || is_dhcp_len == 0)
  {
    // 如果没有找到DHCP设置或长度为0，设置默认值
    nvs_close(nvs_handle);
    return strdup("1"); // 默认为动态IP
  }

  char *is_dhcp = malloc(is_dhcp_len);
  if (is_dhcp == NULL)
  {
    nvs_close(nvs_handle);
    return strdup("1");
  }

  ret = nvs_get_str(nvs_handle, "is_dhcp", is_dhcp, &is_dhcp_len);
  if (ret != ESP_OK)
  {
    free(is_dhcp);
    nvs_close(nvs_handle);
    return strdup("1");
  }

  nvs_close(nvs_handle);
  return is_dhcp;
}

// 获取主DNS
char *get_primary_dns(void)
{
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t primary_dns_len = 0;
  esp_err_t ret = nvs_get_str(nvs_handle, "primary_dns", NULL, &primary_dns_len);
  if (ret != ESP_OK || primary_dns_len == 0)
  {
    // 如果没有找到主DNS或长度为0，设置默认值
    nvs_close(nvs_handle);
    return strdup("8.8.8.8");
  }

  char *primary_dns = malloc(primary_dns_len);
  if (primary_dns == NULL)
  {
    nvs_close(nvs_handle);
    return strdup("8.8.8.8");
  }

  ret = nvs_get_str(nvs_handle, "primary_dns", primary_dns, &primary_dns_len);
  if (ret != ESP_OK)
  {
    free(primary_dns);
    nvs_close(nvs_handle);
    return strdup("8.8.8.8");
  }

  nvs_close(nvs_handle);
  return primary_dns;
}

// 获取备用DNS
char *get_secondary_dns(void)
{
  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t secondary_dns_len = 0;
  esp_err_t ret = nvs_get_str(nvs_handle, "secondary_dns", NULL, &secondary_dns_len);
  if (ret != ESP_OK || secondary_dns_len == 0)
  {
    // 如果没有找到备用DNS或长度为0，设置默认值
    nvs_close(nvs_handle);
    return strdup("114.114.114.114");
  }

  char *secondary_dns = malloc(secondary_dns_len);
  if (secondary_dns == NULL)
  {
    nvs_close(nvs_handle);
    return strdup("114.114.114.114");
  }

  ret = nvs_get_str(nvs_handle, "secondary_dns", secondary_dns, &secondary_dns_len);
  if (ret != ESP_OK)
  {
    free(secondary_dns);
    nvs_close(nvs_handle);
    return strdup("114.114.114.114");
  }

  nvs_close(nvs_handle);
  return secondary_dns;
}

// 应用任务统一走 PSRAM 栈分配，系统任务保持默认堆
BaseType_t create_app_task_psram(TaskFunction_t task_fn, const char *name,
                                 uint32_t stack_size_bytes, void *params,
                                 UBaseType_t priority, TaskHandle_t *handle,
                                 BaseType_t core_id)
{
  const UBaseType_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  return xTaskCreatePinnedToCoreWithCaps(task_fn, name, stack_size_bytes,
                                         params, priority, handle, core_id,
                                         caps);
}

// 释放使用 WithCaps 创建的任务
void delete_app_task_with_caps(TaskHandle_t handle)
{
  vTaskDeleteWithCaps(handle);
}

// 允许任务自身退出并回收 PSRAM 栈
void delete_self_app_task_with_caps(void)
{
  vTaskDeleteWithCaps(NULL);
}

// 打印系统资源使用情况（内部RAM、PSRAM、任务CPU/栈）
void print_system_resource_usage(void)
{
  const char *TAG_RES = "SYS_RES";

  // 内存信息
  size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
  size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  size_t internal_used = internal_total - internal_free;

  size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  size_t psram_used = psram_total - psram_free;

  float internal_usage =
      (internal_total == 0) ? 0 : (100.0f * internal_used / internal_total);
  float psram_usage =
      (psram_total == 0) ? 0 : (100.0f * psram_used / psram_total);

  ESP_LOGI(TAG_RES, "------------------ Memory Usage (KB) -----------------");
  ESP_LOGI(TAG_RES, "| Region    |   Used |   Free |  Total | Usage |");
  ESP_LOGI(TAG_RES, "| Internal  | %6u | %6u | %6u | %5.1f%% |",
           (unsigned)(internal_used / 1024), (unsigned)(internal_free / 1024),
           (unsigned)(internal_total / 1024), internal_usage);
  ESP_LOGI(TAG_RES, "| PSRAM     | %6u | %6u | %6u | %5.1f%% |",
           (unsigned)(psram_used / 1024), (unsigned)(psram_free / 1024),
           (unsigned)(psram_total / 1024), psram_usage);

#if (configUSE_TRACE_FACILITY == 1)
  UBaseType_t task_count = uxTaskGetNumberOfTasks();
  TaskStatus_t *task_array =
      calloc(task_count, sizeof(TaskStatus_t)); // NOLINT
  if (task_array == NULL)
  {
    ESP_LOGE(TAG_RES, "无法分配任务状态缓存，跳过任务资源打印");
    return;
  }

  uint32_t total_runtime = 0;
  task_count = uxTaskGetSystemState(task_array, task_count, &total_runtime);
  if (task_count == 0)
  {
    ESP_LOGW(TAG_RES, "未获取到任务状态信息");
    free(task_array);
    return;
  }

  ESP_LOGI(TAG_RES, "------------------- Task Usage -----------------------");
#if (configGENERATE_RUN_TIME_STATS == 1)
  ESP_LOGI(TAG_RES, "| Task                 | CPU%% | StackFree(B) |");
#else
  ESP_LOGI(TAG_RES, "| Task                 | StackFree(B) |");
#endif

  for (UBaseType_t i = 0; i < task_count; i++)
  {
    const TaskStatus_t *ts = &task_array[i];
    size_t stack_free_bytes = ts->usStackHighWaterMark * sizeof(StackType_t);

#if (configGENERATE_RUN_TIME_STATS == 1)
    float cpu_percent =
        (total_runtime == 0)
            ? 0.0f
            : (100.0f * ts->ulRunTimeCounter / (float)total_runtime);
    ESP_LOGI(TAG_RES, "| %-20s | %4.1f | %12u |", ts->pcTaskName, cpu_percent,
             (unsigned)stack_free_bytes);
#else
    ESP_LOGI(TAG_RES, "| %-20s | %12u |", ts->pcTaskName,
             (unsigned)stack_free_bytes);
#endif
  }

#if (configGENERATE_RUN_TIME_STATS == 0)
  ESP_LOGW(TAG_RES, "configGENERATE_RUN_TIME_STATS 未开启，CPU占用率为0");
#endif

  free(task_array);
#else
  ESP_LOGW(TAG_RES, "configUSE_TRACE_FACILITY 未开启，无法获取任务运行统计");
#endif
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
