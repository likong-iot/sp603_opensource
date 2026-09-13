/*
 * @Author: Orion
 * @Date: 2025-01-28 00:00:00
 * @LastEditors: Orion
 * @LastEditTime: 2025-01-28 00:00:00
 * @FilePath: \2CH_485HUB-V1.0\main\sx_modbus_queue.c
 * @Description: Modbus过滤透传排队模式实现 - 基于transparent_queue的排队逻辑 +
 * 地址过滤映射
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "include/sx_modbus_queue.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "include/sx_utils.h"
#include "nvs.h"
#include "sx_async_uart.h"
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*==============================================================================
 * 配置参数宏定义 - 所有可调节参数集中在此处
 *============================================================================*/

// ===== 队列基本参数 =====
#define MAX_QUEUE_SIZE 8       // 最大队列大小
#define DATA_BUFFER_SIZE 2048  // 单个数据缓冲区大小(字节)
#define MAX_DATA_LENGTH 2048   // 最大数据长度限制

// ===== 任务和性能参数 =====
#define TASK_STACK_SIZE 16384 // 任务栈大小(字节) - 放PSRAM更宽裕
#define TASK_PRIORITY 18      // 任务优先级(0-25)
#define TASK_CORE_ID 1        // 任务绑定的CPU核心(0或1)
#define MAIN_LOOP_DELAY_MS 15 // 主循环延时(毫秒)
#define QUEUE_STOP_WAIT_MS 1500

// ===== 时间管理参数 =====
#define TIME_UPDATE_INTERVAL_MS 10    // 时间更新间隔(毫秒)
#define CACHE_UPDATE_CYCLES 10        // 缓存时间更新周期(次)
#define WATCHDOG_TIMEOUT_MULTIPLIER 2 // 看门狗超时倍数
#define URGENT_REQUEST_BATCH_SIZE 3   // 优先更新的紧急请求数量

// ===== 日志配置参数 =====
#define ENABLE_DETAILED_LOGS 0        // 详细日志开关(0=关闭, 1=开启)
#define QUEUE_STATUS_LOG_INTERVAL 200 // 队列状态日志间隔(循环次数)

// ===== 缓冲区管理参数 =====
#define BUFFER_POOL_SEARCH_LIMIT MAX_QUEUE_SIZE // 缓冲池搜索限制
#define INVALID_BUFFER_INDEX 0xFF               // 无效缓冲区索引标记

// ===== 通道相关参数 =====
#define CHANNEL_1 1       // 通道1标识
#define CHANNEL_2 2       // 通道2标识
#define CHANNEL_3 3       // 通道3标识
#define INVALID_CHANNEL 0 // 无效通道标识

// ===== 默认超时时间(毫秒) =====
#define DEFAULT_CH1_TIMEOUT_MS 1000 // CH1默认超时时间
#define DEFAULT_CH2_TIMEOUT_MS 1000 // CH2默认超时时间
#define DEFAULT_CH3_TIMEOUT_MS 1000 // CH3默认超时时间

// ===== 地址映射和过滤参数 =====
#define ADDRESS_FILTER_CACHE_TTL 1000 // 地址过滤缓存生存时间(ms)
#define VIRTUAL_TO_REAL_CACHE_SIZE 32 // 虚拟到真实地址映射缓存

/*==============================================================================
 * 条件编译宏定义
 *============================================================================*/

// 性能优化的条件日志宏
#if ENABLE_DETAILED_LOGS
#define LOG_DETAILED(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#define LOG_QUEUE_OP(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#define LOG_FILTER(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)
#else
#define LOG_DETAILED(fmt, ...)                                                 \
  do {                                                                         \
  } while (0)
#define LOG_QUEUE_OP(fmt, ...)                                                 \
  do {                                                                         \
  } while (0)
#define LOG_FILTER(fmt, ...)                                                   \
  do {                                                                         \
  } while (0)
#endif

// Modbus功能码定义
#define MODBUS_READ_COILS 0x01
#define MODBUS_READ_DISCRETE 0x02
#define MODBUS_READ_HOLDING 0x03
#define MODBUS_READ_INPUT 0x04
#define MODBUS_WRITE_SINGLE_COIL 0x05
#define MODBUS_WRITE_SINGLE_REG 0x06
#define MODBUS_WRITE_MULTIPLE_COILS 0x0F
#define MODBUS_WRITE_MULTIPLE_REGS 0x10

/*==============================================================================
 * 全局变量定义
 *============================================================================*/

static const char *TAG = "MODBUS_QUEUE";
int timeout1_mq, timeout2_mq, timeout3_mq;

// 全局数据缓冲池 - 运行时从PSRAM分配
static uint8_t *data_buffer_pool = NULL;
static bool *buffer_pool_used = NULL;

// 地址映射缓存条目
typedef struct {
  uint8_t virtual_addr;  // 虚拟地址
  uint8_t real_addr;     // 真实地址
  uint32_t timestamp_ms; // 缓存时间戳
  bool valid;            // 缓存有效性
} addr_mapping_cache_entry_t;

// 优化的地址映射缓存 - 虚拟到真实
static EXT_RAM_BSS_ATTR addr_mapping_cache_entry_t v2r_cache[VIRTUAL_TO_REAL_CACHE_SIZE];
static uint8_t v2r_cache_index = 0;

// 优化的地址映射缓存 - 真实到虚拟
static EXT_RAM_BSS_ATTR addr_mapping_cache_entry_t r2v_cache[VIRTUAL_TO_REAL_CACHE_SIZE];
static uint8_t r2v_cache_index = 0;

