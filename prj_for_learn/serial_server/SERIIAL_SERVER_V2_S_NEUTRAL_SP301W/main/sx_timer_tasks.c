/*
 * @Author: AI Assistant
 * @Date: 2024-07-08
 * @Description: 使用ESP32定时器替代FreeRTOS任务，减少内存占用
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"  // 添加信号量头文件
#include "nvs.h"
#include "nvs_flash.h"
#include "esp_http_client.h"



#include "sx_gpio.h"


#include "sx_utils.h"
#include "sx_platform.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sx_task.h"
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

// 新增LED闪烁定时器回调函数声明
void time_sync_led_callback(void *arg);
// HTTP事件处理函数声明
esp_err_t _timer_http_event_handler(esp_http_client_event_t *evt);
// 平台连接定时器回调函数声明
void platform_timer_callback(void *arg);
// 心跳定时器回调函数声明
void heartbeat_timer_callback(void *arg);
// 心跳看门狗回调函数声明
void heartbeat_watchdog_callback(void *arg);
// 新增：初始化按键扫描定时器函数声明
esp_err_t init_key_timer(void);

// 新增初始化LED闪烁定时器的函数，使用独立的定时器任务
esp_err_t init_led_timer(void);
// 按键定时器回调函数声明
static void key_timer_callback(void *arg);

// 定时器句柄（声明为extern后需要在此定义）
esp_timer_handle_t led_timer = NULL;
esp_timer_handle_t key_timer = NULL;
esp_timer_handle_t i2c_timer = NULL;
esp_timer_handle_t free_timer = NULL;
esp_timer_handle_t platform_timer = NULL;
esp_timer_handle_t heartbeat_timer = NULL;
esp_timer_handle_t time_sync_led_timer = NULL; // 导出LED闪烁定时器句柄，去掉static关键字

// 全局变量
static bool ap_closed = false;
static int64_t ap_start_time = 0;
static int platform_send_count = 0;
static bool reset_requested = false;      // 标记是否请求重置设备
static bool restart_requested = false;    // 标记是否请求重启设备
static bool reset_task_running = false;   // 标记重置/重启任务是否已经在运行

// 时间同步任务句柄
TaskHandle_t time_sync_task_handle = NULL;

// 心跳检查任务句柄
static TaskHandle_t heartbeat_check_task_handle = NULL;

// 心跳检查异步任务
static void heartbeat_check_task(void *pvParameters) {
    const char *TAG = "HEARTBEAT_TASK";
    extern bool is_network_connected;
    extern bool is_intranet_mode;
    extern char got_ip_gw[16];
    extern char got_ip_addrs[16];

    bool wdt_registered = false;
    esp_err_t wdt_err = esp_task_wdt_add(NULL);
    if (wdt_err == ESP_OK) {
        wdt_registered = true;
    } else {
        ESP_LOGW(TAG, "无法将心跳任务加入看门狗: %s", esp_err_to_name(wdt_err));
    }

    // 获取参数
    bool old_network_connected = *((bool*)pvParameters);
    bool old_intranet_mode = *((bool*)pvParameters + 1);

    ESP_LOGI(TAG, "开始异步心跳检查");

    // 改进的网络状态检查：先检查是否有IP地址
    bool has_valid_ip = (strlen(got_ip_addrs) > 0 &&
                        strcmp(got_ip_addrs, "0.0.0.0") != 0 &&
                        strcmp(got_ip_addrs, "---") != 0);

    if (!has_valid_ip) {
        ESP_LOGW(TAG, "没有有效的IP地址，跳过心跳检查");
        heartbeat_check_task_handle = NULL;
        free(pvParameters);
        if (wdt_registered) {
            (void)esp_task_wdt_delete(NULL);
        }
        vTaskDelete(NULL);
        return;
    }

    // 检查网关地址是否有效（用于判断基本网络连通性）
    bool has_valid_gateway = (strlen(got_ip_gw) > 0 &&
                             strcmp(got_ip_gw, "0.0.0.0") != 0 &&
                             strcmp(got_ip_gw, "---") != 0);

    // 尝试获取HTTP互斥量，避免与平台连接冲突
    extern SemaphoreHandle_t http_mutex;
    if (http_mutex != NULL) {
        if (xSemaphoreTake(http_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
            ESP_LOGW(TAG, "HTTP资源被占用，跳过本次心跳检查");
            heartbeat_check_task_handle = NULL;
            free(pvParameters);
            if (wdt_registered) {
                (void)esp_task_wdt_delete(NULL);
            }
            vTaskDelete(NULL);
            return;
        }
    }

    // 参数已经使用完，立即释放
    free(pvParameters);

    // 喂狗并小延时，给系统喘息机会
    if (wdt_registered) {
        esp_task_wdt_reset();
    }
    vTaskDelay(pdMS_TO_TICKS(5));

    // 第二步：获取平台心跳（调用前暂时移除看门狗，避免长阻塞被误判）
    bool restore_wdt_after_http = false;
    if (wdt_registered) {
        (void)esp_task_wdt_delete(NULL);
        wdt_registered = false;
        restore_wdt_after_http = true;
    }
    int heartbeat_status = http_rest_with_url_heartbeat("https://platform.likong-iot.com/api/v1/device_link/heartbeat", "");

    if (restore_wdt_after_http) {
        esp_err_t readd_err = esp_task_wdt_add(NULL);
        if (readd_err == ESP_OK) {
            wdt_registered = true;
        } else {
            ESP_LOGW(TAG, "心跳任务重新加入看门狗失败: %s", esp_err_to_name(readd_err));
        }
    }

    // 喂狗
    if (wdt_registered) {
        esp_task_wdt_reset();
    }

    // 释放互斥量
    if (http_mutex != NULL) {
        xSemaphoreGive(http_mutex);
    }

    // 使用静态变量来跟踪计数器
    static uint32_t consecutive_failures = 0;
    static uint32_t consecutive_successes = 0;
    static uint32_t total_heartbeat_attempts = 0;
    static uint32_t successful_heartbeats = 0;

    total_heartbeat_attempts++;

    if (heartbeat_status == 200) {
        // 平台心跳成功 - 外网模式
        ESP_LOGI(TAG, "平台心跳成功，外网模式");
        consecutive_successes++;
        consecutive_failures = 0;  // 重置失败计数
        successful_heartbeats++;

        // 更新网络状态
        is_network_connected = true;
        is_intranet_mode = false;

        ESP_LOGI(TAG, "心跳统计: 成功率 %u/%u (%.1f%%)",
                 (unsigned int)successful_heartbeats, (unsigned int)total_heartbeat_attempts,
                 (float)successful_heartbeats * 100.0 / total_heartbeat_attempts);
    } else if (heartbeat_status > 0) {
        // HTTP请求成功但返回非200状态码
        ESP_LOGW(TAG, "平台心跳HTTP状态码: %d", heartbeat_status);

        if (has_valid_gateway) {
            // 如果有网关，切换到内网模式但保持网络连接状态
            ESP_LOGI(TAG, "HTTP请求成功但状态码异常，切换到内网模式");
            is_network_connected = true;
            is_intranet_mode = true;
            consecutive_successes++;
            consecutive_failures = 0;
        } else {
            consecutive_failures++;
            consecutive_successes = 0;
        }
    } else {
        // HTTP请求完全失败
        ESP_LOGW(TAG, "平台心跳HTTP请求失败，错误码: %d", heartbeat_status);
        consecutive_failures++;
        consecutive_successes = 0;

        if (has_valid_gateway) {
            // 如果有网关地址，说明本地网络可能正常，切换到内网模式
            ESP_LOGI(TAG, "HTTP请求失败但有网关地址，切换到内网模式");
            is_network_connected = true;
            is_intranet_mode = true;
        } else {
            // 完全没有网络连接
            if (consecutive_failures >= 5) {
                ESP_LOGW(TAG, "连续心跳失败，暂时标记网络断开");
                is_network_connected = false;
                is_intranet_mode = false;
            }
        }
    }

    // 检查状态是否发生变化，如果有变化则输出详细日志
    if (old_network_connected != is_network_connected || old_intranet_mode != is_intranet_mode) {
        ESP_LOGI(TAG, "网络状态变化: 连接状态 %s->%s, 内网模式 %s->%s",
                 old_network_connected ? "是" : "否", is_network_connected ? "是" : "否",
                 old_intranet_mode ? "是" : "否", is_intranet_mode ? "是" : "否");
    }

    // 输出当前网络状态用于调试
    ESP_LOGI(TAG, "当前网络状态: 连接=%s, 内网模式=%s, IP=%s, 网关=%s",
             is_network_connected ? "是" : "否",
             is_intranet_mode ? "是" : "否",
             got_ip_addrs,
             got_ip_gw);

    // 再次小延时并喂狗
    vTaskDelay(pdMS_TO_TICKS(5));
    if (wdt_registered) {
        esp_task_wdt_reset();
    }

    // 清除任务句柄
    heartbeat_check_task_handle = NULL;
    if (wdt_registered) {
        (void)esp_task_wdt_delete(NULL);
    }
    vTaskDelete(NULL);
}

// 心跳定时器回调函数
void heartbeat_timer_callback(void *arg) {
    static bool first_heartbeat = true;
    static bool last_network_state = false;
    static uint32_t consecutive_failures = 0;
    static uint32_t consecutive_successes = 0;
    static int64_t last_heartbeat_time = 0;
    static uint32_t heartbeat_interval = 15000; // 默认15秒间隔
    int64_t current_time = esp_timer_get_time() / 1000;

    const char *TAG = "HEARTBEAT_TASK";
    extern bool is_network_connected;
    extern bool is_intranet_mode;
    extern char got_ip_addrs[16];
    extern char got_ip_gw[16];

    // 记录当前状态用于比较
    bool old_network_connected = is_network_connected;
    bool old_intranet_mode = is_intranet_mode;

    // 基本的IP地址检查
    bool has_valid_ip = (strlen(got_ip_addrs) > 0 &&
                        strcmp(got_ip_addrs, "0.0.0.0") != 0 &&
                        strcmp(got_ip_addrs, "---") != 0);

    // 检测网络状态变化：从断开到连接
    bool network_recovered = (!last_network_state && has_valid_ip);
    if (network_recovered) {
        ESP_LOGI(TAG, "检测到网络恢复，重置心跳定时器状态");
        first_heartbeat = true;
        (void)consecutive_failures;   // 标记为可能未使用，避免警告
        (void)consecutive_successes;  // 标记为可能未使用，避免警告
        last_heartbeat_time = 0;
    }
    last_network_state = has_valid_ip;

    // 首次启动或网络恢复时设置周期性模式
    if (first_heartbeat) {
        first_heartbeat = false;
        ESP_LOGI(TAG, "初始化心跳定时器周期模式");

        esp_err_t stop_err = esp_timer_stop(heartbeat_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "停止心跳定时器时出现警告: %s", esp_err_to_name(stop_err));
        }

        esp_err_t err = esp_timer_start_periodic(heartbeat_timer, heartbeat_interval * 1000);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "无法启动心跳定时器周期模式: %s", esp_err_to_name(err));
            return;
        }
        ESP_LOGI(TAG, "心跳定时器已设置为周期模式 (%u秒间隔)", (unsigned int)(heartbeat_interval / 1000));
    }

    // 改进的网络检查逻辑：只检查是否有有效IP地址
    if (!has_valid_ip) {
        ESP_LOGD(TAG, "没有有效IP地址: %s，跳过心跳检查", got_ip_addrs);
        return;
    }

    // 检查心跳间隔时间（防止过于频繁）
    if (current_time - last_heartbeat_time < 10000) {  // 减少到10秒最小间隔
        ESP_LOGD(TAG, "距离上次心跳时间过短，跳过本次心跳");
        return;
    }

    last_heartbeat_time = current_time;

    // 检查是否已有心跳检查任务在运行
    if (heartbeat_check_task_handle != NULL) {
        ESP_LOGD(TAG, "心跳检查任务已在运行，跳过本次检查");
        return;
    }

    ESP_LOGI(TAG, "开始心跳检查 - IP: %s, 网关: %s", got_ip_addrs, got_ip_gw);

    // 创建异步任务来处理HTTP请求，避免阻塞定时器
    bool *task_params = malloc(sizeof(bool) * 2);
    if (task_params != NULL) {
        task_params[0] = old_network_connected;
        task_params[1] = old_intranet_mode;

        BaseType_t result = xTaskCreate(
            heartbeat_check_task,           // 任务函数
            "heartbeat_check",              // 任务名称
            4096,                           // 增加栈大小到8KB
            task_params,                    // 参数
            5,                              // 优先级
            &heartbeat_check_task_handle    // 任务句柄
        );

        if (result != pdPASS) {
            ESP_LOGE(TAG, "无法创建心跳检查任务，可能内存不足");
            free(task_params);
            heartbeat_check_task_handle = NULL;

            // 如果任务创建失败，尝试直接进行简单的心跳检查
            ESP_LOGW(TAG, "任务创建失败，尝试简化心跳检查");
            if (has_valid_ip) {
                is_network_connected = true;  // 至少有IP地址，认为网络基本可用
                ESP_LOGI(TAG, "基于IP地址状态更新网络连接标志");
            }
        } else {
            ESP_LOGI(TAG, "已启动异步心跳检查任务");
        }
    } else {
        ESP_LOGE(TAG, "无法分配心跳检查任务参数内存");

        // 内存分配失败时的降级处理
        if (has_valid_ip) {
            is_network_connected = true;
            ESP_LOGW(TAG, "内存不足，基于IP状态进行基础网络检查");
        }
    }
}

// 时间同步LED闪烁定时器回调函数
void time_sync_led_callback(void *arg) {
    static int intranet_flash_step = 0;  // 内网模式闪烁步骤计数器
    static int intranet_wait_counter = 0;  // 内网模式长灭等待计数器
    static bool last_intranet_mode = false;  // 记录上次的内网模式状态
    static bool last_network_connected = false;  // 记录上次的网络连接状态
    static bool last_wifi_active = false;  // 记录上次的WiFi状态
    extern bool is_network_connected; // 声明外部变量，用于判断网络是否连接
    extern bool is_intranet_mode;     // 声明外部变量，用于判断是否为内网模式
    extern bool is_wifi_active;       // 声明外部变量，用于判断是否为WiFi模式

    // 检测网络连接状态变化
    if (last_network_connected != is_network_connected) {
        ESP_LOGI(TAG, "LED检测到网络连接状态变化: %s -> %s",
                 last_network_connected ? "已连接" : "未连接",
                 is_network_connected ? "已连接" : "未连接");
        last_network_connected = is_network_connected;

        // 重置所有计数器和状态
        intranet_flash_step = 0;
        intranet_wait_counter = 0;
    }

    // 检测内网模式状态变化，重置相关计数器
    if (last_intranet_mode != is_intranet_mode) {
        ESP_LOGI(TAG, "LED检测到网络模式变化: %s -> %s",
                 last_intranet_mode ? "内网" : "外网",
                 is_intranet_mode ? "内网" : "外网");

        // 重置所有计数器和状态
        intranet_flash_step = 0;
        intranet_wait_counter = 0;

        last_intranet_mode = is_intranet_mode;
    }

    // 检测WiFi状态变化，重置相关计数器
    if (last_wifi_active != is_wifi_active) {
        ESP_LOGI(TAG, "LED检测到网络类型变化: %s -> %s",
                 last_wifi_active ? "WiFi" : "以太网",
                 is_wifi_active ? "WiFi" : "以太网");

        // 重置所有计数器和状态
        intranet_flash_step = 0;
        intranet_wait_counter = 0;

        last_wifi_active = is_wifi_active;
    }

    if (is_network_connected) {
        if (is_intranet_mode) {
            // 内网模式：快闪两下，正常灭一下的模式
            // 总周期约2秒：快闪(100ms亮+100ms灭) + 快闪(100ms亮+100ms灭) + 长灭(1.6秒)
            // 由于定时器现在是100ms周期，每次回调就是100ms间隔

            switch (intranet_flash_step) {
                case 0: // 第一次快闪 - 亮
                    if (is_wifi_active) {
                        LED_WIFI_ON();
                    } else {
                        LED_LAN_ON();
                    }
                    intranet_flash_step = 1;
                    intranet_wait_counter = 0; // 重置等待计数器
                    ESP_LOGD(TAG, "内网模式%sLED: 第一次快闪亮", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 1: // 第一次快闪 - 灭
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    intranet_flash_step = 2;
                    ESP_LOGD(TAG, "内网模式%sLED: 第一次快闪灭", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 2: // 第二次快闪 - 亮
                    if (is_wifi_active) {
                        LED_WIFI_ON();
                    } else {
                        LED_LAN_ON();
                    }
                    intranet_flash_step = 3;
                    ESP_LOGD(TAG, "内网模式%sLED: 第二次快闪亮", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 3: // 第二次快闪 - 灭，然后进入长灭状态
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    intranet_flash_step = 4;
                    intranet_wait_counter = 0; // 重置等待计数器
                    ESP_LOGD(TAG, "内网模式%sLED: 第二次快闪灭，开始长灭", is_wifi_active ? "WiFi" : "以太网");
                    break;
                case 4: // 长灭状态，等待1.6秒（16个100ms周期）后重新开始
                    // 确保LED在长灭期间保持关闭状态
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    intranet_wait_counter++;
                    if (intranet_wait_counter >= 16) { // 16 * 100ms = 1.6秒
                        intranet_flash_step = 0; // 重新开始循环
                        intranet_wait_counter = 0; // 重置等待计数器
                        ESP_LOGD(TAG, "内网模式%sLED: 长灭结束，重新开始循环", is_wifi_active ? "WiFi" : "以太网");
                    }
                    break;
                default:
                    // 异常状态，重置到初始状态
                    intranet_flash_step = 0;
                    intranet_wait_counter = 0;
                    if (is_wifi_active) {
                        LED_WIFI_OFF();
                    } else {
                        LED_LAN_OFF();
                    }
                    ESP_LOGW(TAG, "内网模式%sLED: 异常状态，重置到初始状态", is_wifi_active ? "WiFi" : "以太网");
                    break;
            }
        } else {
            // 外网模式：常亮
            if (is_wifi_active) {
                LED_WIFI_ON();
                ESP_LOGD(TAG, "外网模式WiFi LED常亮");
            } else {
                LED_LAN_ON();
                ESP_LOGD(TAG, "外网模式以太网LED常亮");
            }
            intranet_flash_step = 0; // 重置内网闪烁步骤
            intranet_wait_counter = 0; // 重置内网等待计数器
        }
    } else {
        // 网络断开时LED长灭
        if (is_wifi_active) {
            LED_WIFI_OFF();
            ESP_LOGD(TAG, "WiFi网络断开，WiFi LED长灭");
        } else {
            LED_LAN_OFF();
            ESP_LOGD(TAG, "以太网断开，以太网LED长灭");
        }
        intranet_flash_step = 0; // 重置内网闪烁步骤
        intranet_wait_counter = 0; // 重置内网等待计数器
    }
}

// 初始化按键扫描定时器
esp_err_t init_key_timer(void) {
    const char *TAG = "init_key_timer";

    // 创建按键扫描定时器，10ms扫描一次
    esp_timer_create_args_t key_timer_args = {
        .callback = &key_timer_callback,
        .name = "key_timer",
        .dispatch_method = ESP_TIMER_TASK,  // 使用专用的定时器任务
        .skip_unhandled_events = true       // 跳过未处理的事件，避免积压
    };

    esp_err_t err = esp_timer_create(&key_timer_args, &key_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法创建按键定时器: %s", esp_err_to_name(err));
        return err;
    }

    // 启动按键定时器，10ms周期
    err = esp_timer_start_periodic(key_timer, 10 * 1000); // 10ms
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法启动按键定时器: %s", esp_err_to_name(err));
        esp_timer_delete(key_timer);
        key_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "按键定时器初始化成功，10ms扫描周期");
    return ESP_OK;
}



// 新增初始化LED闪烁定时器的函数，使用独立的定时器任务
esp_err_t init_led_timer(void) {
    const char *TAG = "init_led_timer";

    // 如果LED定时器已存在，先停止并删除
    if (time_sync_led_timer != NULL) {
        ESP_LOGI(TAG, "清理已存在的LED闪烁定时器");
        esp_timer_stop(time_sync_led_timer);
        esp_timer_delete(time_sync_led_timer);
        time_sync_led_timer = NULL;
    }

    // 创建LED闪烁定时器，使用独立的定时器任务，避免被心跳检查阻塞
    esp_timer_create_args_t led_timer_args = {
        .callback = &time_sync_led_callback,
        .name = "time_sync_led_timer",
        .dispatch_method = ESP_TIMER_TASK,  // 使用专用的定时器任务，确保不被阻塞
        .skip_unhandled_events = true       // 跳过未处理的事件，避免积压
    };

    esp_err_t err = esp_timer_create(&led_timer_args, &time_sync_led_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法创建LED闪烁定时器: %s", esp_err_to_name(err));
        return err;
    }

    // 启动LED闪烁定时器，100ms周期，支持内网快闪
    err = esp_timer_start_periodic(time_sync_led_timer, 100 * 1000); // 100ms
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法启动LED闪烁定时器: %s", esp_err_to_name(err));
        esp_timer_delete(time_sync_led_timer);
        time_sync_led_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "LED闪烁定时器初始化成功，100ms扫描周期，使用独立定时器任务防止阻塞");
    return ESP_OK;
}

// 初始化所有定时器任务，替代原来的FreeRTOS任务
esp_err_t init_timer_tasks(void) {
    esp_timer_create_args_t timer_args;
    esp_err_t err;

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

    // 基本功能定时器（按键、I2C、LED、内存监控）已在app_main中启动
    // 这里只初始化网络相关的定时器

    // 清理已存在的网络相关定时器，避免重复创建
    if (heartbeat_timer != NULL) {
        ESP_LOGI(TAG, "清理已存在的心跳定时器");
        esp_timer_stop(heartbeat_timer);
        esp_timer_delete(heartbeat_timer);
        heartbeat_timer = NULL;
    }

    // 初始化心跳定时器，使用一次性启动避免周期冲突
    timer_args = (esp_timer_create_args_t) {
        .callback = &heartbeat_timer_callback,
        .name = "heartbeat_timer"
    };
    err = esp_timer_create(&timer_args, &heartbeat_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法创建心跳定时器: %s", esp_err_to_name(err));
        return err;
    }

    // 启动一次性定时器，让回调函数自己设置周期性模式
    err = esp_timer_start_once(heartbeat_timer, 3000 * 1000); // 3秒后首次触发（更快启动）
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法启动心跳定时器: %s", esp_err_to_name(err));
        // 清理资源
        esp_timer_delete(heartbeat_timer);
        heartbeat_timer = NULL;
        return err;
    }

    ESP_LOGI(TAG, "心跳定时器将在3秒后首次触发，然后切换到15秒周期模式");

    // 创建心跳看门狗定时器
    static esp_timer_handle_t heartbeat_watchdog_timer = NULL;
    if (heartbeat_watchdog_timer != NULL) {
        ESP_LOGI(TAG, "清理已存在的心跳看门狗定时器");
        esp_timer_stop(heartbeat_watchdog_timer);
        esp_timer_delete(heartbeat_watchdog_timer);
        heartbeat_watchdog_timer = NULL;
    }

    timer_args = (esp_timer_create_args_t) {
        .callback = &heartbeat_watchdog_callback,
        .name = "heartbeat_watchdog"
    };
    err = esp_timer_create(&timer_args, &heartbeat_watchdog_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(heartbeat_watchdog_timer, 30000 * 1000); // 每30秒检查一次
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "心跳看门狗定时器已启动，每30秒检查一次");
        } else {
            ESP_LOGE(TAG, "无法启动心跳看门狗定时器: %s", esp_err_to_name(err));
            esp_timer_delete(heartbeat_watchdog_timer);
            heartbeat_watchdog_timer = NULL;
        }
    } else {
        ESP_LOGE(TAG, "无法创建心跳看门狗定时器: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "网络相关定时器任务初始化完成");

    // 执行网络诊断以检查连接状态
    ESP_LOGI(TAG, "执行初始网络诊断...");
    network_diagnostics();

    return ESP_OK;
}

// 停止所有定时器任务
esp_err_t stop_timer_tasks(void) {
    esp_err_t err;
    esp_err_t result = ESP_OK;  // 跟踪整体结果

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

    if (i2c_timer) {
        err = esp_timer_stop(i2c_timer);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "无法停止I2C传感器读取定时器: %s", esp_err_to_name(err));
            result = err;
        }
        err = esp_timer_delete(i2c_timer);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "无法删除I2C传感器读取定时器: %s", esp_err_to_name(err));
            result = err;
        }
        i2c_timer = NULL;
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

// 处理重置和重启的任务
static void reset_restart_task(void *pvParameters) {
    if (reset_requested) {
        printf("执行完全重置...\n");

        // 先停止LED闪烁定时器，防止定时器回调覆盖LED状态
        if (time_sync_led_timer != NULL) {
            esp_err_t err = esp_timer_stop(time_sync_led_timer);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "停止LED定时器失败: %s", esp_err_to_name(err));
            } else {
                ESP_LOGI(TAG, "LED闪烁定时器已停止");
            }
        }

        // 重置配置
        ESP_ERROR_CHECK(nvs_safe_reset());
        vTaskDelay(100 / portTICK_PERIOD_MS);

        // 所有灯全亮一秒
        LED_DAT_ON();
        LED_WIFI_ON();
        LED_LAN_ON();
        vTaskDelay(1000 / portTICK_PERIOD_MS);

        // 所有灯全灭
        LED_DAT_OFF();
        LED_WIFI_OFF();
        LED_LAN_OFF();
        vTaskDelay(100 / portTICK_PERIOD_MS);
    } else if (restart_requested) {
        printf("执行重启...\n");
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }

    // 重置标志
    reset_requested = false;
    restart_requested = false;
    reset_task_running = false;

    // 重启设备
    esp_restart();
}

// 按键定时器回调函数
static void key_timer_callback(void *arg) {
    static int press_time = 0;

    if (gpio_get_level(KEY) == 0) {  // 按键按下
        if (KeyState == 0) {
            KeyState = 1;
            press_time = 0;
        } else {
            press_time++;

            if (press_time >= 500) {  // 长按超过5秒 (10ms×500=5000ms)
                if (!reset_task_running) {
                    printf("RESET ALL!\n");
                    // 设置复位标志
                    reset_requested = true;
                    press_time = 0;  // 防止重复触发

                    // 创建任务来处理重置（在任务中会闪烁LED）
                    BaseType_t task_created = xTaskCreate(reset_restart_task, "reset_task", 2048, NULL, 5, NULL);
                    if (task_created == pdPASS) {
                        reset_task_running = true;
                    } else {
                        reset_requested = false;
                        printf("创建reset_task失败: %ld\n", (long)task_created);
                    }
                }
            }
        }
    } else {
        if (KeyState == 1) {
            // 如果reset任务正在执行，不处理按键释放事件，避免干扰LED显示
            if (reset_task_running) {
                printf("Reset任务执行中，忽略按键释放\n");
                return;
            }
            
            if (press_time > 0 && press_time < 100) {  // 按下1秒以内释放 (10ms×100=1000ms)
                printf("RESTART!\n");
                // 创建任务来处理重启
                if (!reset_task_running) {
                    restart_requested = true;
                    BaseType_t task_created = xTaskCreate(reset_restart_task, "restart_task", 2048, NULL, 5, NULL);
                    if (task_created == pdPASS) {
                        reset_task_running = true;
                    } else {
                        restart_requested = false;
                        printf("创建restart_task失败: %ld\n", (long)task_created);
                    }
                }
            } else if (press_time >= 100 && press_time < 500) {  // 按下1-5秒释放
                printf("按键释放，未执行任何操作\n");
                // 1-5秒释放不做任何处理，不改变LED状态
            }
            KeyState = 0;
            press_time = 0;
        }
    }
}



// 内存监控定时器回调函数
void free_timer_callback(void *arg) {
    printf("=================================================\r\n");
    printf("\r\nremaining memory = %zu,minimum memory = %zu\r\n",
           xPortGetFreeHeapSize(), xPortGetMinimumEverFreeHeapSize());

    // 获取自系统启动以来的时间（单位：微秒）
    int64_t time_since_boot = esp_timer_get_time();
    // 转换为天、小时、分钟
    int days = time_since_boot / (1000000LL * 60 * 60 * 24);
    int hours = (time_since_boot % (1000000LL * 60 * 60 * 24)) / (1000000LL * 60 * 60);
    int minutes = (time_since_boot % (1000000LL * 60 * 60)) / (1000000LL * 60);

    if (days > 0) {
        printf("Time since boot: %d days, %d hours, %d minutes\n", days, hours, minutes);
    } else if (hours > 0) {
        printf("Time since boot: %d hours, %d minutes\n", hours, minutes);
    } else {
        printf("Time since boot: %d minutes\n", minutes);
    }

    esp_reset_reason_t reason = esp_reset_reason();
    printf("Reset reason: %s\n", reset_reason_to_string(reason));

    // 处理AP模式自动关闭（10分钟后）
    if (!ap_closed) {
        int64_t current_time = esp_timer_get_time();
        if (ap_start_time == 0) {
            ap_start_time = current_time;
        } else if ((current_time - ap_start_time) > (10 * 60 * 1000000LL)) { // 10分钟
            printf("AP timeout reached, closing AP mode...\n");
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ap_closed = true;
            printf("AP closed successfully\n");
        }
    }

    printf("=================================================\r\n");
}

esp_err_t _timer_http_event_handler(esp_http_client_event_t *evt) {
    static char *timer_output_buffer = NULL; // 静态缓冲区，避免与其他函数的静态变量冲突
    static int timer_output_len = 0;         // 静态长度，避免与其他函数的静态变量冲突
    static bool disconnected = false;        // 标记是否已经收到过断开连接事件

    switch (evt->event_id) {
    case HTTP_EVENT_ERROR:
        ESP_LOGE(TAG, "HTTP_EVENT_ERROR");
        // 确保在错误事件中释放资源，但不释放用户提供的缓冲区
        if (timer_output_buffer != NULL && evt->user_data != timer_output_buffer) {
            free(timer_output_buffer);
            timer_output_buffer = NULL;
            timer_output_len = 0;
        }
        break;
    case HTTP_EVENT_ON_CONNECTED:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
        // 每次新连接时重置断开标志
        disconnected = false;
        // 连接时清理旧缓冲区，确保每次请求都使用新的缓冲区
        // 但不释放用户提供的缓冲区
        if (timer_output_buffer != NULL && evt->user_data != timer_output_buffer) {
            free(timer_output_buffer);
            timer_output_buffer = NULL;
            timer_output_len = 0;
        }
        break;
    case HTTP_EVENT_HEADER_SENT:
        ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
        break;
    case HTTP_EVENT_ON_HEADER:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
        break;
    case HTTP_EVENT_ON_DATA:
        ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
        if (evt->data_len > 0) {
            // 如果用户数据直接可用（传递了一个预分配的缓冲区）
            if (evt->user_data) {
                // 确保不会溢出用户缓冲区
                int copy_len = MIN(evt->data_len, MAX_HTTP_OUTPUT_BUFFER - timer_output_len - 1);
                if (copy_len > 0) {
                    memcpy((char*)evt->user_data + timer_output_len, evt->data, copy_len);
                    timer_output_len += copy_len;
                    // 确保字符串以NULL结尾
                    ((char*)evt->user_data)[timer_output_len] = '\0';
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
                    memcpy(timer_output_buffer + timer_output_len, evt->data, evt->data_len);
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
            ESP_LOGI(TAG, "收到的数据: %.*s", evt->data_len, (char*)evt->data);
        }
        break;
    case HTTP_EVENT_ON_FINISH:
        ESP_LOGI(TAG, "HTTP_EVENT_ON_FINISH");
        // 重置输出长度，但不立即释放缓冲区，等待断开连接时再释放
        timer_output_len = 0;
        break;
    case HTTP_EVENT_DISCONNECTED:
        // 防止重复处理断开事件
        if (disconnected) {
            ESP_LOGW(TAG, "忽略重复的断开连接事件");
            return ESP_OK;
        }

        ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
        disconnected = true; // 标记已收到断开事件

        int mbedtls_err = 0;
        esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)evt->data, &mbedtls_err, NULL);
        if (err != 0) {
            ESP_LOGE(TAG, "TLS错误: ESP错误码=0x%x, mbedTLS错误码=0x%x", err, mbedtls_err);
            ESP_LOGE(TAG, "TLS错误详情：%s", esp_err_to_name(err));
        }

        // 在释放缓冲区之前等待较长时间，确保TCP处理完成
        vTaskDelay(100 / portTICK_PERIOD_MS);

        // 确保安全释放缓冲区
        if (timer_output_buffer != NULL) {
            // 检查是否安全释放内存
            bool should_free = true;

            // 只有在buffer不是用户数据时才释放
            if (evt->user_data == timer_output_buffer) {
                ESP_LOGI(TAG, "跳过释放用户提供的缓冲区");
                should_free = false;
            }

            if (should_free) {
                ESP_LOGI(TAG, "释放动态分配的输出缓冲区");
                free(timer_output_buffer);
            }

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

// 平台连接定时器回调函数
void platform_timer_callback(void *arg) {
    // 只执行两次
    if (platform_send_count < 2) {
        // 检查网络连接状态
        extern bool is_network_connected;
        extern bool is_intranet_mode;

        if (!is_network_connected) {
            ESP_LOGD(TAG, "网络未连接，延后平台连接请求");  // 降低日志级别
            return;
        }

        // 在内网模式下，跳过平台连接请求
        if (is_intranet_mode) {
            ESP_LOGD(TAG, "内网模式，跳过平台连接请求");
            platform_send_count = 2; // 标记为已完成，停止定时器

            // 停止定时器
            if (platform_timer) {
                esp_err_t err = esp_timer_stop(platform_timer);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "无法停止平台连接定时器: %s", esp_err_to_name(err));
                }
                err = esp_timer_delete(platform_timer);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "无法删除平台连接定时器: %s", esp_err_to_name(err));
                }
                platform_timer = NULL;
            }
            return;
        }

        // 检查客户端HTTP是否忙，如果忙则延后平台请求
        if (client_http_busy) {
            ESP_LOGD(TAG, "客户端HTTP正忙，延后平台连接请求");  // 降低日志级别
            return; // 下次定时器触发时再尝试
        }

        // 尝试获取HTTP互斥量，使用非阻塞方式
        if (http_mutex != NULL) {
            if (xSemaphoreTake(http_mutex, 0) != pdTRUE) {
                ESP_LOGD(TAG, "HTTP资源正忙，平台连接请求稍后重试");  // 降低日志级别
                return; // 如果无法获取互斥量，直接返回，下次定时器触发时再尝试
            }
        }

        // 调用平台连接函数，保持与原始任务功能一致
        http_rest_with_url_platform("https://platform.likong-iot.com/api/v1/device_link", "");

        // 释放互斥量
        if (http_mutex != NULL) {
            xSemaphoreGive(http_mutex);
        }

        platform_send_count++;

        if (platform_send_count == 1) {
            ESP_LOGD(TAG, "平台第一次请求已发送，1秒后进行第二次尝试");  // 降低日志级别
            // 1秒后会再次触发回调
        } else {
            ESP_LOGD(TAG, "平台第二次请求已完成，定时器将停止");  // 降低日志级别
            // 停止定时器
            if (platform_timer) {
                esp_err_t err = esp_timer_stop(platform_timer);
                if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(TAG, "无法停止平台连接定时器: %s", esp_err_to_name(err));
                }
                err = esp_timer_delete(platform_timer);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "无法删除平台连接定时器: %s", esp_err_to_name(err));
                }
                platform_timer = NULL;
            }
        }
    }
}

// 心跳状态监控和自动恢复
void heartbeat_watchdog_callback(void *arg) {
    static int heartbeat_check_count = 0;
    static int64_t last_heartbeat_success_time = 0;
    static bool watchdog_enabled = false;

    const char *TAG = "HEARTBEAT_WATCHDOG";
    extern bool is_network_connected;
    extern char got_ip_addrs[16];
    int64_t current_time = esp_timer_get_time() / 1000;

    heartbeat_check_count++;

    // 检查是否有有效IP地址
    bool has_valid_ip = (strlen(got_ip_addrs) > 0 &&
                        strcmp(got_ip_addrs, "0.0.0.0") != 0 &&
                        strcmp(got_ip_addrs, "---") != 0);

    if (!has_valid_ip) {
        ESP_LOGD(TAG, "没有有效IP地址，看门狗暂停");
        watchdog_enabled = false;
        return;
    }

    if (!watchdog_enabled) {
        watchdog_enabled = true;
        last_heartbeat_success_time = current_time;
        ESP_LOGI(TAG, "心跳看门狗已启用，IP: %s", got_ip_addrs);
        return;
    }

    // 检查心跳定时器是否正常运行
    if (heartbeat_timer == NULL) {
        ESP_LOGW(TAG, "心跳定时器不存在，尝试重新创建");

        esp_timer_create_args_t timer_args = {
            .callback = &heartbeat_timer_callback,
            .name = "heartbeat_timer"
        };
        esp_err_t err = esp_timer_create(&timer_args, &heartbeat_timer);
        if (err == ESP_OK) {
            err = esp_timer_start_once(heartbeat_timer, 2000 * 1000); // 2秒后开始
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "心跳定时器重新创建成功");
                last_heartbeat_success_time = current_time;
            } else {
                ESP_LOGE(TAG, "心跳定时器启动失败: %s", esp_err_to_name(err));
            }
        } else {
            ESP_LOGE(TAG, "心跳定时器创建失败: %s", esp_err_to_name(err));
        }
        return;
    }

    // 检查网络连接状态
    if (!is_network_connected && has_valid_ip) {
        ESP_LOGW(TAG, "有IP但网络状态为断开，强制更新网络状态");
        force_network_status_update();
        last_heartbeat_success_time = current_time;
        return;
    }

    // 检查是否长时间没有成功的心跳（超过60秒）
    if (current_time - last_heartbeat_success_time > 60000) {
        ESP_LOGW(TAG, "心跳长时间未成功 (%.1f秒)，尝试重启心跳系统",
                 (current_time - last_heartbeat_success_time) / 1000.0);

        // 停止并重启心跳定时器
        esp_err_t stop_err = esp_timer_stop(heartbeat_timer);
        if (stop_err == ESP_OK || stop_err == ESP_ERR_INVALID_STATE) {
            esp_err_t start_err = esp_timer_start_once(heartbeat_timer, 1000 * 1000); // 1秒后重启
            if (start_err == ESP_OK) {
                ESP_LOGI(TAG, "心跳定时器已重启");
                last_heartbeat_success_time = current_time;
            } else {
                ESP_LOGE(TAG, "心跳定时器重启失败: %s", esp_err_to_name(start_err));
            }
        }
    }

    // 每10次检查输出一次状态
    if (heartbeat_check_count % 10 == 0) {
        ESP_LOGI(TAG, "心跳看门狗状态: 网络=%s, IP=%s, 上次成功: %.1f秒前",
                 is_network_connected ? "连接" : "断开",
                 got_ip_addrs,
                 (current_time - last_heartbeat_success_time) / 1000.0);
    }
}
