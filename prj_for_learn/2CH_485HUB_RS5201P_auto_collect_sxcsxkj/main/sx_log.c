/*
 * @Author: Orion
 * @Date: 2024-12-20 15:40:52
 * @LastEditors: Orion
 * @LastEditTime: 2025-01-17 15:20:13
 * @Description:
 *
 */
#include "sx_log.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "sx_web_server.h"
#define SIMPLE_LOG_BUFFER_SIZE 4096
#define SIMPLE_LOG_TEMP_BUFFER_SIZE 512

// 日志缓冲区和位置指针
static char *simple_log_buffer = NULL;
static char *simple_log_temp_buffer = NULL;
static size_t simple_log_position = 0;

// 互斥锁
static SemaphoreHandle_t simple_log_mutex = NULL;

static void *log_psram_calloc(size_t count, size_t size)
{
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static bool sx_log_ensure_buffers(void)
{
    if (simple_log_buffer == NULL) {
        simple_log_buffer = log_psram_calloc(1, SIMPLE_LOG_BUFFER_SIZE);
    }
    if (simple_log_temp_buffer == NULL) {
        simple_log_temp_buffer = log_psram_calloc(1, SIMPLE_LOG_TEMP_BUFFER_SIZE);
    }
    return simple_log_buffer != NULL && simple_log_temp_buffer != NULL;
}


// 系统日志输出函数 - 负责标准串口输出和详细日志记录
static int log_output_func(const char* fmt, va_list args) {


    // 直接输出到标准串口
    return vprintf(fmt, args);
}

// 初始化日志系统
void sx_log_init(void) {

    if (simple_log_mutex == NULL) {
        simple_log_mutex = xSemaphoreCreateMutex();
    }

    if (!sx_log_ensure_buffers()) {
        ESP_LOGE("SX_LOG", "Failed to allocate log buffers");
        return;
    }

    // 清空缓冲区
    memset(simple_log_buffer, 0, SIMPLE_LOG_BUFFER_SIZE);
    memset(simple_log_temp_buffer, 0, SIMPLE_LOG_TEMP_BUFFER_SIZE);
    simple_log_position = 0;

    // 设置系统日志输出函数
    esp_log_set_vprintf(log_output_func);
}



// 获取简明日志缓冲区内容
char* sx_log_get_simple_buffer(void) {
    if (simple_log_mutex == NULL || !sx_log_ensure_buffers()) return NULL;

    char* result = NULL;
    if (xSemaphoreTake(simple_log_mutex, portMAX_DELAY) == pdTRUE) {
        if (simple_log_position > 0) {
            result = strdup(simple_log_buffer);
        }
        xSemaphoreGive(simple_log_mutex);
    }
    return result;
}

// 清除所有日志
void sx_log_clear(void) {


    // 清除简明日志
    if (simple_log_mutex && sx_log_ensure_buffers()) {
        if (xSemaphoreTake(simple_log_mutex, portMAX_DELAY) == pdTRUE) {
            memset(simple_log_buffer, 0, SIMPLE_LOG_BUFFER_SIZE);
            simple_log_position = 0;
            xSemaphoreGive(simple_log_mutex);
        }
    }
}

// 写入详细日志
void sx_log_write(const char* format, ...) {
    va_list args;
    va_start(args, format);

    // 直接使用系统日志输出函数
    log_output_func(format, args);

    va_end(args);
}

void sx_log_write_simple(const char* format, ...) {
    if (simple_log_mutex == NULL || !sx_log_ensure_buffers()) return;

    // 获取互斥锁
    if (xSemaphoreTake(simple_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    char *temp_buffer = simple_log_temp_buffer;
    va_list args;
    va_start(args, format);

    // 格式化日志消息
    int len = vsnprintf(temp_buffer, SIMPLE_LOG_TEMP_BUFFER_SIZE, format, args);
    va_end(args);

    if (len > 0 && len < SIMPLE_LOG_TEMP_BUFFER_SIZE) {
        // 确保字符串以换行符结束
        if (temp_buffer[len-1] != '\n') {
            if (len < SIMPLE_LOG_TEMP_BUFFER_SIZE - 1) {
                temp_buffer[len] = '\n';
                temp_buffer[len+1] = '\0';
                len++;
            }
        }




        // 处理简明日志缓冲区
        if (simple_log_position + len >= SIMPLE_LOG_BUFFER_SIZE) {
            // 查找第一个换行符的位置
            char *first_newline = strchr(simple_log_buffer, '\n');
            if (first_newline != NULL) {
                size_t remove_len = first_newline - simple_log_buffer + 1;
                memmove(simple_log_buffer,
                       first_newline + 1,
                       simple_log_position - remove_len);
                simple_log_position -= remove_len;
            } else {
                simple_log_position = 0;
            }
        }

        // 添加到简明日志缓冲区
        if (simple_log_position + len < SIMPLE_LOG_BUFFER_SIZE) {
            memcpy(simple_log_buffer + simple_log_position, temp_buffer, len);
            simple_log_position += len;
            simple_log_buffer[simple_log_position] = '\0';
        }

        // 输出到标准串口
        printf("%s", temp_buffer);
    }

    xSemaphoreGive(simple_log_mutex);
}