// 优化的队列请求结构体 - 零拷贝设计
typedef struct {
  uint8_t source_channel;     // 源通道(1或2) - 使用uint8_t节省内存
  uint8_t *data_ptr;          // 指向缓冲池的指针，避免数据拷贝
  uint16_t data_length;       // 数据长度 - 使用uint16_t节省内存
  uint32_t start_time_ms;     // 开始等待时间(ms) - 使用32位节省内存
  uint16_t remaining_time_ms; // 剩余时间(ms) - 使用16位节省内存
  uint8_t buffer_index;       // 缓冲池索引
} queue_request_t;

// 排队状态枚举
typedef enum {
  QUEUE_IDLE = 0, // 空闲状态
  QUEUE_WAITING   // 等待CH3响应
} queue_state_t;

static queue_request_t *request_queue = NULL;
static int queue_size = 0;
static int current_waiting_index = -1; // 当前等待响应的请求索引

// 全局状态变量
static queue_state_t current_state = QUEUE_IDLE;
static uint64_t wait_start_time = 0;
static uint8_t current_source_channel =
    0; // 当前请求的源通道（用于直接发送模式）

// 性能优化变量 - 使用宏定义的参数
static bool queue_needs_resort = false;
static uint64_t last_time_update = 0;
static uint64_t cached_time = 0;
static uint32_t time_cache_counter = 0;

// 配置结构体
static modbus_queue_config_t current_config = {
    .enabled = false,
    .mode = MODBUS_QUEUE_WHITELIST,
    .range_count = 0,
    .queue_timeout_ms = 1000,
    .max_queue_size = 8,
    .mapping_enabled = false,
    .slave_mappings = {{0, 0, false}},
    .mapping_count = 0};

static bool config_loaded = false;
static bool queue_running = false;
static TaskHandle_t modbus_queue_task_handle = NULL;
static volatile bool modbus_queue_task_exited = true;

static void reset_to_idle(void);

static void *queue_psram_calloc(size_t count, size_t size) {
  void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr == NULL) {
    ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
  }
  return ptr;
}

static bool ensure_queue_storage(void) {
  if (data_buffer_pool == NULL) {
    data_buffer_pool = queue_psram_calloc(MAX_QUEUE_SIZE, DATA_BUFFER_SIZE);
  }
  if (buffer_pool_used == NULL) {
    buffer_pool_used = queue_psram_calloc(MAX_QUEUE_SIZE, sizeof(bool));
  }
  if (request_queue == NULL) {
    request_queue = queue_psram_calloc(MAX_QUEUE_SIZE, sizeof(queue_request_t));
  }
  return data_buffer_pool != NULL && buffer_pool_used != NULL &&
         request_queue != NULL;
}

/*==============================================================================
 * 缓冲池管理函数 - 零拷贝优化
 *============================================================================*/

static inline uint8_t *allocate_buffer_from_pool(uint8_t *buffer_index) {
  if (!ensure_queue_storage()) {
    return NULL;
  }
  for (uint8_t i = 0; i < MAX_QUEUE_SIZE; i++) {
    if (!buffer_pool_used[i]) {
      buffer_pool_used[i] = true;
      *buffer_index = i;
      return data_buffer_pool + ((size_t)i * DATA_BUFFER_SIZE);
    }
  }
  return NULL; // 缓冲池已满
}

static inline void release_buffer_to_pool(uint8_t buffer_index) {
  if (buffer_index < MAX_QUEUE_SIZE) {
    buffer_pool_used[buffer_index] = false;
  }
}

static void clear_queue_state(void) {
  if (!ensure_queue_storage()) {
    queue_size = 0;
    queue_needs_resort = false;
    reset_to_idle();
    return;
  }
  for (int i = 0; i < queue_size; i++) {
    release_buffer_to_pool(request_queue[i].buffer_index);
  }
  memset(request_queue, 0, MAX_QUEUE_SIZE * sizeof(queue_request_t));
  memset(buffer_pool_used, 0, MAX_QUEUE_SIZE * sizeof(bool));
  queue_size = 0;
  queue_needs_resort = false;
  reset_to_idle();
}

static bool wait_modbus_queue_task_exit(uint32_t timeout_ms) {
  uint32_t waited = 0;
  while (!modbus_queue_task_exited && waited < timeout_ms) {
    vTaskDelay(sx_ms_to_ticks(10));
    waited += 10;
  }
  return modbus_queue_task_exited;
}

/*==============================================================================
 * 时间管理函数
 *============================================================================*/

// 快速获取当前时间(ms) - 内联优化
static inline uint32_t get_current_time_ms_fast(void) {
  return (uint32_t)(esp_timer_get_time() / 1000);
}

// 获取当前时间（毫秒） - 带缓存优化
static uint64_t get_current_time_ms(void) {
  return esp_timer_get_time() / 1000;
}

// 获取缓存的时间 - 减少系统调用开销
static uint64_t get_cached_time_ms(void) {
  // 使用宏定义的缓存更新周期
  if (++time_cache_counter >= CACHE_UPDATE_CYCLES) {
    cached_time = esp_timer_get_time() / 1000;
    time_cache_counter = 0;
  }
  return cached_time;
}

// 获取通道对应的超时时间 - 优化版本
static inline uint16_t get_timeout_for_channel_fast(uint8_t channel) {
  return (uint16_t)((channel == 1) ? timeout1_mq : timeout2_mq);
}

/*==============================================================================
 * 地址映射缓存函数
 *============================================================================*/

// 简单哈希函数用于地址映射缓存
static inline uint8_t hash_addr(uint8_t addr) {
  return addr & (VIRTUAL_TO_REAL_CACHE_SIZE - 1);
}

