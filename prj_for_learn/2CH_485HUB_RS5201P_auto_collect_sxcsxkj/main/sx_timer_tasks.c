/*
 * @Author: AI Assistant
 * @Date: 2024-07-08
 * @Description: 使用ESP32定时器替代FreeRTOS任务，减少内存占用
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h" // 添加信号量头文件
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_gpio.h"
#include "sx_utils.h"
#include "cJSON.h"
#include "sx_task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
// #include "sx_http_tls_fix.h" // 引入新的TLS修复头文件
#include "esp_tls.h"
#include <inttypes.h>
// 定义MIN宏，用于HTTP处理程序中
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
// 在此处手动定义禁用证书验证的宏
#ifndef CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY
#define CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY 1
#endif
// 添加一个辅助宏来确保跳过证书验证
#define SKIP_CERT_VERIFICATION true

extern const uint8_t server_cert_pem_start[] asm("_binary_ca_cert_pem_start");
extern const uint8_t server_cert_pem_end[] asm("_binary_ca_cert_pem_end");

// 定义HTTP缓冲区大小
#define MAX_HTTP_OUTPUT_BUFFER 2048

static const char *TAG = "SX_TIMER";

// 添加HTTP请求互斥量，避免同时执行多个HTTP请求
SemaphoreHandle_t http_mutex = NULL;

// 引用HTTP客户端模块中的忙状态标记
extern volatile bool client_http_busy;

// 静态开关：控制平台定时器回调日志输出（0=关闭，1=开启）
static int s_platform_timer_log_enabled = 0;

// HTTP事件处理函数声明
esp_err_t _timer_http_event_handler(esp_http_client_event_t *evt);
// 平台连接定时器回调函数声明
void platform_timer_callback(void *arg);
// 心跳定时器回调函数声明
void heartbeat_timer_callback(void *arg);

// 平台连接定时器回调函数实现
void platform_timer_callback(void *arg) {
  if (s_platform_timer_log_enabled) {
    ESP_LOGI("PLATFORM_TIMER", "Platform timer callback triggered");
  }
  // 这里可以添加平台连接相关的逻辑
}

// 心跳定时器回调函数实现
void heartbeat_timer_callback(void *arg) {
  ESP_LOGI("HEARTBEAT_TIMER", "Heartbeat timer callback triggered");
  // 这里可以添加心跳发送相关的逻辑
}

// 新增：初始化按键扫描定时器函数声明
esp_err_t init_key_timer(void);
// 新增：初始化I2C传感器读取定时器函数声明
esp_err_t init_i2c_timer(void);

// 定时器句柄（声明为extern后需要在此定义）
esp_timer_handle_t led_timer = NULL;
esp_timer_handle_t key_timer = NULL;
esp_timer_handle_t i2c_timer = NULL;
esp_timer_handle_t free_timer = NULL;
esp_timer_handle_t platform_timer = NULL;
esp_timer_handle_t heartbeat_timer = NULL;
esp_timer_handle_t time_sync_timer = NULL;
esp_timer_handle_t time_sync_led_timer =
    NULL; // 导出LED闪烁定时器句柄，去掉static关键字

// 全局变量
static bool ap_closed = false;
static int64_t ap_start_time = 0;
static int ap_timeout_minutes = 30;     // AP开放时间（分钟），默认30分钟
static int64_t last_logged_minute = -1; // 用于跟踪上次打印剩余时间的分钟数

// 时间同步任务句柄
TaskHandle_t time_sync_task_handle = NULL;

// 全局按键状态标志
volatile bool key_short_press_detected = false;
volatile bool key_long_press_detected = false;
static volatile uint32_t key_press_start_time = 0;

// 从NVS读取AP超时配置
static void load_ap_timeout_config(void) {
  nvs_handle_t nvs_handle;

  // 打开NVS
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "无法打开NVS，使用默认AP超时时间: %d 分钟",
             ap_timeout_minutes);
    return;
  }

  // 读取AP超时配置
  char timeout_str[16];
  size_t required_size = sizeof(timeout_str);
  err = nvs_get_str(nvs_handle, "ap_timeout", timeout_str, &required_size);
  if (err == ESP_OK) {
    int timeout_value = atoi(timeout_str);
    if (timeout_value == 0 ||
        (timeout_value >= 1 &&
         timeout_value <= 1440)) { // 0=从不关闭，1分钟到24小时
      ap_timeout_minutes = timeout_value;
      if (timeout_value == 0) {
        ESP_LOGI(TAG, "从NVS读取AP超时配置: 从不关闭");
      } else {
        ESP_LOGI(TAG, "从NVS读取AP超时配置: %d 分钟", ap_timeout_minutes);
      }
    } else {
      ESP_LOGW(TAG, "AP超时配置值无效: %d，使用默认值: %d 分钟", timeout_value,
               ap_timeout_minutes);
    }
  } else {
    ESP_LOGI(TAG, "未找到AP超时配置，使用默认值: %d 分钟", ap_timeout_minutes);
  }

  nvs_close(nvs_handle);
}

// 重新加载AP超时配置（用于运行时更新）
void reload_ap_timeout_config(void) { load_ap_timeout_config(); }

// 按键定时器回调函数 - 只检测状态，不执行操作
static void key_timer_callback(void *arg) {
  static bool key_was_pressed = false;
  static uint32_t press_count = 0;

  bool key_current_state = gpio_get_level(KEY);

  if (key_current_state == 0) { // 按键按下
    if (!key_was_pressed) {
      // 按键刚按下
      key_was_pressed = true;
      press_count = 0;
      key_press_start_time = esp_timer_get_time();
    } else {
      // 按键持续按下
      press_count++;

      // 长按检测 (300 * 10ms = 3秒)
      if (press_count >= 300 && !key_long_press_detected) {
        key_long_press_detected = true;
        ESP_LOGI(TAG, "按键长按检测到");
      }
    }
  } else { // 按键释放
    if (key_was_pressed) {
      // 按键刚释放
      key_was_pressed = false;

      // 计算按键持续时间
      uint32_t press_duration =
          (esp_timer_get_time() - key_press_start_time) / 1000; // 转换为毫秒

      // 短按检测 (100ms - 3秒之间)
      if (press_duration >= 100 && press_duration < 3000 &&
          !key_long_press_detected) {
        key_short_press_detected = true;
        ESP_LOGI(TAG, "按键短按检测到，持续时间: %d ms", press_duration);
      }

      // 重置长按标志
      key_long_press_detected = false;
    }
  }
}

// 内存监控定时器回调函数
static void free_timer_callback(void *arg) {
  // 注释掉调试输出以减少日志信息
  // printf("=================================================\r\n");
  // printf("\r\nremaining memory = %zu,minimum memory = %zu\r\n",
  //        xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());
  // esp_reset_reason_t reason = esp_reset_reason();
  // printf("Reset reason: %s\n", reset_reason_to_string(reason));

  wifi_mode_t current_mode;
  if (esp_wifi_get_mode(&current_mode) == ESP_OK) {
    // 检查当前是否处于AP模式
    if (current_mode == WIFI_MODE_AP || current_mode == WIFI_MODE_APSTA) {
      // 处于AP模式
      if (!ap_closed) {
        // 检查是否设置为"从不关闭"(值为0)
        if (ap_timeout_minutes == 0) {
          // 从不关闭模式：只在首次检测时记录，不进行倒计时
          if (ap_start_time == 0) {
            ap_start_time = esp_timer_get_time();
            printf("AP mode started, set to never close\n");
          }
          // 从不关闭模式下不执行倒计时逻辑
        } else {
          // 正常倒计时模式
          int64_t current_time = esp_timer_get_time();
          if (ap_start_time == 0) {
            ap_start_time = current_time;
            printf("AP mode started, will close after %d minutes\n",
                   ap_timeout_minutes);
          } else {
            // 每60秒打印一次剩余时间（可选，用于调试）
            int64_t elapsed_seconds =
                (current_time - ap_start_time) / 1000000LL;
            if (elapsed_seconds > 0 && elapsed_seconds % 60 == 0 &&
                elapsed_seconds != last_logged_minute * 60) {
              int remaining_minutes =
                  ap_timeout_minutes - (elapsed_seconds / 60);
              if (remaining_minutes > 0) {
                printf("AP will close in %d minutes\n", remaining_minutes);
              }
              last_logged_minute = elapsed_seconds / 60;
            }

            if ((current_time - ap_start_time) >
                (ap_timeout_minutes * 60LL * 1000000LL)) { // 配置的分钟数
              printf("AP timeout reached (%d minutes), closing AP mode...\n",
                     ap_timeout_minutes);
              ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
              ap_closed = true;
              ap_start_time = 0;       // 重置计时器
              last_logged_minute = -1; // 重置日志记录
              printf("AP closed successfully\n");
            }
          }
        }
      }
    } else {
      // 不在AP模式，重置计时器和状态
      if (ap_start_time != 0 || ap_closed) {
        ap_start_time = 0;
        ap_closed = false;
        last_logged_minute = -1;
        // printf("WiFi mode changed, reset AP timer\n");
      }
    }
  }

  // printf("=================================================\r\n");
}

esp_err_t _timer_http_event_handler(esp_http_client_event_t *evt) {
  static char *timer_output_buffer =
      NULL;                        // 静态缓冲区，避免与其他函数的静态变量冲突
  static int timer_output_len = 0; // 静态长度，避免与其他函数的静态变量冲突

  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
    // 确保在错误事件中释放资源
    if (timer_output_buffer != NULL) {
      free(timer_output_buffer);
      timer_output_buffer = NULL;
      timer_output_len = 0;
    }
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
    // 连接时清理旧缓冲区，确保每次请求都使用新的缓冲区
    if (timer_output_buffer != NULL) {
      free(timer_output_buffer);
      timer_output_buffer = NULL;
      timer_output_len = 0;
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
    if (evt->data_len > 0) {
      // 如果用户数据直接可用（传递了一个预分配的缓冲区）
      if (evt->user_data) {
        // 确保不会溢出用户缓冲区
        int copy_len =
            MIN(evt->data_len, MAX_HTTP_OUTPUT_BUFFER - timer_output_len);
        if (copy_len > 0) {
          memcpy((char *)evt->user_data + timer_output_len, evt->data,
                 copy_len);
          timer_output_len += copy_len;
          // 确保字符串以NULL结尾
          ((char *)evt->user_data)[timer_output_len] = '\0';
        }
      } else {
        // 动态分配内存的情况
        int content_length = esp_http_client_get_content_length(evt->client);
        if (content_length <= 0) {
          content_length = MAX_HTTP_OUTPUT_BUFFER; // 默认大小
        }

        // 第一次接收数据时分配内存
        if (timer_output_buffer == NULL) {
          timer_output_buffer = malloc(content_length + 1); // +1 用于NULL终止符
          if (timer_output_buffer == NULL) {
            ESP_LOGE(TAG, "无法为HTTP响应分配内存");
            return ESP_FAIL;
          }
          memset(timer_output_buffer, 0, content_length + 1); // 初始化内存
          timer_output_len = 0;
        }

        // 确保不会溢出缓冲区
        if (timer_output_len + evt->data_len <= content_length) {
          memcpy(timer_output_buffer + timer_output_len, evt->data,
                 evt->data_len);
          timer_output_len += evt->data_len;
          timer_output_buffer[timer_output_len] = '\0'; // 确保字符串以NULL结尾
        } else {
          ESP_LOGW(TAG, "数据长度超过预分配缓冲区大小，截断数据");
          int copy_len = content_length - timer_output_len;
          if (copy_len > 0) {
            memcpy(timer_output_buffer + timer_output_len, evt->data, copy_len);
            timer_output_len += copy_len;
            timer_output_buffer[timer_output_len] = '\0';
          }
        }
      }

      // 输出收到的数据用于调试
      // ESP_LOGI(TAG, "收到的数据: %.*s", evt->data_len, (char*)evt->data);
    }
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
    // 重置输出长度但不释放用户传入的缓冲区
    timer_output_len = 0;
    // 检查并释放动态分配的内存
    if (timer_output_buffer != NULL && evt->user_data != timer_output_buffer) {
      free(timer_output_buffer);
      timer_output_buffer = NULL;
    }
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
    int mbedtls_err = 0;
    esp_err_t err = esp_tls_get_and_clear_last_error(
        (esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
    if (err != 0) {
      ESP_LOGE(TAG, "TLS错误: ESP错误码=0x%x, mbedTLS错误码=0x%x", err,
               mbedtls_err);
      ESP_LOGE(TAG, "TLS错误详情：%s", esp_err_to_name(err));
    }
    // 确保在断开连接时释放资源
    if (timer_output_buffer != NULL) {
      free(timer_output_buffer);
      timer_output_buffer = NULL;
      timer_output_len = 0;
    }
    break;
  case HTTP_EVENT_REDIRECT:
    ESP_LOGI(TAG, "HTTP_EVENT_REDIRECT");
    // 处理重定向事件
    esp_http_client_set_redirection(evt->client);
    break;
  }
  return ESP_OK;
}

// 新增初始化按键扫描定时器的函数
esp_err_t init_key_timer(void) {
  esp_timer_create_args_t timer_args;
  esp_err_t err;

  // 如果按键定时器已存在，直接返回
  if (key_timer != NULL) {
    ESP_LOGI(TAG, "按键扫描定时器已存在，无需重新初始化");
    return ESP_OK;
  }

  // 初始化按键扫描定时器 (10ms周期)
  timer_args = (esp_timer_create_args_t){.callback = &key_timer_callback,
                                         .name = "key_timer"};
  err = esp_timer_create(&timer_args, &key_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法创建按键扫描定时器: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_timer_start_periodic(key_timer, 10 * 1000); // 10ms
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法启动按键扫描定时器: %s", esp_err_to_name(err));
    esp_timer_delete(key_timer);
    key_timer = NULL;
    return err;
  }

  ESP_LOGI(TAG, "按键扫描定时器初始化成功，周期10ms");
  return ESP_OK;
}

// 初始化所有定时器任务，替代原来的FreeRTOS任务
esp_err_t init_timer_tasks(void) {
  esp_timer_create_args_t timer_args;
  esp_err_t err;

  // 读取AP超时配置
  load_ap_timeout_config();

  // 创建HTTP请求互斥量
  if (http_mutex == NULL) {
    http_mutex = xSemaphoreCreateMutex();
    if (http_mutex == NULL) {
      ESP_LOGE(TAG, "无法创建HTTP请求互斥量");
      return ESP_FAIL;
    }
    ESP_LOGI(TAG, "HTTP互斥量创建成功");
  } else {
    ESP_LOGI(TAG, "HTTP互斥量已存在，无需重新创建");
  }

  // 清理任何已存在的定时器，避免重复创建
  if (free_timer != NULL) {
    ESP_LOGI(TAG, "清理已存在的内存监控定时器");
    esp_timer_stop(free_timer);
    esp_timer_delete(free_timer);
    free_timer = NULL;
  }

  if (time_sync_led_timer != NULL) {
    ESP_LOGI(TAG, "清理已存在的LED闪烁定时器");
    esp_timer_stop(time_sync_led_timer);
    esp_timer_delete(time_sync_led_timer);
    time_sync_led_timer = NULL;
  }

  if (time_sync_timer != NULL) {
    ESP_LOGI(TAG, "清理已存在的时间同步定时器");
    esp_timer_stop(time_sync_timer);
    esp_timer_delete(time_sync_timer);
    time_sync_timer = NULL;
  }

  // 初始化按键扫描定时器
  if (key_timer == NULL) {
    err = init_key_timer();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "无法初始化按键扫描定时器");
      return err;
    }
  }

  // 初始化内存监控定时器 (5000ms周期)
  timer_args = (esp_timer_create_args_t){.callback = &free_timer_callback,
                                         .name = "free_timer"};
  err = esp_timer_create(&timer_args, &free_timer);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法创建内存监控定时器: %s", esp_err_to_name(err));

    if (key_timer) {
      esp_timer_stop(key_timer);
      esp_timer_delete(key_timer);
      key_timer = NULL;
    }
    return err;
  }

  err = esp_timer_start_periodic(free_timer, 5000 * 1000); // 5000ms
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法启动内存监控定时器: %s", esp_err_to_name(err));
    // 清理资源
    esp_timer_delete(free_timer);
    free_timer = NULL;

    if (key_timer) {
      esp_timer_stop(key_timer);
      esp_timer_delete(key_timer);
      key_timer = NULL;
    }
    return err;
  }

  return ESP_OK;
}

// 停止所有定时器任务
esp_err_t stop_timer_tasks(void) {
  esp_err_t err;
  esp_err_t result = ESP_OK; // 跟踪整体结果

  if (key_timer) {
    err = esp_timer_stop(key_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "无法停止按键扫描定时器: %s", esp_err_to_name(err));
      result = err;
    }
    err = esp_timer_delete(key_timer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "无法删除按键扫描定时器: %s", esp_err_to_name(err));
      result = err;
    }
    key_timer = NULL;
  }

  if (free_timer) {
    err = esp_timer_stop(free_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "无法停止内存监控定时器: %s", esp_err_to_name(err));
      result = err;
    }
    err = esp_timer_delete(free_timer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "无法删除内存监控定时器: %s", esp_err_to_name(err));
      result = err;
    }
    free_timer = NULL;
  }

  if (platform_timer) {
    err = esp_timer_stop(platform_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "无法停止平台连接定时器: %s", esp_err_to_name(err));
      result = err;
    }
    err = esp_timer_delete(platform_timer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "无法删除平台连接定时器: %s", esp_err_to_name(err));
      result = err;
    }
    platform_timer = NULL;
  }

  if (heartbeat_timer) {
    err = esp_timer_stop(heartbeat_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "无法停止心跳定时器: %s", esp_err_to_name(err));
      result = err;
    }
    err = esp_timer_delete(heartbeat_timer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "无法删除心跳定时器: %s", esp_err_to_name(err));
      result = err;
    }
    heartbeat_timer = NULL;
  }

  if (time_sync_led_timer) {
    err = esp_timer_stop(time_sync_led_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "无法停止时间同步LED闪烁定时器: %s", esp_err_to_name(err));
      result = err;
    }
    err = esp_timer_delete(time_sync_led_timer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "无法删除时间同步LED闪烁定时器: %s", esp_err_to_name(err));
      result = err;
    }
    time_sync_led_timer = NULL;
  }

  if (time_sync_timer) {
    err = esp_timer_stop(time_sync_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "无法停止时间同步定时器: %s", esp_err_to_name(err));
      result = err;
    }
    err = esp_timer_delete(time_sync_timer);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "无法删除时间同步定时器: %s", esp_err_to_name(err));
      result = err;
    }
    time_sync_timer = NULL;
  }

  // 删除HTTP互斥量
  if (http_mutex != NULL) {
    vSemaphoreDelete(http_mutex);
    http_mutex = NULL;
  }

  if (result == ESP_OK) {
    ESP_LOGI(TAG, "定时器任务已全部正常停止");
  } else {
    ESP_LOGW(TAG, "定时器任务停止过程中出现一些错误，但已尽可能清理");
  }
  return result;
}
