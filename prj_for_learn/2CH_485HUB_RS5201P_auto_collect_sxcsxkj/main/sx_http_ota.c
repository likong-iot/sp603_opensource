/*
 * @Author: Orion
 * @Date: 2024-01-11 11:07:33
 * @LastEditors: Orion
 * @LastEditTime: 2024-05-07 11:28:29
 * @FilePath: \SERIIAL_SERVER\main\sx_http_ota.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "esp_https_ota.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sx_async_uart.h"
#include "sx_auto_collect.h"
#include "sx_timer_tasks.h"
#include "sx_utils.h"
#include <string.h>
#include <stdio.h>

#define HASH_LEN 32
#define OTA_URL_SIZE 256
extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");
static const char *TAG = "OTA";


/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

static int ota_progress_percent = 0;
static char *ota_status = NULL;

static void *ota_psram_calloc(size_t count, size_t size) {
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static bool ensure_ota_status_buffer(void) {
    if (ota_status == NULL) {
        ota_status = ota_psram_calloc(1, 128);
    }
    return ota_status != NULL;
}

// 通过任务名称删除任务（FreeRTOS任务名最多15字符）
static void delete_task_by_name(const char *task_name) {
    char truncated_name[16] = {0};
    strncpy(truncated_name, task_name, 15);
    truncated_name[15] = '\0';
    
    TaskHandle_t task_handle = xTaskGetHandle(truncated_name);
    if (task_handle != NULL) {
        ESP_LOGI(TAG, "删除任务: %s", truncated_name);
        delete_app_task_with_caps(task_handle);
    } else {
        ESP_LOGD(TAG, "任务不存在或已停止: %s", truncated_name);
    }
}

// 准备OTA升级，停止串口任务和工作任务
static void prepare_for_ota(void) {
    ESP_LOGI(TAG, "======================================================================");
    ESP_LOGI(TAG, "开始准备OTA升级，停止串口任务和所有工作模式任务");
    ESP_LOGI(TAG, "======================================================================");
    
    // 停止所有UART接收任务
    ESP_LOGI(TAG, ">>> 停止所有UART接收任务");
    suspend_all_uart_rx_tasks();
    
    // 停止所有工作模式任务
    ESP_LOGI(TAG, ">>> 停止所有工作模式任务");
    
    // 停止自动采集模式
    ESP_LOGI(TAG, "停止自动采集模式");
    sx_auto_collect_stop();

    // 清理自动采集任务
    delete_task_by_name("ac_poll_ch2");
    delete_task_by_name("ac_poll_ch3");
    delete_task_by_name("ac_resp_ch1");

    // 低优先级杂项任务（释放小块堆）
    delete_task_by_name("key_task");

    // OTA前卸载UART驱动以释放环形缓冲/DMA等内存
    deinit_all_uart_drivers_for_ota();

    // 短暂等待任务停止与驱动卸载生效
    vTaskDelay(pdMS_TO_TICKS(300));

    // 打印堆诊断（关注连续最大块）
    size_t free_8bit = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "堆内存: 总可用(非连续)=%u 字节, 最大连续可用块=%u 字节", (unsigned)free_8bit, (unsigned)largest_8bit);
}

void ota_progress_callback(size_t downloaded, size_t total) {
    if (!ensure_ota_status_buffer()) {
        return;
    }
    ota_progress_percent = (downloaded * 100) / total;
    snprintf(ota_status, 128, "已下载: %d%%", ota_progress_percent);
}

int get_ota_progress(void) {
    return ota_progress_percent;
}

const char* get_ota_status(void) {
    if (!ensure_ota_status_buffer()) {
        return "准备更新";
    }
    if (ota_status[0] == '\0') {
        return "准备更新";
    }
    return ota_status;
}

// 检查URL是否为HTTPS
static bool is_https_url(const char *url) {
    return (strncmp(url, "https://", 8) == 0);
}


esp_err_t _http_event_handler_ota(esp_http_client_event_t *evt) {
  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_HEADER_SENT");
    break;
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key,
             evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_DISCONNECTED");
    break;
  case HTTP_EVENT_REDIRECT:
    ESP_LOGD(TAG, "OTA_HTTP_EVENT_REDIRECT");
    break;
  }
  return ESP_OK;
}

// HTTP OTA任务 (使用esp_https_ota处理HTTP)
static void http_ota_task(char *updateUrl) {
    ESP_LOGI(TAG, "======================================================================");
    ESP_LOGI(TAG, "开始HTTP OTA升级: %s", updateUrl);
    ESP_LOGI(TAG, "======================================================================");
    
    // 准备OTA升级，停止串口任务和工作任务
    prepare_for_ota();
    
    esp_http_client_config_t config = {
        .url = updateUrl,
        .timeout_ms = 120000,     // 增加到120秒，适应大文件下载
        .keep_alive_enable = true,
        .keep_alive_idle = 180,   // TCP keep-alive空闲时间3分钟
        .keep_alive_interval = 60, // TCP keep-alive间隔60秒
        .keep_alive_count = 3,    // TCP keep-alive重试次数
        .buffer_size = 12288,     // 初始12KB，速度优先
        .buffer_size_tx = 2048,   // 降低到2KB，为接收缓冲腾出空间
        .user_agent = "ESP32-485HUB-OTA/2.0",
        .event_handler = _http_event_handler_ota,
        .transport_type = HTTP_TRANSPORT_OVER_TCP,  // 明确指定HTTP传输
        .disable_auto_redirect = false,  // 允许自动重定向
        .max_redirection_count = 5,      // 最大重定向次数
        // HTTP不需要证书相关配置
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .http_client_init_cb = NULL,
        .bulk_flash_erase = true,
        .partial_http_download = true,      // 启用分段下载
        .max_http_request_size = 24576,     // 初始24KB，速度优先
        .buffer_caps = 0,  // Use default capability
        .ota_resumption = false,
        .ota_image_bytes_written = 0,
    };

    ESP_LOGI(TAG, "======================================================================");
    ESP_LOGI(TAG, "尝试从 %s 下载更新 (HTTP)", config.url);
    
    // 使用手动OTA流程以获取进度
    esp_https_ota_handle_t https_ota_handle = NULL;
    esp_err_t ret = ESP_FAIL;
    int retry_count = 3;
    
    for (int i = 0; i < retry_count; i++) {
        if (i > 0) {
            ESP_LOGI(TAG, "重试连接... (%d/%d)", i + 1, retry_count);
            ensure_ota_status_buffer();
            snprintf(ota_status, 128, "连接失败，重试中...(%d/%d)", i + 1, retry_count);
            vTaskDelay(pdMS_TO_TICKS(5000)); // 等待5秒再重试
        }
        
        // 重置进度
        ota_progress_percent = 0;
    ensure_ota_status_buffer();
    snprintf(ota_status, 128, "开始连接服务器...");
        
        // 更细粒度自适应：按1KB粒度搜索最佳组合（请求块优先大，RX优先小）
        size_t largest_8bit = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
        const int safety = 1024;            // 1KB裕量
        const int min_req = 16384;          // 16KB最低请求块
        const int max_req = 32768;          // 32KB最高请求块
        const int min_rx  = 8192;           // 8KB最低接收缓冲
        const int max_rx  = 14336;          // 14KB最高接收缓冲

        // 以 largest 为基准，计算起始请求块，按1KB对齐
        int req_start = (int)largest_8bit - safety;
        if (req_start > max_req) req_start = max_req;
        if (req_start < min_req) req_start = min_req;
        req_start = (req_start / 1024) * 1024; // 1KB对齐

        ret = ESP_FAIL;
        for (int req = req_start; req >= min_req && ret != ESP_OK; req -= 1024) {
            // RX从小到大尝试，优先让升级缓冲拿到更大的连续块
            for (int rx = min_rx; rx <= max_rx && ret != ESP_OK; rx += 1024) {
                config.buffer_size = rx;
                config.buffer_size_tx = 2048; // TX维持2KB
                ota_config.max_http_request_size = req;
                ESP_LOGI(TAG, "尝试启动OTA: rx=%d, tx=%d, req=%d (largest=%u)", rx, 2048, req, (unsigned)largest_8bit);
                ret = esp_https_ota_begin(&ota_config, &https_ota_handle);
                if (ret == ESP_ERR_NO_MEM) {
                    continue; // 内存不足则继续降级组合
                }
            }
        }
        if (ret != ESP_OK) {
            if (ret == ESP_ERR_HTTP_CONNECT) {
                ensure_ota_status_buffer();
                snprintf(ota_status, 128, "连接失败，准备重试...");
                continue;
            } else if (ret == ESP_ERR_NO_MEM) {
                ESP_LOGE(TAG, "OTA初始化失败：内存不足");
            }
            ESP_LOGE(TAG, "OTA初始化失败，不重试: %s", esp_err_to_name(ret));
            break;
        }
        
        ESP_LOGI(TAG, "连接成功，开始下载...");
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "连接成功，开始下载...");
        
        // 执行OTA，逐步更新进度
        int last_percent = 0;
        while (1) {
            ret = esp_https_ota_perform(https_ota_handle);
            if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
                break;
            }
            
            // 更新进度（最小化CPU开销）
            int image_size = esp_https_ota_get_image_size(https_ota_handle);
            int image_done = esp_https_ota_get_image_len_read(https_ota_handle);
            if (image_size > 0) {
                ota_progress_percent = (image_done * 100) / image_size;
                // 只在进度变化超过10%时才输出日志和更新状态
                if (ota_progress_percent - last_percent >= 10) {
                    ensure_ota_status_buffer();
                    snprintf(ota_status, 128, "下载中... %d%%", ota_progress_percent);
                    ESP_LOGI(TAG, "OTA进度: %d%%", ota_progress_percent);
                    last_percent = ota_progress_percent;
                }
            }
            
            // 移除延时，最大化下载速度
        }
        
        if (ret == ESP_OK) {
            // 完成OTA
            ESP_LOGI(TAG, "下载完成，正在验证...");
            ensure_ota_status_buffer();
            snprintf(ota_status, 128, "下载完成，正在验证...");
            ota_progress_percent = 95;
            
            ret = esp_https_ota_finish(https_ota_handle);
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "HTTP OTA成功，准备重启...");
                ensure_ota_status_buffer();
                snprintf(ota_status, 128, "升级成功，准备重启...");
                ota_progress_percent = 100;
                vTaskDelay(pdMS_TO_TICKS(2000));
                esp_restart();
                return;
            } else {
                ESP_LOGE(TAG, "esp_https_ota_finish失败: %s", esp_err_to_name(ret));
                ensure_ota_status_buffer();
                snprintf(ota_status, 128, "固件验证失败: %s", esp_err_to_name(ret));
                esp_https_ota_abort(https_ota_handle);
                break;
            }
        } else {
            ESP_LOGE(TAG, "esp_https_ota_perform失败: %s", esp_err_to_name(ret));
            esp_https_ota_abort(https_ota_handle);
            if (ret == ESP_ERR_HTTP_CONNECT) {
                ensure_ota_status_buffer();
                snprintf(ota_status, 128, "下载中断，准备重试...");
                continue;
            } else {
                break;
            }
        }
    }
    
    ESP_LOGE(TAG, "HTTP固件升级失败: %s (0x%x)", esp_err_to_name(ret), ret);
    ensure_ota_status_buffer();
    snprintf(ota_status, 128, "HTTP升级失败: %s", esp_err_to_name(ret));
}

// HTTPS OTA任务
static void https_ota_task(char *updateUrl) {
    ESP_LOGI(TAG, "======================================================================");
    ESP_LOGI(TAG, "开始HTTPS OTA升级: %s", updateUrl);
    ESP_LOGI(TAG, "======================================================================");
    
    // 准备OTA升级，停止串口任务和工作任务
    prepare_for_ota();
    
    esp_http_client_config_t config = {
        .url = updateUrl,
        .cert_pem = (char *)server_cert_pem_start,
        .event_handler = _http_event_handler_ota,
        .keep_alive_enable = true,
        .keep_alive_idle = 180,   // TCP keep-alive空闲时间3分钟
        .keep_alive_interval = 60, // TCP keep-alive间隔60秒
        .keep_alive_count = 3,    // TCP keep-alive重试次数
        .timeout_ms = 120000,     // 增加超时到120秒
        .buffer_size = 12288,     // 初始12KB，速度优先
        .buffer_size_tx = 4096,   // 初始4KB
        .user_agent = "ESP32-485HUB-OTA/2.0",
        .skip_cert_common_name_check = true,
        .disable_auto_redirect = false,  // 允许自动重定向
        .max_redirection_count = 5,      // 最大重定向次数
    };

    esp_https_ota_config_t ota_config = {
        .http_config = &config,
        .http_client_init_cb = NULL,
        .bulk_flash_erase = true,
        .partial_http_download = true,      // 启用分段下载
        .max_http_request_size = 24576,     // 初始24KB，速度优先
        .buffer_caps = 0,  // Use default capability
        .ota_resumption = false,
        .ota_image_bytes_written = 0,
    };

    ESP_LOGI(TAG, "======================================================================");
    ESP_LOGI(TAG, "尝试从 %s 下载更新", config.url);
    
    // 使用手动OTA流程以获取进度
    esp_https_ota_handle_t https_ota_handle = NULL;
    
    // 重置进度
    ota_progress_percent = 0;
    ensure_ota_status_buffer();
    snprintf(ota_status, 128, "开始连接HTTPS服务器...");
    
    // 更细粒度自适应（HTTPS）：按1KB粒度搜索最佳组合
    size_t largest_8bit_https = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const int safety_https = 1024;
    const int min_req_https = 16384;
    const int max_req_https = 32768;
    const int min_rx_https  = 8192;
    const int max_rx_https  = 14336;

    int req_start_https = (int)largest_8bit_https - safety_https;
    if (req_start_https > max_req_https) req_start_https = max_req_https;
    if (req_start_https < min_req_https) req_start_https = min_req_https;
    req_start_https = (req_start_https / 1024) * 1024;

    esp_err_t ret = ESP_FAIL;
    for (int req = req_start_https; req >= min_req_https && ret != ESP_OK; req -= 1024) {
        for (int rx = min_rx_https; rx <= max_rx_https && ret != ESP_OK; rx += 1024) {
            config.buffer_size = rx;
            config.buffer_size_tx = 2048;
            ota_config.max_http_request_size = req;
            ESP_LOGI(TAG, "尝试启动HTTPS OTA: rx=%d, tx=%d, req=%d (largest=%u)", rx, 2048, req, (unsigned)largest_8bit_https);
            ret = esp_https_ota_begin(&ota_config, &https_ota_handle);
            if (ret == ESP_ERR_NO_MEM) {
                continue;
            }
        }
    }
    if (ret != ESP_OK) {
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "HTTPS连接失败: %s", esp_err_to_name(ret));
        return;
    }
    
    ESP_LOGI(TAG, "HTTPS连接成功，开始下载...");
    ensure_ota_status_buffer();
    snprintf(ota_status, 128, "HTTPS连接成功，开始下载...");
    
    // 执行OTA，逐步更新进度
    int last_percent = 0;
    while (1) {
        ret = esp_https_ota_perform(https_ota_handle);
        if (ret != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        
        // 更新进度 (减少日志输出频率以提升速度)
        int image_size = esp_https_ota_get_image_size(https_ota_handle);
        int image_done = esp_https_ota_get_image_len_read(https_ota_handle);
        if (image_size > 0) {
            ota_progress_percent = (image_done * 100) / image_size;
            ensure_ota_status_buffer();
            snprintf(ota_status, 128, "下载中... %d%%", ota_progress_percent);
            // 只在进度变化超过5%时才输出日志，减少CPU开销
            if (ota_progress_percent - last_percent >= 5) {
                ESP_LOGI(TAG, "HTTPS OTA进度: %d%% (%d/%d)", ota_progress_percent, image_done, image_size);
                last_percent = ota_progress_percent;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(20)); // 增加到20ms，减少任务切换开销，提升下载速度0 
    }
    
    if (ret == ESP_OK) {
        // 完成OTA
        ESP_LOGI(TAG, "HTTPS下载完成，正在验证...");
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "下载完成，正在验证...");
        ota_progress_percent = 95;
        
        ret = esp_https_ota_finish(https_ota_handle);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "HTTPS OTA成功，准备重启...");
            ensure_ota_status_buffer();
            snprintf(ota_status, 128, "升级成功，准备重启...");
            ota_progress_percent = 100;
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else {
            ESP_LOGE(TAG, "esp_https_ota_finish失败: %s", esp_err_to_name(ret));
            ensure_ota_status_buffer();
            snprintf(ota_status, 128, "固件验证失败: %s", esp_err_to_name(ret));
            esp_https_ota_abort(https_ota_handle);
        }
    } else {
        ESP_LOGE(TAG, "esp_https_ota_perform失败: %s", esp_err_to_name(ret));
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "HTTPS下载失败: %s", esp_err_to_name(ret));
        esp_https_ota_abort(https_ota_handle);
    }
}

// 智能选择OTA方式的主函数
void simple_ota_task(char *updateUrl) {
    if (updateUrl == NULL || strlen(updateUrl) == 0) {
        ESP_LOGE(TAG, "URL为空或无效");
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "错误：URL为空或无效");
        return;
    }
    
    if (strlen(updateUrl) > 512) {
        ESP_LOGE(TAG, "URL过长");
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "错误：URL过长");
        return;
    }
    
    ESP_LOGI(TAG, "检测到URL: %s", updateUrl);
    
    // 根据URL协议自动选择OTA方式
    if (is_https_url(updateUrl)) {
        ESP_LOGI(TAG, "检测到HTTPS协议，使用HTTPS OTA");
        https_ota_task(updateUrl);
    } else if (strncmp(updateUrl, "http://", 7) == 0) {
        ESP_LOGI(TAG, "检测到HTTP协议，使用HTTP OTA");
        http_ota_task(updateUrl);
    } else {
        ESP_LOGE(TAG, "不支持的URL协议: %s", updateUrl);
        ESP_LOGE(TAG, "支持的协议: http:// 或 https://");
        ensure_ota_status_buffer();
        snprintf(ota_status, 128, "不支持的协议: %s", updateUrl);
        return;
    }
}

static void print_sha256(const uint8_t *image_hash, const char *label) {
  char hash_print[HASH_LEN * 2 + 1];
  hash_print[HASH_LEN * 2] = 0;
  for (int i = 0; i < HASH_LEN; ++i) {
    sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
  }
  ESP_LOGI(TAG, "%s %s", label, hash_print);
}

void get_sha256_of_partitions(void) {
  uint8_t sha_256[HASH_LEN] = {0};
  esp_partition_t partition;
  // get sha256 digest for bootloader
  partition.address = ESP_BOOTLOADER_OFFSET;
  partition.size = ESP_PARTITION_TABLE_OFFSET;
  partition.type = ESP_PARTITION_TYPE_APP;
  esp_partition_get_sha256(&partition, sha_256);
  print_sha256(sha_256, "SHA-256 for bootloader: ");
  // get sha256 digest for running partition
  esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
  print_sha256(sha_256, "SHA-256 for current firmware: ");
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