// 快速查找虚拟到真实地址映射缓存
static inline uint8_t find_v2r_cache(uint8_t virtual_addr) {
  uint32_t current_time = get_cached_time_ms();
  uint8_t hash = hash_addr(virtual_addr);

  addr_mapping_cache_entry_t *entry = &v2r_cache[hash];
  if (entry->valid && entry->virtual_addr == virtual_addr &&
      (current_time - entry->timestamp_ms) < ADDRESS_FILTER_CACHE_TTL) {
    return entry->real_addr;
  }

  // 线性探测查找
  for (uint8_t i = 1; i < VIRTUAL_TO_REAL_CACHE_SIZE; i++) {
    uint8_t idx = (hash + i) & (VIRTUAL_TO_REAL_CACHE_SIZE - 1);
    entry = &v2r_cache[idx];
    if (entry->valid && entry->virtual_addr == virtual_addr &&
        (current_time - entry->timestamp_ms) < ADDRESS_FILTER_CACHE_TTL) {
      return entry->real_addr;
    }
  }

  return virtual_addr; // 缓存未命中，返回原地址
}

// 更新虚拟到真实地址映射缓存
static inline void update_v2r_cache(uint8_t virtual_addr, uint8_t real_addr) {
  uint8_t hash = hash_addr(virtual_addr);
  addr_mapping_cache_entry_t *entry = &v2r_cache[hash];

  if (entry->valid) {
    hash = (v2r_cache_index++) & (VIRTUAL_TO_REAL_CACHE_SIZE - 1);
    entry = &v2r_cache[hash];
  }

  entry->virtual_addr = virtual_addr;
  entry->real_addr = real_addr;
  entry->timestamp_ms = get_cached_time_ms();
  entry->valid = true;
}

// 快速查找真实到虚拟地址映射缓存
static inline uint8_t find_r2v_cache(uint8_t real_addr) {
  uint32_t current_time = get_cached_time_ms();
  uint8_t hash = hash_addr(real_addr);

  addr_mapping_cache_entry_t *entry = &r2v_cache[hash];
  if (entry->valid && entry->real_addr == real_addr &&
      (current_time - entry->timestamp_ms) < ADDRESS_FILTER_CACHE_TTL) {
    return entry->virtual_addr;
  }

  // 线性探测查找
  for (uint8_t i = 1; i < VIRTUAL_TO_REAL_CACHE_SIZE; i++) {
    uint8_t idx = (hash + i) & (VIRTUAL_TO_REAL_CACHE_SIZE - 1);
    entry = &r2v_cache[idx];
    if (entry->valid && entry->real_addr == real_addr &&
        (current_time - entry->timestamp_ms) < ADDRESS_FILTER_CACHE_TTL) {
      return entry->virtual_addr;
    }
  }

  return real_addr; // 缓存未命中
}

// 更新真实到虚拟地址映射缓存
static inline void update_r2v_cache(uint8_t real_addr, uint8_t virtual_addr) {
  uint8_t hash = hash_addr(real_addr);
  addr_mapping_cache_entry_t *entry = &r2v_cache[hash];

  if (entry->valid) {
    hash = (r2v_cache_index++) & (VIRTUAL_TO_REAL_CACHE_SIZE - 1);
    entry = &r2v_cache[hash];
  }

  entry->real_addr = real_addr;
  entry->virtual_addr = virtual_addr;
  entry->timestamp_ms = get_cached_time_ms();
  entry->valid = true;
}

/*==============================================================================
 * Modbus地址过滤和映射功能
 *============================================================================*/

/**
 * @brief 解析Modbus请求帧获取从机号和寄存器地址
 */
static bool parse_modbus_request(const uint8_t *data, int length,
                                 uint8_t *slave_id, uint8_t *function_code,
                                 uint16_t *start_addr, uint16_t *count) {
  if (length < 6)
    return false;

  *slave_id = data[0];
  *function_code = data[1];
  *start_addr = (data[2] << 8) | data[3];

  switch (*function_code) {
  case 0x01:
  case 0x02:
  case 0x03:
  case 0x04:
    if (length < 8)
      return false;
    *count = (data[4] << 8) | data[5];
    break;
  case 0x05:
  case 0x06:
    if (length < 8)
      return false;
    *count = 1;
    break;
  case 0x0F:
  case 0x10:
    if (length < 9)
      return false;
    *count = (data[4] << 8) | data[5];
    break;
  default:
    if (length < 8)
      return false;
    *count = (data[4] << 8) | data[5];
    break;
  }

  return true;
}

/**
 * @brief 检查从机号和寄存器地址是否在指定范围内
 */
static bool is_address_in_range(uint8_t slave_id, uint16_t register_addr,
                                const modbus_queue_addr_range_t *range) {
  if (range->slave_id != 0 && range->slave_id != slave_id) {
    return false;
  }
  return (register_addr >= range->start_addr &&
          register_addr <= range->end_addr);
}

/**
 * @brief 检查单个地址是否在任何配置的范围内
 */
static bool is_address_in_any_range(uint8_t slave_id, uint16_t register_addr,
                                    const modbus_queue_config_t *config) {
  for (int i = 0; i < config->range_count; i++) {
    if (is_address_in_range(slave_id, register_addr, &config->addr_ranges[i])) {
      return true;
    }
  }
  return false;
}

/**
 * @brief 检查从机地址是否为已映射的真实地址
 */
static bool is_mapped_real_address(uint8_t slave_id) {
  if (!config_loaded || !current_config.mapping_enabled ||
      current_config.mapping_count == 0) {
    return false;
  }

  for (int i = 0; i < current_config.mapping_count; i++) {
    const modbus_slave_mapping_t *mapping = &current_config.slave_mappings[i];
    if (mapping->enabled && mapping->real_addr == slave_id) {
      return true;
    }
  }
  return false;
}

