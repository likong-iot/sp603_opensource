/*
 * @Author: Orion
 * @Date: 2025-01-28 00:00:00
 * @LastEditors: Orion
 * @LastEditTime: 2025-01-28 00:00:00
 * @FilePath: \2CH_485HUB-V1.0\main\include\sx_modbus_queue.h
 * @Description: Modbus过滤透传排队模式头文件
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#ifndef SX_MODBUS_QUEUE_H
#define SX_MODBUS_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Modbus地址范围常量
#define MODBUS_MIN_SLAVE_ADDR    1    // Modbus从站地址最小值
#define MODBUS_MAX_SLAVE_ADDR    247  // Modbus从站地址最大值
#define MODBUS_MAX_ADDR_RANGES   247  // 最大地址范围数量（等于最大从机数量）
#define MODBUS_MAX_SLAVE_MAPPINGS 247 // 最大从机地址映射数量（等于最大从机数量）

// Modbus地址范围结构体
typedef struct {
    uint8_t slave_id;     // 从机号（设备地址，0表示所有从机）
    uint16_t start_addr;  // 起始寄存器地址 (0-65535)
    uint16_t end_addr;    // 结束寄存器地址 (0-65535)
} modbus_queue_addr_range_t;

// Modbus从机地址映射结构体
typedef struct {
    uint8_t virtual_addr;   // 虚拟从机地址 (1-247)
    uint8_t real_addr;      // 真实从机地址 (1-247)
    bool enabled;           // 是否启用此映射
} modbus_slave_mapping_t;

// Modbus过滤模式枚举
typedef enum {
    MODBUS_QUEUE_WHITELIST = 1, // 白名单模式
    MODBUS_QUEUE_BLACKLIST = 2  // 黑名单模式
} modbus_queue_filter_mode_t;

// Modbus队列状态枚举
typedef enum {
    MODBUS_QUEUE_IDLE = 0,      // 空闲状态
    MODBUS_QUEUE_WAITING        // 等待CH3响应
} modbus_queue_state_t;

// Modbus队列状态结构体
typedef struct {
    int queue_size;             // 当前队列大小
    int max_queue_size;         // 最大队列大小
    modbus_queue_state_t current_state; // 当前状态
    int waiting_index;          // 等待响应的请求索引
    bool is_running;            // 队列是否运行
    bool config_loaded;         // 配置是否已加载
    uint64_t wait_elapsed_ms;   // 等待时间（毫秒）
} modbus_queue_status_t;

// Modbus队列配置结构体
typedef struct {
    bool enabled;                    // 是否启用过滤
    modbus_queue_filter_mode_t mode; // 过滤模式
    modbus_queue_addr_range_t addr_ranges[MODBUS_MAX_ADDR_RANGES]; // 地址范围数组
    int range_count;                 // 地址范围数量
    uint32_t queue_timeout_ms;       // 队列超时时间(ms)
    uint8_t max_queue_size;          // 最大队列大小
    
    // 从机地址映射配置
    bool mapping_enabled;            // 是否启用从机地址映射
    modbus_slave_mapping_t slave_mappings[MODBUS_MAX_SLAVE_MAPPINGS]; // 从机地址映射数组
    int mapping_count;               // 映射条目数量
} modbus_queue_config_t;

// 函数声明

/**
 * @brief 初始化Modbus过滤透传排队功能
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_init(void);

/**
 * @brief 检查从机号和寄存器地址是否通过过滤
 * @param slave_id 从机号（设备地址）
 * @param register_addr 寄存器地址
 * @return true 通过过滤，false 被过滤掉
 */
bool modbus_queue_check_address(uint8_t slave_id, uint16_t register_addr);

/**
 * @brief 加载Modbus队列配置
 * @param config 配置结构体指针
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_load_config(modbus_queue_config_t *config);

/**
 * @brief 保存Modbus队列配置
 * @param config 配置结构体指针
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_save_config(const modbus_queue_config_t *config);

/**
 * @brief 重置Modbus队列配置为默认值
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_reset_config(void);

/**
 * @brief 验证Modbus地址范围是否有效
 * @param range 地址范围结构体指针
 * @return true 有效，false 无效
 */
bool modbus_queue_validate_range(const modbus_queue_addr_range_t *range);

/**
 * @brief 验证Modbus队列配置是否有效
 * @param config 配置结构体指针
 * @return ESP_OK 有效，其他值表示错误
 */
esp_err_t modbus_queue_validate_config(const modbus_queue_config_t *config);

/**
 * @brief 启动Modbus过滤透传排队功能
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_start(void);

/**
 * @brief 停止Modbus过滤透传排队功能
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_stop(void);

/**
 * @brief 检查Modbus过滤透传排队功能是否正在运行
 * @return true 正在运行，false 未运行
 */
bool modbus_queue_is_running(void);

/**
 * @brief 初始化Modbus队列自动模式
 * 从NVS读取配置并自动启动
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t init_modbus_queue_auto_mode(void);

/**
 * @brief 启动Modbus过滤透传排队模式主任务
 */
void sx_modbus_queue_init(void);

/**
 * @brief Modbus过滤透传队列主任务
 * @param pvParameter 任务参数（未使用）
 */
void modbus_queue_task(void *pvParameter);

/**
 * @brief 处理Modbus数据包，进行过滤和排队
 * @param data 数据包指针
 * @param length 数据包长度
 * @param source_channel 源通道
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_process_data(const uint8_t *data, size_t length, uint8_t source_channel);

/**
 * @brief 将虚拟从机地址映射为真实从机地址
 * @param virtual_addr 虚拟从机地址
 * @return 真实从机地址，如果没有映射则返回原地址
 */
uint8_t modbus_queue_map_virtual_to_real(uint8_t virtual_addr);

/**
 * @brief 将真实从机地址映射为虚拟从机地址
 * @param real_addr 真实从机地址
 * @return 虚拟从机地址，如果没有映射则返回原地址
 */
uint8_t modbus_queue_map_real_to_virtual(uint8_t real_addr);

/**
 * @brief 验证从机地址映射是否有效
 * @param mapping 映射结构体指针
 * @return true 有效，false 无效
 */
bool modbus_queue_validate_mapping(const modbus_slave_mapping_t *mapping);

/**
 * @brief 处理包含地址映射的Modbus数据包
 * @param data 数据包指针
 * @param length 数据包长度
 * @param is_request 是否为请求包（true为请求，false为响应）
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_process_mapping(uint8_t *data, size_t length, bool is_request);

/**
 * @brief 处理响应包的地址映射（将真实地址转换回虚拟地址）
 * @param data 响应包数据指针
 * @param length 数据长度
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_process_response_mapping(uint8_t *data, size_t length);

/**
 * @brief 处理来自CH3的响应
 * @param data 响应数据
 * @param length 数据长度
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_handle_response(const uint8_t *data, size_t length);

/**
 * @brief 获取队列当前状态信息
 * @param status 状态结构体指针
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_get_status(modbus_queue_status_t *status);

/**
 * @brief 清空队列中的所有请求
 * @return ESP_OK 成功
 */
esp_err_t modbus_queue_clear(void);

/**
 * @brief 重新加载配置（用于配置热更新）
 * @return ESP_OK 成功，其他值表示错误
 */
esp_err_t modbus_queue_reload_config(void);

#ifdef __cplusplus
}
#endif

#endif /* SX_MODBUS_QUEUE_H */
