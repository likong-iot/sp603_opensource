/*
 * @Author: Orion
 * @Date: 2024-01-23 16:11:42
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-27 12:44:19
 * @FilePath: \ETH_TH\main\sx_http_client.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
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
// 添加mbedtls加密库
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

// 如果没有MIN宏定义，添加一个
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif

#define MAX_HTTP_RECV_BUFFER 1024
#define MAX_HTTP_OUTPUT_BUFFER 2048
static const char *TAG = "HTTP_PLATFORM";

// 全局变量定义（从sx_time.c移动过来）
char strftime_buf[64] = "--:--:--"; // 初始化为占位符值

// 加密相关定义
#define AES_BLOCK_SIZE 16
#define AES_KEY_SIZE 32
#define SALT "likong" // 加密盐值

// 生成密钥函数
void generate_key(const char *salt, uint8_t *key, size_t key_len) {
  // 使用PBKDF2算法从盐值生成密钥
  mbedtls_md_context_t sha_ctx;
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  mbedtls_md_init(&sha_ctx);
  mbedtls_md_setup(&sha_ctx, info, 1);
  mbedtls_md_hmac_starts(&sha_ctx, (const unsigned char *)salt, strlen(salt));
  mbedtls_md_hmac_update(&sha_ctx, (const unsigned char *)salt, strlen(salt));
  mbedtls_md_hmac_finish(&sha_ctx, key);
  mbedtls_md_free(&sha_ctx);
}

// 加密函数
char *encrypt_data(const char *plaintext) {
  if (plaintext == NULL) {
    return NULL;
  }

  // 生成密钥
  uint8_t key[AES_KEY_SIZE];
  generate_key(SALT, key, AES_KEY_SIZE);

  // 初始化IV (初始化向量)
  uint8_t iv[AES_BLOCK_SIZE];
  for (int i = 0; i < AES_BLOCK_SIZE; i++) {
    iv[i] = (uint8_t)i;
  }

  // 计算填充后的大小
  size_t plaintext_len = strlen(plaintext);
  size_t padded_len =
      ((plaintext_len + AES_BLOCK_SIZE - 1) / AES_BLOCK_SIZE) * AES_BLOCK_SIZE;

  // 准备加密缓冲区
  uint8_t *padded_text = calloc(padded_len, sizeof(uint8_t));
  if (padded_text == NULL) {
    ESP_LOGE(TAG, "Memory allocation failed for padded text");
    return NULL;
  }

  // 复制明文并填充
  memcpy(padded_text, plaintext, plaintext_len);
  uint8_t padding_len = padded_len - plaintext_len;
  for (size_t i = plaintext_len; i < padded_len; i++) {
    padded_text[i] = padding_len;
  }

  // 准备密文缓冲区
  uint8_t *ciphertext = calloc(padded_len, sizeof(uint8_t));
  if (ciphertext == NULL) {
    ESP_LOGE(TAG, "Memory allocation failed for ciphertext");
    free(padded_text);
    return NULL;
  }

  // 进行AES-CBC加密
  mbedtls_aes_context aes_ctx;
  mbedtls_aes_init(&aes_ctx);
  mbedtls_aes_setkey_enc(&aes_ctx, key, AES_KEY_SIZE * 8);

  uint8_t iv_tmp[AES_BLOCK_SIZE];
  memcpy(iv_tmp, iv, AES_BLOCK_SIZE);

  mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_ENCRYPT, padded_len, iv_tmp,
                        padded_text, ciphertext);
  mbedtls_aes_free(&aes_ctx);

  // Base64编码
  size_t b64_len;
  mbedtls_base64_encode(NULL, 0, &b64_len, ciphertext, padded_len);

  char *b64_ciphertext = calloc(b64_len + 1, sizeof(char));
  if (b64_ciphertext == NULL) {
    ESP_LOGE(TAG, "Memory allocation failed for base64 ciphertext");
    free(padded_text);
    free(ciphertext);
    return NULL;
  }

  mbedtls_base64_encode((unsigned char *)b64_ciphertext, b64_len, &b64_len,
                        ciphertext, padded_len);
  b64_ciphertext[b64_len] = '\0'; // 确保字符串以NULL结尾

  // 释放临时缓冲区
  free(padded_text);
  free(ciphertext);

  return b64_ciphertext;
}

// 解密函数 (服务器端使用)
char *decrypt_data(const char *ciphertext_b64) {
  if (ciphertext_b64 == NULL) {
    return NULL;
  }

  // 生成密钥
  uint8_t key[AES_KEY_SIZE];
  generate_key(SALT, key, AES_KEY_SIZE);

  // 初始化IV (初始化向量)
  uint8_t iv[AES_BLOCK_SIZE];
  for (int i = 0; i < AES_BLOCK_SIZE; i++) {
    iv[i] = (uint8_t)i;
  }

  // Base64解码
  size_t ciphertext_len;
  mbedtls_base64_decode(NULL, 0, &ciphertext_len,
                        (const unsigned char *)ciphertext_b64,
                        strlen(ciphertext_b64));

  uint8_t *ciphertext = calloc(ciphertext_len, sizeof(uint8_t));
  if (ciphertext == NULL) {
    ESP_LOGE(TAG, "Memory allocation failed for decoded ciphertext");
    return NULL;
  }

  mbedtls_base64_decode(ciphertext, ciphertext_len, &ciphertext_len,
                        (const unsigned char *)ciphertext_b64,
                        strlen(ciphertext_b64));

  // 准备明文缓冲区
  uint8_t *plaintext = calloc(ciphertext_len, sizeof(uint8_t));
  if (plaintext == NULL) {
    ESP_LOGE(TAG, "Memory allocation failed for plaintext");
    free(ciphertext);
    return NULL;
  }

  // 进行AES-CBC解密
  mbedtls_aes_context aes_ctx;
  mbedtls_aes_init(&aes_ctx);
  mbedtls_aes_setkey_dec(&aes_ctx, key, AES_KEY_SIZE * 8);

  uint8_t iv_tmp[AES_BLOCK_SIZE];
  memcpy(iv_tmp, iv, AES_BLOCK_SIZE);

  mbedtls_aes_crypt_cbc(&aes_ctx, MBEDTLS_AES_DECRYPT, ciphertext_len, iv_tmp,
                        ciphertext, plaintext);
  mbedtls_aes_free(&aes_ctx);

  // 处理填充
  uint8_t padding_len = plaintext[ciphertext_len - 1];
  if (padding_len > 0 && padding_len <= AES_BLOCK_SIZE) {
    plaintext[ciphertext_len - padding_len] = '\0';
  } else {
    plaintext[ciphertext_len] = '\0';
  }

  // 转换为字符串
  char *result = strdup((char *)plaintext);

  // 释放临时缓冲区
  free(ciphertext);
  free(plaintext);

  return result;
}

// 创建加密的JSON包
char *create_encrypted_json(const char *original_json) {
  if (original_json == NULL) {
    return NULL;
  }

  // 加密原始JSON
  char *encrypted_data = encrypt_data(original_json);
  if (encrypted_data == NULL) {
    return NULL;
  }

  // 创建新的JSON对象，包含加密数据
  cJSON *root = cJSON_CreateObject();
  if (root == NULL) {
    free(encrypted_data);
    return NULL;
  }

  cJSON_AddStringToObject(root, "encrypted_data", encrypted_data);

  // 转换为字符串
  char *result = cJSON_Print(root);

  // 清理
  cJSON_Delete(root);
  free(encrypted_data);

  return result;
}

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// 辅助结构体，用于跟踪每个HTTP请求的状态
typedef struct {
  char *buffer;
  int len;
} http_buffer_ctx_t;

esp_err_t _http_event_handler_platform(esp_http_client_event_t *evt) {
  // 不再使用静态变量，完全依赖user_data
  http_buffer_ctx_t *ctx = NULL;
  esp_http_client_get_user_data(evt->client, (void **)&ctx);

  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
    if (ctx) {
      ctx->len = 0;  // 重置长度
    }
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
    if (!esp_http_client_is_chunked_response(evt->client)) {
      if (ctx && ctx->buffer) {
        int copy_len = MIN(evt->data_len, (MAX_HTTP_OUTPUT_BUFFER - ctx->len - 1));
        if (copy_len > 0) {
          memcpy(ctx->buffer + ctx->len, evt->data, copy_len);
          ctx->len += copy_len;
          ctx->buffer[ctx->len] = '\0';  // 确保字符串结束
        }
      }
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
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

void http_rest_with_url_platform(char *url, char *header) {
  static int send_count = 0;  // 静态变量放在函数内部

  // 分配缓冲区和上下文
  char *local_response_buffer = (char *)malloc(MAX_HTTP_OUTPUT_BUFFER);
  if (!local_response_buffer) {
    ESP_LOGE(TAG, "Failed to allocate response buffer");
    return;
  }
  memset(local_response_buffer, 0, MAX_HTTP_OUTPUT_BUFFER);

  http_buffer_ctx_t ctx = {
    .buffer = local_response_buffer,
    .len = 0
  };

  if (url == NULL) {
    ESP_LOGE(TAG, "URL is NULL, using default URL");
    url = "https://platform.likong-iot.com/api/v1/device_link"; // 默认URL
  }

  // 检查可用内存
  size_t free_heap = xPortGetFreeHeapSize();
  size_t min_free_heap = xPortGetMinimumEverFreeHeapSize();
  ESP_LOGI(TAG, "平台连接前内存: 当前可用=%zu字节, 最小可用=%zu字节", free_heap, min_free_heap);

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = _http_event_handler_platform,
      .user_data = &ctx,  // 传递上下文结构体
      .disable_auto_redirect = true,
      .skip_cert_common_name_check = true,
      .cert_pem = NULL,
      .buffer_size = 1024, // 减小缓冲区以降低内存占用
      .buffer_size_tx = 1024, // 减小发送缓冲区以降低内存占用
      .crt_bundle_attach = NULL,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .use_global_ca_store = false,
      .is_async = false,
      .timeout_ms = 3000, // 3秒超时，避免长时间占用资源
      .keep_alive_enable = false, // 禁用keep-alive，确保连接及时关闭
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    free(local_response_buffer);
    return;
  }

  char *datamsg = build_mqtt_config_json();

  if (datamsg == NULL) {
    ESP_LOGE(TAG, "Failed to build MQTT config JSON");
    esp_http_client_cleanup(client);
    free(local_response_buffer);
    return;
  }

  // 对JSON数据进行加密
  char *encrypted_json = create_encrypted_json(datamsg);
  if (encrypted_json == NULL) {
    ESP_LOGE(TAG, "Failed to encrypt JSON data");
    free(datamsg);
    esp_http_client_cleanup(client);
    free(local_response_buffer);
    return;
  }

  // 减少日志输出，只在必要时打印
  ESP_LOGD(TAG, "原始数据: %s", datamsg);
  ESP_LOGD(TAG, "加密后数据: %s", encrypted_json);

  free(datamsg); // 释放原始数据内存

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, encrypted_json,
                                 strlen(encrypted_json));

  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %" PRIu64,
             esp_http_client_get_status_code(client),
             esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
  }

  // 清理所有分配的资源
  free(local_response_buffer);
  free(encrypted_json);
  esp_http_client_cleanup(client);

  // 等待500ms确保TCP连接完全关闭，释放socket资源
  // ESP-IDF 5.5需要更长时间来处理TIME_WAIT状态
  vTaskDelay(pdMS_TO_TICKS(500));

  send_count++;  // 增加发送计数

  // 检查平台连接后的内存状态
  free_heap = xPortGetFreeHeapSize();
  min_free_heap = xPortGetMinimumEverFreeHeapSize();
  ESP_LOGI(TAG, "平台连接后内存: 当前可用=%zu字节, 最小可用=%zu字节", free_heap, min_free_heap);
  ESP_LOGI(TAG, "Request sent count: %d", send_count);

  return; // 不在这里删除任务，也不等待
}

int http_rest_with_url_heartbeat(char *url, char *header) {
  // 分配缓冲区和上下文
  char *local_response_buffer = (char *)malloc(MAX_HTTP_OUTPUT_BUFFER);
  if (!local_response_buffer) {
    ESP_LOGE(TAG, "Failed to allocate response buffer");
    return -1; // 返回错误状态码
  }
  memset(local_response_buffer, 0, MAX_HTTP_OUTPUT_BUFFER);

  http_buffer_ctx_t ctx = {
    .buffer = local_response_buffer,
    .len = 0
  };

  if (url == NULL) {
    ESP_LOGE(TAG, "URL is NULL, using default URL");
    url = "https://platform.likong-iot.com/api/v1/device_link/heartbeat"; // 默认URL
  }

  esp_http_client_config_t config = {
      .url = url,
      .event_handler = _http_event_handler_platform,
      .user_data = &ctx,  // 传递上下文结构体
      .disable_auto_redirect = true,
      .skip_cert_common_name_check = true,
      .cert_pem = NULL,
      .buffer_size = 1024, // 减小缓冲区以降低内存占用
      .buffer_size_tx = 1024, // 减小发送缓冲区以降低内存占用
      .crt_bundle_attach = NULL,
      .transport_type = HTTP_TRANSPORT_OVER_SSL,
      .use_global_ca_store = false,
      .is_async = false,
      .timeout_ms = 3000, // 3秒超时，避免长时间占用资源
      .keep_alive_enable = false, // 禁用keep-alive，确保连接及时关闭
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize HTTP client");
    free(local_response_buffer);
    return -1; // 返回错误状态码
  }

  char *datamsg = heartbeat_config_json();

  if (datamsg == NULL) {
    ESP_LOGE(TAG, "Failed to build MQTT config JSON");
    esp_http_client_cleanup(client);
    free(local_response_buffer);
    return -1; // 返回错误状态码
  }

  // 对JSON数据进行加密
  char *encrypted_json = create_encrypted_json(datamsg);
  if (encrypted_json == NULL) {
    ESP_LOGE(TAG, "Failed to encrypt JSON data");
    free(datamsg);
    esp_http_client_cleanup(client);
    free(local_response_buffer);
    return -1; // 返回错误状态码
  }

  // 减少日志输出，只在必要时打印
  ESP_LOGD(TAG, "原始数据: %s", datamsg);
  ESP_LOGD(TAG, "加密后数据: %s", encrypted_json);

  free(datamsg); // 释放原始数据内存

  esp_http_client_set_method(client, HTTP_METHOD_POST);
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_post_field(client, encrypted_json,
                                 strlen(encrypted_json));

  int status_code = -1; // 默认错误状态码
  esp_err_t err = esp_http_client_perform(client);
  if (err == ESP_OK) {
    status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP POST Status = %d, content_length = %" PRIu64,
             status_code, esp_http_client_get_content_length(client));
  } else {
    ESP_LOGE(TAG, "HTTP POST request failed: %s", esp_err_to_name(err));
  }

  // 清理所有分配的资源
  free(local_response_buffer);
  free(encrypted_json);
  esp_http_client_cleanup(client);

  // 等待500ms确保TCP连接完全关闭，释放socket资源
  // ESP-IDF 5.5需要更长时间来处理TIME_WAIT状态
  vTaskDelay(pdMS_TO_TICKS(500));

  return status_code; // 返回HTTP状态码
}

// 初始化时间为占位符的函数（不再从NVS加载时间）
void init_load_time_from_nvs(void) {
  // 直接设置为占位符，不再从NVS加载时间
  strcpy(strftime_buf, "--:--:--");
  ESP_LOGI(TAG, "时间已设置为占位符: %s", strftime_buf);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