/**
 * @brief 检查地址范围是否与配置中的任何范围有重叠
 */
static bool
is_range_overlapping_with_any_config(uint8_t slave_id, uint16_t req_start,
                                     uint16_t req_end,
                                     const modbus_queue_config_t *config) {
  for (int i = 0; i < config->range_count; i++) {
    const modbus_queue_addr_range_t *cfg_range = &config->addr_ranges[i];

    // 检查从机号匹配（0表示匹配所有从机）
    if (cfg_range->slave_id != 0 && cfg_range->slave_id != slave_id) {
      continue;
    }

    // 检查地址范围是否重叠：req_start <= cfg_end && req_end >= cfg_start
    bool overlapping = (req_start <= cfg_range->end_addr) &&
                       (req_end >= cfg_range->start_addr);

    if (overlapping) {
      return true;
    }
  }

  return false;
}

/**
 * @brief 智能地址过滤检查（根据映射情况决定过滤逻辑）
 */
static bool check_modbus_address_filter_smart(const uint8_t *data, int length) {
  if (!config_loaded || !current_config.enabled) {
    return true;
  }

  uint8_t slave_id, function_code;
  uint16_t start_addr, count;

  if (!parse_modbus_request(data, length, &slave_id, &function_code,
                            &start_addr, &count)) {
    ESP_LOGW(TAG, "无法解析Modbus请求帧");
    return false;
  }

  // 拒绝直接访问已映射的真实地址
  if (is_mapped_real_address(slave_id)) {
    LOG_FILTER("拒绝直接访问已映射的真实地址%d", slave_id);
    return false;
  }

  // 如果没有配置地址范围
  if (current_config.range_count == 0) {
    if (current_config.mode == MODBUS_QUEUE_WHITELIST) {
      LOG_FILTER("白名单模式无配置范围，允许所有数据通过");
      return true;
    } else {
      LOG_FILTER("黑名单模式无配置范围，允许所有数据通过");
      return true;
    }
  }

  // 计算请求的地址范围
  uint8_t filter_slave_id = slave_id;
  if (count == 0 || (uint32_t)start_addr + (uint32_t)count > 0x10000U) {
    LOG_FILTER("拒绝非法地址范围: 从机%d, 地址%d, 数量%d", filter_slave_id,
               start_addr, count);
    return false;
  }
  uint16_t end_addr = (uint16_t)(start_addr + count - 1);

  if (current_config.mode == MODBUS_QUEUE_WHITELIST) {
    // 白名单模式：整个地址范围必须在配置范围内
    bool in_range =
        is_address_in_any_range(filter_slave_id, start_addr, &current_config) &&
        is_address_in_any_range(filter_slave_id, end_addr, &current_config);

    if (in_range) {
      LOG_FILTER("白名单通过: 从机%d, 地址%d-%d", filter_slave_id, start_addr,
                 end_addr);
      return true;
    } else {
      LOG_FILTER("白名单拒绝: 从机%d, 地址%d-%d不在允许范围内", filter_slave_id,
                 start_addr, end_addr);
      return false;
    }
  } else {
    // 黑名单模式：地址范围不能与配置范围重叠
    bool overlapping = is_range_overlapping_with_any_config(
        filter_slave_id, start_addr, end_addr, &current_config);

    if (!overlapping) {
      LOG_FILTER("黑名单通过: 从机%d, 地址%d-%d", filter_slave_id, start_addr,
                 end_addr);
      return true;
    } else {
      LOG_FILTER("黑名单拒绝: 从机%d, 地址%d-%d与禁止范围重叠", filter_slave_id,
                 start_addr, end_addr);
      return false;
    }
  }
}

static bool modbus_queue_verify_crc(const uint8_t *data, size_t length) {
  if (data == NULL || length < 4 || length - 2U > UINT16_MAX)
    return false;

  uint16_t received_crc =
      (uint16_t)data[length - 2U] | ((uint16_t)data[length - 1U] << 8);
  uint16_t calculated_crc = ModbusCRC16((uint8_t *)data, (uint16_t)(length - 2U));
  return received_crc == calculated_crc;
}

/**
 * @brief 优化的虚拟到真实地址映射
 */
uint8_t modbus_queue_map_virtual_to_real(uint8_t virtual_addr) {
  if (!config_loaded || !current_config.mapping_enabled ||
      current_config.mapping_count == 0) {
    return virtual_addr;
  }

  uint8_t cached_real = find_v2r_cache(virtual_addr);
  if (cached_real != virtual_addr) {
    return cached_real;
  }

  for (int i = 0; i < current_config.mapping_count; i++) {
    const modbus_slave_mapping_t *mapping = &current_config.slave_mappings[i];
    if (mapping->enabled && mapping->virtual_addr == virtual_addr) {
      update_v2r_cache(virtual_addr, mapping->real_addr);
      return mapping->real_addr;
    }
  }

  return virtual_addr;
}

/**
 * @brief 优化的真实到虚拟地址映射
 */
uint8_t modbus_queue_map_real_to_virtual(uint8_t real_addr) {
  if (!config_loaded || !current_config.mapping_enabled ||
      current_config.mapping_count == 0) {
    return real_addr;
  }

  uint8_t cached_virtual = find_r2v_cache(real_addr);
  if (cached_virtual != real_addr) {
    return cached_virtual;
  }

  for (int i = 0; i < current_config.mapping_count; i++) {
    const modbus_slave_mapping_t *mapping = &current_config.slave_mappings[i];
    if (mapping->enabled && mapping->real_addr == real_addr) {
      update_r2v_cache(real_addr, mapping->virtual_addr);
      return mapping->virtual_addr;
    }
  }

  return real_addr;
}

