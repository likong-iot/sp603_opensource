/*
 * @Author: Orion
 * @Date: 2024-05-08 16:08:44
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-19 11:19:28
 * @FilePath: \SERIIAL_SERVER_P\main\include\sx_async_uart.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sx_async_uart.h"  // 直接包含完整定义

// 手动缓存配置最大条目数
#define MODBUS_CACHE_MAX_ITEMS 256
#define MODBUS_CACHE_BLOB_VERSION 1

// Modbus缓存配置项结构体
typedef struct {
    bool enabled;           // 启用状态字段
    char slave_addr[8];     // 真实从机地址
    char function_code[8];
    char register_addr[8];  // 真实寄存器地址
    char register_num[8];
    char timeout[8];
    char data_format[16];
    char interval_time[8];
    char report_format[8];
    char baud_rate[8];
    char data_bit[8];
    char stop_bit[8];
    char check_bit[8];
    char mapped_slave_addr[8];    // 映射从机地址
    char mapped_register_addr[8]; // 映射寄存器地址
} ModbusCacheItemConfig;

typedef struct __attribute__((packed)) {
    uint8_t enabled;
    uint8_t slave_addr;
    uint8_t function_code;
    uint16_t register_addr;
    uint16_t register_num;
    uint8_t mapped_slave_addr;
    uint16_t mapped_register_addr;
    uint32_t timeout_ms;
    uint32_t interval_ms;
    uint32_t baud_rate;
    uint8_t data_bit;
    uint8_t check_bit;
    uint8_t stop_bits_x2;
} ModbusCacheItemBlob;

// Modbus缓存任务配置结构体
typedef struct {
    int items_count;
    ModbusCacheItemConfig *items;
} ModbusCacheTaskConfig;

// 手动缓存配置参数（融合策略已移除）

// 真实数据表项 - 按从机地址融合存储
typedef struct {
    uint8_t slave_addr;           // 真实从机地址
    uint8_t function_code;        // 功能码
    uint16_t start_register;      // 起始寄存器地址（融合后）
    uint16_t register_count;      // 寄存器数量（融合后的总数量）
    uint8_t *data;               // 实际数据缓存
    size_t data_length;          // 数据长度
    uint64_t timestamp;          // 最后更新时间
    bool data_valid;             // 数据有效性
} RealDataEntry;

// 映射数据表项 - 虚拟地址到真实数据的映射
typedef struct {
    uint8_t mapped_slave_addr;    // 映射从机地址
    uint16_t mapped_register;     // 映射寄存器地址
    uint16_t register_count;      // 寄存器数量
    
    // 指向真实数据的引用
    RealDataEntry *real_data_ref; // 指向真实数据表项
    uint16_t real_offset;         // 在真实数据中的偏移（寄存器偏移）
} MappedDataEntry;

// 原有的缓存管理结构体（用于自动模式）
typedef struct {
    RealDataEntry *real_data_table;     // 真实数据表
    int real_data_count;                 // 真实数据表项数量
    
    MappedDataEntry *mapped_data_table;  // 映射数据表  
    int mapped_data_count;               // 映射数据表项数量
    
    SemaphoreHandle_t cache_mutex;       // 缓存访问互斥锁
} ModbusCacheManager;

// 新的手动缓存双表结构体
typedef struct {
    uint8_t real_slave_addr;        // 真实从机地址
    uint8_t function_code;          // 功能码(通常是03)
    uint16_t start_register;        // 融合后的起始寄存器
    uint16_t register_count;        // 融合后的寄存器数量
    uint8_t *data_buffer;           // 数据缓冲区
    size_t data_length;             // 数据长度(字节)
    uint64_t last_update_time;      // 最后更新时间
    uint32_t poll_interval_ms;      // 轮询间隔
    uint32_t timeout_ms;            // 超时时间
    channel_uart_config_t uart_config; // UART配置
    bool data_valid;                // 数据有效性
    bool data_healthy;              // 连续成功标记，三次失败将置false
    uint8_t fail_count;             // 连续失败计数
} ManualRealDataTable_t;

typedef struct {
    uint8_t virtual_slave_addr;     // 虚拟从机地址
    uint16_t virtual_start_reg;     // 虚拟起始寄存器
    uint16_t register_count;        // 寄存器数量
    // 指向真实数据表的引用
    ManualRealDataTable_t *real_table_ref;
    uint16_t real_offset;           // 在真实数据中的偏移(寄存器偏移)
} ManualMappingTable_t;

typedef struct {
    ManualRealDataTable_t *real_tables;     // 真实数据表数组
    int real_table_count;                   // 真实表项数量
    ManualMappingTable_t *mapping_tables;   // 映射表数组  
    int mapping_table_count;                // 映射表项数量
    SemaphoreHandle_t mutex;                // 互斥锁
    TaskHandle_t polling_task;              // 轮询任务句柄
    TaskHandle_t response_task;             // 响应任务句柄
    bool config_updated;                    // 配置更新标志
} ManualCacheManager_t;

// 请求队列项
typedef struct {
    uint8_t request_data[64];               // 请求数据
    int request_len;                        // 请求长度
    int source_channel;                     // 来源通道(1或2)
    uint64_t timestamp;                     // 请求时间戳
} RequestQueueItem_t;

// 函数声明
void stop_modbus_cache_tasks(void);
esp_err_t start_modbus_cache_task(ModbusCacheTaskConfig *config);
uint16_t calculate_crc(uint8_t *data, size_t length);
char* convert_modbus_data(uint8_t *data, size_t length, const char *format);

// 缓存管理函数
esp_err_t init_cache_manager(ModbusCacheTaskConfig *config);
void destroy_cache_manager(void);
esp_err_t update_real_data(uint8_t slave_addr, uint16_t start_register, uint16_t register_count, uint8_t *response_data, size_t data_length);
esp_err_t get_mapped_data(uint8_t mapped_slave_addr, uint16_t mapped_register_addr, 
                         uint16_t register_count, uint8_t *output_data, size_t *output_length);
void print_cache_status(void);

// 虚拟从机响应功能
void virtual_slave_response_task(void *pvParameter);
esp_err_t start_virtual_slave_response_task(void);
void stop_virtual_slave_response_task(void);
esp_err_t build_modbus_response(uint8_t slave_addr, uint8_t function_code, 
                               uint16_t register_addr, uint16_t register_count,
                               uint8_t *response_buffer, size_t *response_length);

// 诊断和调试功能
void print_modbus_cache_status(void);
int get_modbus_items_count_debug(nvs_handle_t storage_handle);
void print_current_work_mode(void);
void test_modbus_cache_config(void);

// 新的统一初始化函数
esp_err_t modbus_cache_init(void);

// 缓存系统清理和重新初始化函数（用于模式切换）；不需要，故注释掉
// void cleanup_and_reinit_modbus_cache(void);

// 子模式初始化函数
esp_err_t init_auto_intelligent_cache(void);
esp_err_t init_manual_polling_cache(nvs_handle_t storage_handle);
void stop_manual_cache_tasks(void);

// 保留占位：无融合策略可配置

// 自动智能缓存任务函数
void auto_intelligent_cache_task(void *pvParameter);

// 保留原有函数（兼容性）
esp_err_t init_modbus_cache_auto_polling(void);
#ifdef __cplusplus
}
#endif
