/*
 * @Description: 自动采集工作模式占位定义
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_err.h"
#include "sx_async_uart.h"

// NVS key names are limited to 15 characters by ESP-IDF.
#define SX_AC_NVS_ERROR_CLEAR_SUFFIX "errc"
#define SX_AC_NVS_ITEM_VERSION 1

typedef struct {
    uint8_t version;
    uint8_t enabled;
    uint8_t real_slave_addr;
    uint8_t function_code;
    uint8_t data_bits;
    uint8_t parity;
    uint8_t stop_bits;
    uint8_t reserved_u8;
    uint16_t register_addr;
    uint16_t mapped_register_addr;
    uint16_t register_num;
    uint16_t error_marker;
    uint16_t error_clear_count;
    uint16_t reserved_u16;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint32_t baudrate;
} sx_ac_nvs_item_t;

_Static_assert(sizeof(sx_ac_nvs_item_t) == 32,
               "automatic collection NVS item layout changed");

// 单个采集条目配置
typedef struct {
    bool enabled;
    uint8_t real_slave_addr;
    uint8_t function_code;
    uint16_t register_addr;
    uint16_t mapped_register_addr; // 映射到虚拟表的起始寄存器
    uint16_t register_num;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint16_t error_marker;
    uint16_t error_clear_count; // 连续失败达到该次数后清空缓存，默认50
    channel_uart_config_t uart; // 每条目绑定的串口参数
} ac_item_config_t;

// 单通道采集配置
typedef struct {
    uint8_t mapped_slave_addr; // 虚拟从机地址
    bool allow_exception_response; // 是否允许该虚拟地址返回Modbus异常帧
    int items_count;
    ac_item_config_t items[60]; // 上限可调
} ac_channel_config_t;

// 双通道总配置
typedef struct {
    ac_channel_config_t ch2;
    ac_channel_config_t ch3;
} ac_config_t;

// 自动采集模式初始化（当前为空壳，后续逐步填充）
esp_err_t sx_auto_collect_init(void);
esp_err_t sx_auto_collect_stop(void);

#ifdef __cplusplus
}
#endif