/**
 * @brief 处理包含地址映射的Modbus数据包
 */
esp_err_t modbus_queue_process_mapping(uint8_t *data, size_t length,
                                       bool is_request) {
  if (!config_loaded || !current_config.mapping_enabled) {
    return ESP_OK;
  }
  if (data == NULL || length < 4 || length - 2U > UINT16_MAX) {
    return ESP_ERR_INVALID_SIZE;
  }
  if (!modbus_queue_verify_crc(data, length)) {
    return ESP_ERR_INVALID_CRC;
  }

  uint8_t slave_addr = data[0];
  bool address_changed = false;

  if (is_request) {
    uint8_t real_addr = modbus_queue_map_virtual_to_real(slave_addr);
    if (real_addr != slave_addr) {
      data[0] = real_addr;
      address_changed = true;
      LOG_DETAILED("请求包地址映射: 虚拟%d -> 真实%d", slave_addr, real_addr);
    }
  } else {
    uint8_t virtual_addr = modbus_queue_map_real_to_virtual(slave_addr);
    if (virtual_addr != slave_addr) {
      data[0] = virtual_addr;
      address_changed = true;
      LOG_DETAILED("响应包地址映射: 真实%d -> 虚拟%d", slave_addr,
                   virtual_addr);
    }
  }

  if (address_changed) {
    uint16_t new_crc = ModbusCRC16(data, (uint16_t)(length - 2U));
    data[length - 2] = new_crc & 0xFF;
    data[length - 1] = (new_crc >> 8) & 0xFF;
    LOG_DETAILED("地址映射后重新计算CRC: 0x%04X", new_crc);
  }

  return ESP_OK;
}

/*==============================================================================
 * 队列排序和管理函数（来自transparent_queue）
 *============================================================================*/

// 更新队列中所有请求的剩余时间 - 优化版本
__attribute__((unused)) static void update_remaining_times(void) {
  uint64_t current_time = get_current_time_ms();

  if (current_time - last_time_update < TIME_UPDATE_INTERVAL_MS) {
    return;
  }

  last_time_update = current_time;

  int update_count = (queue_size < URGENT_REQUEST_BATCH_SIZE)
                         ? queue_size
                         : URGENT_REQUEST_BATCH_SIZE;
  for (int i = 0; i < update_count; i++) {
    uint32_t elapsed = current_time - request_queue[i].start_time_ms;
    uint16_t timeout =
        get_timeout_for_channel_fast(request_queue[i].source_channel);
    if (elapsed >= timeout) {
      request_queue[i].remaining_time_ms = 0;
    } else {
      request_queue[i].remaining_time_ms = timeout - elapsed;
    }
  }

  if (queue_size > URGENT_REQUEST_BATCH_SIZE) {
    static int batch_offset = URGENT_REQUEST_BATCH_SIZE;
    if (batch_offset < queue_size) {
      uint32_t elapsed =
          current_time - request_queue[batch_offset].start_time_ms;
      uint16_t timeout = get_timeout_for_channel_fast(
          request_queue[batch_offset].source_channel);
      if (elapsed >= timeout) {
        request_queue[batch_offset].remaining_time_ms = 0;
      } else {
        request_queue[batch_offset].remaining_time_ms = timeout - elapsed;
      }
      batch_offset++;
    } else {
      batch_offset = URGENT_REQUEST_BATCH_SIZE;
    }
  }
}

// 按剩余时间排序队列（时间最少的在前） - 优化的插入排序
static void sort_queue_by_remaining_time(void) {
  if (!queue_needs_resort || queue_size <= 1) {
    return;
  }

  for (int i = 1; i < queue_size; i++) {
    queue_request_t key = request_queue[i];
    int j = i - 1;

    while (j >= 0 &&
           request_queue[j].remaining_time_ms > key.remaining_time_ms) {
      request_queue[j + 1] = request_queue[j];
      j--;
    }
    request_queue[j + 1] = key;
  }

  queue_needs_resort = false;
  ESP_LOGD(TAG, "队列已重新排序，大小: %d", queue_size);
}

// 添加请求到队列 - 零拷贝优化版本（添加过滤和映射）
static bool add_request_to_queue_zero_copy(uint8_t channel, const uint8_t *data,
                                           uint16_t length) {
  if (queue_size >= MAX_QUEUE_SIZE) {
    ESP_LOGW(TAG, "队列已满，丢弃CH%d请求", channel);
    return false;
  }

  if (length > MAX_DATA_LENGTH) {
    ESP_LOGW(TAG, "数据长度超过缓冲区大小，截断");
    length = MAX_DATA_LENGTH;
  }

  // 从缓冲池分配缓冲区
  uint8_t buffer_index;
  uint8_t *buffer = allocate_buffer_from_pool(&buffer_index);
  if (!buffer) {
    ESP_LOGE(TAG, "缓冲池已满，无法分配缓冲区");
    return false;
  }

  // 拷贝并处理数据（过滤和映射）
  memcpy(buffer, data, length);

  // 地址过滤
  if (!check_modbus_address_filter_smart(buffer, length)) {
    LOG_FILTER("CH%d数据被地址过滤器丢弃", channel);
    release_buffer_to_pool(buffer_index);
    return false;
  }

  // 地址映射：虚拟地址→真实地址
  esp_err_t mapping_result = modbus_queue_process_mapping(buffer, length, true);
  if (mapping_result != ESP_OK) {
    ESP_LOGW(TAG, "CH%d请求地址映射失败: %s", channel,
             esp_err_to_name(mapping_result));
    release_buffer_to_pool(buffer_index);
    return false;
  }

  queue_request_t *req = &request_queue[queue_size];
  req->source_channel = channel;
  req->data_ptr = buffer;
  req->data_length = length;
  req->buffer_index = buffer_index;
  req->start_time_ms = get_current_time_ms_fast();
  req->remaining_time_ms = get_timeout_for_channel_fast(channel);

  queue_size++;
  LOG_QUEUE_OP("CH%d请求加入队列，当前队列大小: %d", channel, queue_size);

  queue_needs_resort = true;
  sort_queue_by_remaining_time();
  return true;
}

// 移除队列中的请求 - 优化版本，释放缓冲区
static void remove_request_from_queue_optimized(int index) {
  if (index < 0 || index >= queue_size)
    return;

  release_buffer_to_pool(request_queue[index].buffer_index);

  if (index < queue_size - 1) {
    memmove(&request_queue[index], &request_queue[index + 1],
            (queue_size - index - 1) * sizeof(queue_request_t));
  }
  queue_size--;

  if (current_waiting_index == index) {
    current_waiting_index = -1;
  } else if (current_waiting_index > index) {
    current_waiting_index--;
  }
}

// 重置状态为空闲
static void reset_to_idle(void) {
  current_state = QUEUE_IDLE;
  current_waiting_index = -1;
  current_source_channel = 0;
  wait_start_time = 0;
  ESP_LOGD(TAG, "状态重置为空闲");
}

/*==============================================================================
 * 配置管理功能
 *============================================================================*/

esp_err_t modbus_queue_load_config(modbus_queue_config_t *config) {
  nvs_handle_t nvs_handle;
  esp_err_t ret = nvs_open("modbus_queue", NVS_READONLY, &nvs_handle);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "无法打开NVS存储，使用默认配置");
    return ret;
  }

  size_t required_size = sizeof(modbus_queue_config_t);
  ret = nvs_get_blob(nvs_handle, "config", config ? config : &current_config,
                     &required_size);

  nvs_close(nvs_handle);

  if (ret == ESP_OK) {
    if (!config) {
      config_loaded = true;
    }
    ESP_LOGI(TAG, "配置加载成功: 模式=%s, 映射=%s",
             (config ? config->mode : current_config.mode) ==
                     MODBUS_QUEUE_WHITELIST
                 ? "白名单"
                 : "黑名单",
             (config ? config->mapping_enabled : current_config.mapping_enabled)
                 ? "启用"
                 : "禁用");
  } else {
    ESP_LOGW(TAG, "无法读取配置: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t modbus_queue_save_config(const modbus_queue_config_t *config) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs_handle;
  esp_err_t ret = nvs_open("modbus_queue", NVS_READWRITE, &nvs_handle);
  if (ret != ESP_OK) {
    return ret;
  }

  ret =
      nvs_set_blob(nvs_handle, "config", config, sizeof(modbus_queue_config_t));
  if (ret == ESP_OK) {
    ret = nvs_commit(nvs_handle);
    if (ret == ESP_OK) {
      memcpy(&current_config, config, sizeof(modbus_queue_config_t));
      config_loaded = true;
      ESP_LOGI(TAG, "配置保存成功");
    }
  }

  nvs_close(nvs_handle);
  return ret;
}

esp_err_t modbus_queue_get_config(modbus_queue_config_t *config) {
  if (config == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  if (!config_loaded) {
    return ESP_ERR_INVALID_STATE;
  }

  memcpy(config, &current_config, sizeof(modbus_queue_config_t));
  return ESP_OK;
}

esp_err_t modbus_queue_reload_config(void) {
  ESP_LOGI(TAG, "开始重新加载配置...");

  esp_err_t ret = modbus_queue_load_config(NULL);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "配置重新加载成功");
  } else {
    ESP_LOGW(TAG, "配置重新加载失败: %s", esp_err_to_name(ret));
  }

  return ret;
}

/*==============================================================================
 * 主任务函数（基于transparent_queue逻辑 + 过滤映射）
 *============================================================================*/

void modbus_queue_task(void *pvParameter) {
  ESP_LOGI(TAG, "Modbus队列任务启动 - 智能排队透传模式 + 地址过滤映射");

  // 检查当前工作模式，防止多任务冲突
  char work_mode[32] = {0};
  nvs_handle_t nvs_check;
  if (nvs_open("storage", NVS_READONLY, &nvs_check) == ESP_OK) {
    size_t len = sizeof(work_mode);
    nvs_get_str(nvs_check, "w_mode", work_mode, &len);
    nvs_close(nvs_check);

    if (strcmp(work_mode, "modbus_queue") != 0) {
      ESP_LOGW(TAG,
               "⚠️  当前工作模式不是modbus_queue（当前：%s），任务退出防止冲突",
               work_mode);
      modbus_queue_task_exited = true;
      modbus_queue_task_handle = NULL;
      delete_self_app_task_with_caps();
      return;
    }
  }

  uint8_t buffer[DATA_BUFFER_SIZE];
  uint64_t timestamp;
  int data_len;

  ESP_LOGI(TAG, "队列大小: %d, 超时配置: CH1=%dms, CH2=%dms, CH3=%dms",
           MAX_QUEUE_SIZE, timeout1_mq, timeout2_mq, timeout3_mq);

  while (queue_running) {
    // 1. 初始化缓存时间
    cached_time = get_current_time_ms();
    time_cache_counter = 0;

    // 2. 检查CH3是否有响应数据
    data_len = pop_channel_data(3, buffer, sizeof(buffer), &timestamp);
    if (data_len > 0) {
      if (current_state == QUEUE_WAITING) {
        // 处理响应包的地址映射（真实地址→虚拟地址）
        uint8_t response_data[DATA_BUFFER_SIZE];
        memcpy(response_data, buffer, data_len);
        esp_err_t mapping_result =
            modbus_queue_process_mapping(response_data, data_len, false);
        if (mapping_result != ESP_OK) {
          ESP_LOGW(TAG, "CH3响应地址映射失败: %s",
                   esp_err_to_name(mapping_result));
          continue;
        }

        if (current_waiting_index >= 0) {
          // 响应的是队列中的请求
          queue_request_t *current_req = &request_queue[current_waiting_index];
          tx_tasks_to_channel(response_data, data_len,
                              current_req->source_channel);
          remove_request_from_queue_optimized(current_waiting_index);
        } else {
          // 响应的是直接发送的请求（使用保存的源通道）
          tx_tasks_to_channel(response_data, data_len, current_source_channel);
        }
        reset_to_idle();
      }
    }

    // 3. 看门狗机制
    uint64_t current_time = get_cached_time_ms();
    if (current_state == QUEUE_WAITING && wait_start_time > 0) {
      uint64_t wait_duration = current_time - wait_start_time;
      uint64_t max_wait_time;
      if (current_waiting_index >= 0 && current_waiting_index < queue_size) {
        max_wait_time =
            get_timeout_for_channel_fast(
                request_queue[current_waiting_index].source_channel) *
            WATCHDOG_TIMEOUT_MULTIPLIER;
      } else {
        max_wait_time = get_timeout_for_channel_fast(current_source_channel) *
                        WATCHDOG_TIMEOUT_MULTIPLIER;
      }

      if (wait_duration > max_wait_time) {
        ESP_LOGE(TAG, "看门狗触发：等待时间%lldms超过上限%lldms，强制重置",
                 wait_duration, max_wait_time);
        if (current_waiting_index >= 0 && current_waiting_index < queue_size) {
          remove_request_from_queue_optimized(current_waiting_index);
        }
        reset_to_idle();
      }
    }

    // 4. 合并操作：检查超时并更新剩余时间
    uint32_t current_time_ms = get_current_time_ms_fast();
    for (int i = queue_size - 1; i >= 0; i--) {
      uint32_t elapsed = current_time_ms - request_queue[i].start_time_ms;
      uint16_t timeout =
          get_timeout_for_channel_fast(request_queue[i].source_channel);

      if (elapsed >= timeout) {
        request_queue[i].remaining_time_ms = 0;
      } else {
        request_queue[i].remaining_time_ms = timeout - elapsed;
      }

      if (request_queue[i].remaining_time_ms == 0) {
        LOG_QUEUE_OP("CH%d请求超时，移除队列", request_queue[i].source_channel);

        if (current_state == QUEUE_WAITING && current_waiting_index == i) {
          ESP_LOGW(TAG, "当前等待的请求超时，重置状态");
          reset_to_idle();
        }

        remove_request_from_queue_optimized(i);

        if (current_waiting_index > i) {
          current_waiting_index--;
        } else if (current_waiting_index == i &&
                   current_state == QUEUE_WAITING) {
          reset_to_idle();
        }
      }
    }

    // 5. 处理新的CH1和CH2数据（添加过滤和映射）
    data_len = pop_channel_data(1, buffer, sizeof(buffer), &timestamp);
    if (data_len > 0) {
      // 创建处理数据副本
      uint8_t processed_data[DATA_BUFFER_SIZE];
      memcpy(processed_data, buffer, data_len);

      // 地址过滤检查
      if (!check_modbus_address_filter_smart(processed_data, data_len)) {
        LOG_FILTER("CH1数据被地址过滤器丢弃");
      } else {
        // 地址映射：虚拟地址→真实地址
        esp_err_t mapping_result =
            modbus_queue_process_mapping(processed_data, data_len, true);
        if (mapping_result != ESP_OK) {
          ESP_LOGW(TAG, "CH1请求地址映射失败: %s",
                   esp_err_to_name(mapping_result));
        } else if (current_state == QUEUE_IDLE) {
          // IDLE时直接发送，不加入队列
          esp_err_t send_ret = smart_send_data_to_ch3(get_uart_config_mode(), 1,
                                                      processed_data, data_len);
          if (send_ret == ESP_OK) {
            current_state = QUEUE_WAITING;
            current_waiting_index = -1;
            current_source_channel = 1;
            wait_start_time = current_time_ms;
          }
        } else {
          // WAITING时加入队列等待
          add_request_to_queue_zero_copy(1, buffer, data_len);
        }
      }
    }

    data_len = pop_channel_data(2, buffer, sizeof(buffer), &timestamp);
    if (data_len > 0) {
      // 创建处理数据副本
      uint8_t processed_data[DATA_BUFFER_SIZE];
      memcpy(processed_data, buffer, data_len);

      // 地址过滤检查
      if (!check_modbus_address_filter_smart(processed_data, data_len)) {
        LOG_FILTER("CH2数据被地址过滤器丢弃");
      } else {
        // 地址映射：虚拟地址→真实地址
        esp_err_t mapping_result =
            modbus_queue_process_mapping(processed_data, data_len, true);
        if (mapping_result != ESP_OK) {
          ESP_LOGW(TAG, "CH2请求地址映射失败: %s",
                   esp_err_to_name(mapping_result));
        } else if (current_state == QUEUE_IDLE) {
          // IDLE时直接发送，不加入队列
          esp_err_t send_ret = smart_send_data_to_ch3(get_uart_config_mode(), 2,
                                                      processed_data, data_len);
          if (send_ret == ESP_OK) {
            current_state = QUEUE_WAITING;
            current_waiting_index = -1;
            current_source_channel = 2;
            wait_start_time = current_time_ms;
          }
        } else {
          // WAITING时加入队列等待
          add_request_to_queue_zero_copy(2, buffer, data_len);
        }
      }
    }

    // 6. 如果空闲且队列中有请求，处理下一个最紧急的请求
    if (current_state == QUEUE_IDLE && queue_size > 0) {
      if (queue_needs_resort) {
        sort_queue_by_remaining_time();
      }

      queue_request_t *next_req = &request_queue[0];

      esp_err_t send_ret = smart_send_data_to_ch3(
          get_uart_config_mode(), next_req->source_channel, next_req->data_ptr,
          next_req->data_length);
      if (send_ret == ESP_OK) {
        current_state = QUEUE_WAITING;
        current_waiting_index = 0;
        wait_start_time = current_time;
      }
    }

    // 7. 打印队列状态（降低频率）
    static int debug_counter = 0;
    if (++debug_counter >= QUEUE_STATUS_LOG_INTERVAL) {
      debug_counter = 0;
      if (queue_size > 0) {
        ESP_LOGI(TAG, "队列状态: 大小=%d, 等待=%d, 最紧急剩余=%dms", queue_size,
                 current_waiting_index,
                 queue_size > 0 ? request_queue[0].remaining_time_ms : 0);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(MAIN_LOOP_DELAY_MS));
  }

  clear_queue_state();
  modbus_queue_task_exited = true;
  modbus_queue_task_handle = NULL;
  ESP_LOGI(TAG, "Modbus队列任务退出");
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

/*==============================================================================
 * 初始化函数
 *============================================================================*/

void sx_modbus_queue_init(void) {
  ESP_LOGI(TAG, "初始化Modbus队列");
  if (!ensure_queue_storage()) {
    ESP_LOGE(TAG, "无法分配Modbus队列缓冲区");
    return;
  }

  // 加载配置
  modbus_queue_load_config(NULL);

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法打开NVS存储: %s", esp_err_to_name(err));
    return;
  }

  const char *timeouts[] = {"ch1_timeout", "ch2_timeout", "ch3_timeout"};
  size_t sizes[3] = {0};
  for (int i = 0; i < 3; i++) {
    err = nvs_get_str(nvs_handle, timeouts[i], NULL, &sizes[i]);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGE(TAG, "无法获取%s: %s", timeouts[i], esp_err_to_name(err));
    }
  }

  char *timeouts_values[3] = {NULL};
  for (int i = 0; i < 3; i++) {
    if (sizes[i] == 0) {
      ESP_LOGE(TAG, "无法获取%s: %s", timeouts[i], esp_err_to_name(err));
      continue;
    } else {
      timeouts_values[i] = malloc(sizes[i]);
      if (timeouts_values[i] == NULL) {
        ESP_LOGE(TAG, "无法分配内存: %s", esp_err_to_name(err));
        continue;
      }
      err = nvs_get_str(nvs_handle, timeouts[i], timeouts_values[i], &sizes[i]);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法读取%s: %s", timeouts[i], esp_err_to_name(err));
      }
    }
  }

  timeout1_mq =
      timeouts_values[0] ? atoi(timeouts_values[0]) : DEFAULT_CH1_TIMEOUT_MS;
  timeout2_mq =
      timeouts_values[1] ? atoi(timeouts_values[1]) : DEFAULT_CH2_TIMEOUT_MS;
  timeout3_mq =
      timeouts_values[2] ? atoi(timeouts_values[2]) : DEFAULT_CH3_TIMEOUT_MS;

  for (int i = 0; i < 3; i++) {
    if (timeouts_values[i]) {
      free(timeouts_values[i]);
    }
  }

  nvs_close(nvs_handle);

  ESP_LOGI(TAG, "超时配置: CH1=%dms, CH2=%dms, CH3=%dms", timeout1_mq,
           timeout2_mq, timeout3_mq);

  queue_running = true;
  modbus_queue_task_exited = false;
  clear_queue_state();

  BaseType_t task_ret = create_app_task_psram(
      modbus_queue_task, "modbusQueueTask", TASK_STACK_SIZE, NULL,
      TASK_PRIORITY, &modbus_queue_task_handle, TASK_CORE_ID);
  if (task_ret != pdPASS) {
    modbus_queue_task_handle = NULL;
    modbus_queue_task_exited = true;
    queue_running = false;
    ESP_LOGE(TAG, "创建Modbus队列任务失败");
  }
}

esp_err_t modbus_queue_start(void) {
  if (modbus_queue_task_handle == NULL && modbus_queue_task_exited) {
    ESP_LOGW(TAG, "Modbus队列任务未创建，无法仅通过start接口启动");
    return ESP_ERR_INVALID_STATE;
  }
  queue_running = true;
  ESP_LOGI(TAG, "Modbus队列已启动");
  return ESP_OK;
}

esp_err_t modbus_queue_stop(void) {
  queue_running = false;

  if (modbus_queue_task_handle != NULL) {
    if (!wait_modbus_queue_task_exit(QUEUE_STOP_WAIT_MS)) {
      ESP_LOGW(TAG, "Modbus队列任务未及时退出，强制删除");
      delete_app_task_with_caps(modbus_queue_task_handle);
      modbus_queue_task_exited = true;
    }
    modbus_queue_task_handle = NULL;
  } else {
    modbus_queue_task_exited = true;
  }

  clear_queue_state();
  ESP_LOGI(TAG, "Modbus队列已停止");
  return ESP_OK;
}
