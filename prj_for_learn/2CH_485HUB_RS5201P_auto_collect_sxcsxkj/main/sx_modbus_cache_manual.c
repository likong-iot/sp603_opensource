#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sx_modbus_cache.h"
#include "hal/uart_types.h"
#include "nvs.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 手动缓存子模式实现（完全独立） ====================

static const char *TAG_MANUAL = "MODBUS_CACHE_MANUAL";

// 手动缓存运行参数
#define MANUAL_CACHE_POLL_INTERVAL_MS 10
#define MANUAL_CACHE_DEFAULT_TIMEOUT_MS 1000
#define MANUAL_READ_COILS 0x01
#define MANUAL_READ_DISCRETE 0x02
#define MANUAL_READ_HOLDING 0x03
#define MANUAL_READ_INPUT 0x04
#define MANUAL_WRITE_SINGLE_COIL 0x05
#define MANUAL_WRITE_SINGLE_REG 0x06
#define MANUAL_WRITE_MULTIPLE_COILS 0x0F
#define MANUAL_WRITE_MULTIPLE_REGS 0x10
#define MANUAL_MAX_BIT_READ_COUNT 2000
#define MANUAL_MAX_REGISTER_READ_COUNT 125
#define MANUAL_CACHE_STOP_WAIT_MS 1500

// 写请求状态管理（手动模式自有定义）
typedef enum {
  WRITE_STATE_IDLE = 0,
  WRITE_STATE_WAITING_RESPONSE,
} write_state_enum_t;

typedef struct {
  write_state_enum_t state;
  uint8_t slave_id;
  uint8_t function_code;
  int source_channel;
  uint64_t start_time;
  int timeout_ms;
  uint16_t start_addr;
  uint16_t write_value;
  uint16_t register_count;
} write_request_state_t;

// 转发任务参数（手动模式自有定义）
typedef struct {
  uint8_t request[64];
  int request_len;
  int source_channel;
  int timeout_ms;
} ForwardTaskParam_t;

// 融合策略已移除

// 手动缓存全局管理器（仅手动模式使用）
static ManualCacheManager_t g_manual_cache = {0};

// 手动模式内部状态
static write_request_state_t manual_write_state = {0};
static bool manual_poll_suspended = false;
static volatile bool manual_cache_stopping = false;
static volatile bool manual_polling_task_exited = true;
static volatile bool manual_response_task_exited = true;
// 写请求映射状态（仅允许并发1个写）
static bool manual_write_mapped = false;
// 映射已取消：保留开关变量以兼容，但不再使用其他地址字段

// 手动模式本地通道超时（独立于自动模式）
static int timeout1_m = 1000, timeout2_m = 1000, timeout3_m = 1000;

// 工具函数：当前时间(ms)
static inline uint64_t manual_get_current_time_ms(void) {
  return (uint64_t)(esp_timer_get_time() / 1000);
}

static inline bool manual_is_supported_read(uint8_t function_code) {
  return function_code == MANUAL_READ_COILS ||
         function_code == MANUAL_READ_DISCRETE ||
         function_code == MANUAL_READ_HOLDING ||
         function_code == MANUAL_READ_INPUT;
}

static inline bool manual_is_bit_read(uint8_t function_code) {
  return function_code == MANUAL_READ_COILS ||
         function_code == MANUAL_READ_DISCRETE;
}

static inline size_t manual_payload_len(uint8_t function_code,
                                        uint16_t quantity) {
  if (quantity == 0)
    return 0;
  return manual_is_bit_read(function_code) ? ((size_t)quantity + 7U) / 8U
                                           : (size_t)quantity * 2U;
}

static inline uint16_t manual_max_quantity(uint8_t function_code) {
  return manual_is_bit_read(function_code) ? MANUAL_MAX_BIT_READ_COUNT
                                           : MANUAL_MAX_REGISTER_READ_COUNT;
}

static inline bool manual_valid_read_quantity(uint8_t function_code,
                                              uint16_t quantity) {
  return manual_is_supported_read(function_code) && quantity > 0 &&
         quantity <= manual_max_quantity(function_code);
}

static inline bool manual_valid_u16_range(uint16_t start_addr,
                                          uint16_t quantity) {
  return quantity > 0 &&
         (uint32_t)start_addr + (uint32_t)quantity <= 0x10000U;
}

static inline bool manual_valid_read_range(uint8_t function_code,
                                           uint16_t start_addr,
                                           uint16_t quantity) {
  return manual_valid_read_quantity(function_code, quantity) &&
         manual_valid_u16_range(start_addr, quantity);
}

static inline bool manual_get_bit(const uint8_t *payload, uint16_t bit_index) {
  return ((payload[bit_index / 8] >> (bit_index % 8)) & 0x01) != 0;
}

static inline void manual_set_bit(uint8_t *payload, uint16_t bit_index,
                                  bool value) {
  uint8_t mask = (uint8_t)(1U << (bit_index % 8));
  if (value) {
    payload[bit_index / 8] |= mask;
  } else {
    payload[bit_index / 8] &= (uint8_t)~mask;
  }
}

static inline void manual_mask_unused_bits(ManualRealDataTable_t *rt) {
  if (!rt || !rt->data_buffer || !manual_is_bit_read(rt->function_code) ||
      rt->data_length == 0)
    return;
  uint8_t used_bits = rt->register_count % 8;
  if (used_bits != 0) {
    rt->data_buffer[rt->data_length - 1] &=
        (uint8_t)((1U << used_bits) - 1U);
  }
}

static inline bool manual_copy_cached_value(const ManualRealDataTable_t *rt,
                                            uint16_t real_index,
                                            uint8_t *combined,
                                            uint16_t combined_index) {
  if (!rt || !rt->data_buffer || !combined || real_index >= rt->register_count)
    return false;

  if (manual_is_bit_read(rt->function_code)) {
    if ((size_t)(real_index / 8) >= rt->data_length)
      return false;
    manual_set_bit(combined, combined_index,
                   manual_get_bit(rt->data_buffer, real_index));
    return true;
  }

  size_t real_offset = (size_t)real_index * 2U;
  size_t combined_offset = (size_t)combined_index * 2U;
  if (real_offset + 1U >= rt->data_length)
    return false;

  combined[combined_offset] = rt->data_buffer[real_offset];
  combined[combined_offset + 1U] = rt->data_buffer[real_offset + 1U];
  return true;
}

static inline bool manual_write_matches_table(uint8_t write_function,
                                              uint8_t table_function) {
  if (write_function == MANUAL_WRITE_SINGLE_COIL ||
      write_function == MANUAL_WRITE_MULTIPLE_COILS) {
    return table_function == MANUAL_READ_COILS;
  }
  if (write_function == MANUAL_WRITE_SINGLE_REG ||
      write_function == MANUAL_WRITE_MULTIPLE_REGS) {
    return table_function == MANUAL_READ_HOLDING;
  }
  return false;
}

static inline uint8_t manual_write_target_function(uint8_t function_code) {
  if (function_code == MANUAL_WRITE_SINGLE_COIL ||
      function_code == MANUAL_WRITE_MULTIPLE_COILS) {
    return MANUAL_READ_COILS;
  }
  if (function_code == MANUAL_WRITE_SINGLE_REG ||
      function_code == MANUAL_WRITE_MULTIPLE_REGS) {
    return MANUAL_READ_HOLDING;
  }
  return 0;
}

static inline uint16_t manual_write_quantity(uint8_t function_code,
                                             uint16_t request_count) {
  return (function_code == MANUAL_WRITE_SINGLE_COIL ||
          function_code == MANUAL_WRITE_SINGLE_REG)
             ? 1
             : request_count;
}

static void manual_send_exception(uint8_t slave_addr, uint8_t function_code,
                                  uint8_t exception_code,
                                  int source_channel) {
  uint8_t err[5];
  err[0] = slave_addr;
  err[1] = function_code | 0x80;
  err[2] = exception_code;
  uint16_t crc = calculate_crc(err, 3);
  err[3] = crc & 0xFF;
  err[4] = (crc >> 8) & 0xFF;
  tx_tasks_to_channel(err, sizeof(err), source_channel);
}

static void manual_invalidate_written_range(uint8_t slave_addr,
                                            uint8_t write_function,
                                            uint16_t start_addr,
                                            uint16_t quantity) {
  uint8_t read_function = manual_write_target_function(write_function);
  if (read_function == 0 || quantity == 0 || g_manual_cache.mutex == NULL)
    return;

  uint32_t write_start = start_addr;
  uint32_t write_end = write_start + quantity;
  int invalidated = 0;

  xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
  for (int i = 0; i < g_manual_cache.real_table_count; i++) {
    ManualRealDataTable_t *rt = &g_manual_cache.real_tables[i];
    uint32_t cache_start = rt->start_register;
    uint32_t cache_end = cache_start + rt->register_count;
    if (rt->real_slave_addr == slave_addr && rt->function_code == read_function &&
        write_start < cache_end && write_end > cache_start) {
      rt->data_valid = false;
      rt->last_update_time = 0;
      invalidated++;
    }
  }
  xSemaphoreGive(g_manual_cache.mutex);

  if (invalidated > 0) {
    ESP_LOGI(TAG_MANUAL,
             "写操作后失效手动缓存: slave=%u fc=0x%02X addr=%u qty=%u "
             "entries=%d",
             slave_addr, read_function, start_addr, quantity, invalidated);
  }
}

// 工具函数：校验Modbus帧CRC
static inline bool manual_verify_crc(const uint8_t *frame, int total_length) {
  if (total_length < 3)
    return false;
  uint16_t recv_crc = (frame[total_length - 1] << 8) | frame[total_length - 2];
  uint16_t calc_crc = calculate_crc((uint8_t *)frame, (size_t)total_length - 2);
  return recv_crc == calc_crc;
}

static int manual_valid_passthrough_response_len(const uint8_t *rsp, int len,
                                                 uint8_t slave_addr,
                                                 uint8_t function_code) {
  if (rsp == NULL || len < 5 || rsp[0] != slave_addr)
    return 0;

  if (rsp[1] == (uint8_t)(function_code | 0x80)) {
    return manual_verify_crc(rsp, 5) ? 5 : 0;
  }

  if (rsp[1] != function_code)
    return 0;

  int frame_len = 0;
  switch (function_code) {
  case MANUAL_READ_COILS:
  case MANUAL_READ_DISCRETE:
  case MANUAL_READ_HOLDING:
  case MANUAL_READ_INPUT:
    frame_len = 3 + rsp[2] + 2;
    break;
  case MANUAL_WRITE_SINGLE_COIL:
  case MANUAL_WRITE_SINGLE_REG:
  case MANUAL_WRITE_MULTIPLE_COILS:
  case MANUAL_WRITE_MULTIPLE_REGS:
    frame_len = 8;
    break;
  default:
    return 0;
  }

  if (frame_len <= len && manual_verify_crc(rsp, frame_len))
    return frame_len;
  return 0;
}

// 前置声明（仅手动模式内部）
static esp_err_t load_manual_config_from_nvs(nvs_handle_t storage_handle);
static esp_err_t build_manual_dual_tables(void *temp_items, int valid_items);
static void manual_polling_task(void *pvParameter);
static void manual_response_task(void *pvParameter);
static void process_manual_modbus_request(uint8_t *request, int request_len,
                                          int source_channel);
static void check_manual_write_response(void);

static bool manual_wait_task_exit(volatile bool *exited, uint32_t timeout_ms) {
  uint32_t waited_ms = 0;
  while (!*exited && waited_ms < timeout_ms) {
    vTaskDelay(pdMS_TO_TICKS(20));
    waited_ms += 20;
  }
  return *exited;
}

static void manual_free_tables_only(void) {
  if (g_manual_cache.real_tables != NULL) {
    for (int i = 0; i < g_manual_cache.real_table_count; i++) {
      free(g_manual_cache.real_tables[i].data_buffer);
      g_manual_cache.real_tables[i].data_buffer = NULL;
    }
    free(g_manual_cache.real_tables);
    g_manual_cache.real_tables = NULL;
  }
  g_manual_cache.real_table_count = 0;

  if (g_manual_cache.mapping_tables != NULL) {
    free(g_manual_cache.mapping_tables);
    g_manual_cache.mapping_tables = NULL;
  }
  g_manual_cache.mapping_table_count = 0;
}

void stop_manual_cache_tasks(void) {
  static const char *TAG = "STOP_MANUAL_CACHE";
  bool cleanup_allowed = true;

  manual_cache_stopping = true;
  manual_poll_suspended = false;
  manual_write_state.state = WRITE_STATE_IDLE;

  if (g_manual_cache.polling_task != NULL) {
    if (!manual_wait_task_exit(&manual_polling_task_exited,
                               MANUAL_CACHE_STOP_WAIT_MS)) {
      ESP_LOGW(TAG, "手动轮询任务未及时退出，强制删除");
      delete_app_task_with_caps(g_manual_cache.polling_task);
      cleanup_allowed = false;
    }
    g_manual_cache.polling_task = NULL;
    ESP_LOGI(TAG, "手动轮询任务已停止");
  }

  if (g_manual_cache.response_task != NULL) {
    if (!manual_wait_task_exit(&manual_response_task_exited,
                               MANUAL_CACHE_STOP_WAIT_MS)) {
      ESP_LOGW(TAG, "手动响应任务未及时退出，强制删除");
      delete_app_task_with_caps(g_manual_cache.response_task);
      cleanup_allowed = false;
    }
    g_manual_cache.response_task = NULL;
    ESP_LOGI(TAG, "手动响应任务已停止");
  }

  if (cleanup_allowed) {
    manual_free_tables_only();

    if (g_manual_cache.mutex != NULL) {
      vSemaphoreDelete(g_manual_cache.mutex);
      g_manual_cache.mutex = NULL;
    }
  } else {
    ESP_LOGW(TAG, "手动缓存任务为强制删除，跳过共享资源释放以避免并发释放");
  }

  manual_cache_stopping = false;
  manual_polling_task_exited = true;
  manual_response_task_exited = true;

  ESP_LOGI(TAG, "手动缓存系统已完全清理");
}

static esp_err_t load_manual_config_from_nvs(nvs_handle_t storage_handle) {
  typedef struct {
    bool enabled;
    uint8_t real_slave_addr;
    uint8_t function_code;
    uint16_t real_start_reg;
    uint16_t register_count;
    uint8_t virtual_slave_addr;
    uint16_t virtual_start_reg;
    channel_uart_config_t uart_config;
    uint32_t poll_interval_ms;
    uint32_t timeout_ms;
  } TempConfigItem;

  // 优先读取Blob配置，减少NVS条目占用
  size_t blob_size = 0;
  esp_err_t ret = nvs_get_blob(storage_handle, "m_blob", NULL, &blob_size);
  if (ret == ESP_OK && blob_size >= sizeof(ModbusCacheItemBlob)) {
    int items_count = (int)(blob_size / sizeof(ModbusCacheItemBlob));
    if (items_count > MODBUS_CACHE_MAX_ITEMS) {
      items_count = MODBUS_CACHE_MAX_ITEMS;
    }

    ModbusCacheItemBlob *blob_items = (ModbusCacheItemBlob *)malloc(blob_size);
    if (!blob_items) {
      return ESP_ERR_NO_MEM;
    }

    ret = nvs_get_blob(storage_handle, "m_blob", blob_items, &blob_size);
    if (ret == ESP_OK) {
      TempConfigItem *temp_items = calloc(items_count, sizeof(TempConfigItem));
      if (!temp_items) {
        free(blob_items);
        return ESP_ERR_NO_MEM;
      }

      int valid_items = 0;
      for (int i = 0; i < items_count; i++) {
        if (!blob_items[i].enabled) {
          continue;
        }

        TempConfigItem *dst = &temp_items[valid_items];
        dst->enabled = true;
        dst->real_slave_addr = blob_items[i].slave_addr;
        dst->function_code = blob_items[i].function_code;
        dst->real_start_reg = blob_items[i].register_addr;
        dst->register_count = blob_items[i].register_num;
        dst->virtual_slave_addr = blob_items[i].mapped_slave_addr;
        dst->virtual_start_reg = blob_items[i].mapped_register_addr;

        dst->uart_config.baudrate =
            (blob_items[i].baud_rate > 0) ? blob_items[i].baud_rate : 9600;

        switch (blob_items[i].data_bit) {
        case 5:
          dst->uart_config.data_bits = UART_DATA_5_BITS;
          break;
        case 6:
          dst->uart_config.data_bits = UART_DATA_6_BITS;
          break;
        case 7:
          dst->uart_config.data_bits = UART_DATA_7_BITS;
          break;
        default:
          dst->uart_config.data_bits = UART_DATA_8_BITS;
          break;
        }

        if (blob_items[i].check_bit == 1) {
          dst->uart_config.parity = UART_PARITY_ODD;
        } else if (blob_items[i].check_bit == 2) {
          dst->uart_config.parity = UART_PARITY_EVEN;
        } else {
          dst->uart_config.parity = UART_PARITY_DISABLE;
        }

        if (blob_items[i].stop_bits_x2 == 3) {
          dst->uart_config.stop_bits = UART_STOP_BITS_1_5;
        } else if (blob_items[i].stop_bits_x2 == 4) {
          dst->uart_config.stop_bits = UART_STOP_BITS_2;
        } else {
          dst->uart_config.stop_bits = UART_STOP_BITS_1;
        }

        dst->uart_config.frame_len = 512;
        dst->uart_config.channel = 3;

        int interval_time = (int)blob_items[i].interval_ms;
        if (interval_time < 1)
          interval_time = 1;
        if (interval_time > 1000)
          interval_time = 1000;
        dst->uart_config.frame_time = interval_time;
        dst->poll_interval_ms = MANUAL_CACHE_POLL_INTERVAL_MS;
        dst->timeout_ms = (blob_items[i].timeout_ms > 0)
                              ? blob_items[i].timeout_ms
                              : MANUAL_CACHE_DEFAULT_TIMEOUT_MS;

        if (!manual_valid_read_range(dst->function_code, dst->real_start_reg,
                                     dst->register_count) ||
            !manual_valid_read_range(dst->function_code,
                                     dst->virtual_start_reg,
                                     dst->register_count)) {
          ESP_LOGW(TAG_MANUAL,
                   "跳过无效手动缓存项: slave=%u fc=0x%02X addr=%u mapped=%u qty=%u",
                   dst->real_slave_addr, dst->function_code,
                   dst->real_start_reg, dst->virtual_start_reg,
                   dst->register_count);
          continue;
        }

        valid_items++;
      }

      free(blob_items);
      if (valid_items == 0) {
        free(temp_items);
        return ESP_ERR_NOT_FOUND;
      }

      esp_err_t build_ret = build_manual_dual_tables(temp_items, valid_items);
      free(temp_items);
      return build_ret;
    }

    free(blob_items);
  }

  // 读取 items -> 构建临时数组（旧格式）
  int32_t items_count = 0;
  ret = nvs_get_i32(storage_handle, "m_count", &items_count);
  if (ret != ESP_OK || items_count <= 0) {
    ESP_LOGW(TAG_MANUAL, "未找到有效的手动配置: count=%d, ret=%s",
             (int)items_count, esp_err_to_name(ret));
    return ESP_ERR_NOT_FOUND;
  }
  if (items_count > MODBUS_CACHE_MAX_ITEMS) {
    ESP_LOGW(TAG_MANUAL, "手动缓存条目超出上限(%d > %d)，已截断",
             (int)items_count, MODBUS_CACHE_MAX_ITEMS);
    items_count = MODBUS_CACHE_MAX_ITEMS;
  }

  TempConfigItem *temp_items = calloc(items_count, sizeof(TempConfigItem));
  if (!temp_items)
    return ESP_ERR_NO_MEM;

  int valid_items = 0;
  for (int i = 0; i < items_count; i++) {
    char key[32];
    char value[16];
    size_t size;

    snprintf(key, sizeof(key), "m%d_en", i);
    uint8_t enabled = 0;
    if (nvs_get_u8(storage_handle, key, &enabled) == ESP_OK && enabled) {
      temp_items[valid_items].enabled = true;

      snprintf(key, sizeof(key), "m%ds_addr", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].real_slave_addr = (uint8_t)atoi(value);

      snprintf(key, sizeof(key), "m%df_code", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].function_code = (uint8_t)atoi(value);

      snprintf(key, sizeof(key), "m%dr_addr", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].real_start_reg = (uint16_t)atoi(value);

      snprintf(key, sizeof(key), "m%dr_num", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].register_count = (uint16_t)atoi(value);

      snprintf(key, sizeof(key), "m%dm_s_addr", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].virtual_slave_addr = (uint8_t)atoi(value);

      snprintf(key, sizeof(key), "m%dm_r_addr", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].virtual_start_reg = (uint16_t)atoi(value);

      // UART
      snprintf(key, sizeof(key), "m%dbaud_rate", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].uart_config.baudrate = atoi(value);
      else
        temp_items[valid_items].uart_config.baudrate = 9600;

      snprintf(key, sizeof(key), "m%ddata_bit", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK) {
        int data_bits = atoi(value);
        switch (data_bits) {
        case 5:
          temp_items[valid_items].uart_config.data_bits = UART_DATA_5_BITS;
          break;
        case 6:
          temp_items[valid_items].uart_config.data_bits = UART_DATA_6_BITS;
          break;
        case 7:
          temp_items[valid_items].uart_config.data_bits = UART_DATA_7_BITS;
          break;
        default:
          temp_items[valid_items].uart_config.data_bits = UART_DATA_8_BITS;
          break;
        }
      } else {
        temp_items[valid_items].uart_config.data_bits = UART_DATA_8_BITS;
      }

      snprintf(key, sizeof(key), "m%dcheck_bit", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK) {
        if (strcmp(value, "1") == 0)
          temp_items[valid_items].uart_config.parity = UART_PARITY_ODD;
        else if (strcmp(value, "2") == 0)
          temp_items[valid_items].uart_config.parity = UART_PARITY_EVEN;
        else
          temp_items[valid_items].uart_config.parity = UART_PARITY_DISABLE;
      } else {
        temp_items[valid_items].uart_config.parity = UART_PARITY_DISABLE;
      }

      snprintf(key, sizeof(key), "m%dstop_bit", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK) {
        if (strcmp(value, "1.5") == 0)
          temp_items[valid_items].uart_config.stop_bits = UART_STOP_BITS_1_5;
        else if (strcmp(value, "2") == 0)
          temp_items[valid_items].uart_config.stop_bits = UART_STOP_BITS_2;
        else
          temp_items[valid_items].uart_config.stop_bits = UART_STOP_BITS_1;
      } else {
        temp_items[valid_items].uart_config.stop_bits = UART_STOP_BITS_1;
      }

      temp_items[valid_items].uart_config.frame_len = 512;
      temp_items[valid_items].uart_config.channel = 3;

      // 间隔/轮询
      snprintf(key, sizeof(key), "m%di_time", i);
      size = sizeof(value);
      int interval_time = 50;
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK) {
        interval_time = atoi(value);
        if (interval_time < 1)
          interval_time = 1;
        if (interval_time > 1000)
          interval_time = 1000;
      }
      temp_items[valid_items].uart_config.frame_time = interval_time;
      temp_items[valid_items].poll_interval_ms = MANUAL_CACHE_POLL_INTERVAL_MS;

      // 超时
      snprintf(key, sizeof(key), "m%dtimeout", i);
      size = sizeof(value);
      if (nvs_get_str(storage_handle, key, value, &size) == ESP_OK)
        temp_items[valid_items].timeout_ms = atoi(value);
      else
        temp_items[valid_items].timeout_ms = MANUAL_CACHE_DEFAULT_TIMEOUT_MS;

      if (!manual_valid_read_range(temp_items[valid_items].function_code,
                                   temp_items[valid_items].real_start_reg,
                                   temp_items[valid_items].register_count) ||
          !manual_valid_read_range(temp_items[valid_items].function_code,
                                   temp_items[valid_items].virtual_start_reg,
                                   temp_items[valid_items].register_count)) {
        ESP_LOGW(TAG_MANUAL,
                 "跳过无效手动缓存项: slave=%u fc=0x%02X addr=%u mapped=%u qty=%u",
                 temp_items[valid_items].real_slave_addr,
                 temp_items[valid_items].function_code,
                 temp_items[valid_items].real_start_reg,
                 temp_items[valid_items].virtual_start_reg,
                 temp_items[valid_items].register_count);
        continue;
      }

      valid_items++;
    }
  }

  if (valid_items == 0) {
    free(temp_items);
    return ESP_ERR_NOT_FOUND;
  }

  esp_err_t build_ret = build_manual_dual_tables(temp_items, valid_items);
  free(temp_items);
  return build_ret;
}

static esp_err_t build_manual_dual_tables(void *temp_items, int valid_items) {
  typedef struct {
    bool enabled;
    uint8_t real_slave_addr;
    uint8_t function_code;
    uint16_t real_start_reg;
    uint16_t register_count;
    uint8_t virtual_slave_addr;
    uint16_t virtual_start_reg;
    channel_uart_config_t uart_config;
    uint32_t poll_interval_ms;
    uint32_t timeout_ms;
  } TempConfigItem;

  TempConfigItem *items = (TempConfigItem *)temp_items;

  // 完全删除融合策略：每个配置项独立生成一个真实表项
  g_manual_cache.real_tables =
      calloc(valid_items, sizeof(ManualRealDataTable_t));
  if (!g_manual_cache.real_tables)
    return ESP_ERR_NO_MEM;
  g_manual_cache.real_table_count = valid_items;

  for (int i = 0; i < valid_items; i++) {
    TempConfigItem *it = &items[i];
    ManualRealDataTable_t *rt = &g_manual_cache.real_tables[i];
    rt->real_slave_addr = it->real_slave_addr;
    rt->function_code = it->function_code;
    rt->start_register = it->real_start_reg;
    rt->register_count = it->register_count;
    rt->data_length = manual_payload_len(rt->function_code, rt->register_count);
    rt->uart_config = it->uart_config;
    rt->poll_interval_ms = it->poll_interval_ms;
    rt->timeout_ms = it->timeout_ms;
    rt->data_valid = false;
    rt->data_healthy = true;
    rt->fail_count = 0;
    rt->last_update_time = 0;
    rt->data_buffer = calloc(rt->data_length, sizeof(uint8_t));
    if (!rt->data_buffer) {
      // 释放已分配的data_buffer与real_tables
      for (int k = 0; k < i; k++) {
        if (g_manual_cache.real_tables[k].data_buffer)
          free(g_manual_cache.real_tables[k].data_buffer);
      }
      free(g_manual_cache.real_tables);
      g_manual_cache.real_tables = NULL;
      g_manual_cache.real_table_count = 0;
      return ESP_ERR_NO_MEM;
    }
  }

  // 为每个配置项建立一一对应的映射表项
  g_manual_cache.mapping_tables =
      calloc(valid_items, sizeof(ManualMappingTable_t));
  if (!g_manual_cache.mapping_tables) {
    for (int k = 0; k < g_manual_cache.real_table_count; k++)
      free(g_manual_cache.real_tables[k].data_buffer);
    free(g_manual_cache.real_tables);
    g_manual_cache.real_tables = NULL;
    g_manual_cache.real_table_count = 0;
    return ESP_ERR_NO_MEM;
  }

  for (int i = 0; i < valid_items; i++) {
    TempConfigItem *it = &items[i];
    ManualMappingTable_t *mp = &g_manual_cache.mapping_tables[i];
    mp->virtual_slave_addr = it->virtual_slave_addr;
    mp->virtual_start_reg = it->virtual_start_reg;
    mp->register_count = it->register_count;
    mp->real_table_ref = &g_manual_cache.real_tables[i];
    mp->real_offset = 0; // 一一对应，无偏移
  }
  g_manual_cache.mapping_table_count = valid_items;
  return ESP_OK;
}

esp_err_t init_manual_polling_cache(nvs_handle_t storage_handle) {
  ESP_LOGI(TAG_MANUAL, "=== 初始化手动配置双表缓存系统 ===");

  manual_cache_stopping = false;
  manual_poll_suspended = false;
  manual_write_state.state = WRITE_STATE_IDLE;
  manual_polling_task_exited = true;
  manual_response_task_exited = true;

  // 读取通道超时（手动模式自有副本）
  const char *keys[] = {"ch1_timeout", "ch2_timeout", "ch3_timeout"};
  size_t sizes[3] = {0};
  for (int i = 0; i < 3; i++) {
    nvs_get_str(storage_handle, keys[i], NULL, &sizes[i]);
  }
  char *vals[3] = {0};
  for (int i = 0; i < 3; i++) {
    if (sizes[i] > 0) {
      vals[i] = (char *)malloc(sizes[i]);
      if (vals[i])
        nvs_get_str(storage_handle, keys[i], vals[i], &sizes[i]);
    }
  }
  timeout1_m = vals[0] ? atoi(vals[0]) : 1000;
  timeout2_m = vals[1] ? atoi(vals[1]) : 1000;
  timeout3_m = vals[2] ? atoi(vals[2]) : 1000;
  for (int i = 0; i < 3; i++)
    if (vals[i])
      free(vals[i]);

  g_manual_cache.mutex = xSemaphoreCreateMutex();
  if (!g_manual_cache.mutex)
    return ESP_ERR_NO_MEM;

  esp_err_t ret = load_manual_config_from_nvs(storage_handle);
  nvs_close(storage_handle);
  if (ret != ESP_OK) {
    manual_free_tables_only();
    vSemaphoreDelete(g_manual_cache.mutex);
    g_manual_cache.mutex = NULL;
    return ret;
  }

  manual_polling_task_exited = false;
  BaseType_t tr =
      create_app_task_psram(manual_polling_task, "manual_polling", 20480, NULL,
                            2, &g_manual_cache.polling_task, SX_WORK_CORE_ID);
  if (tr != pdPASS) {
    manual_polling_task_exited = true;
    manual_free_tables_only();
    vSemaphoreDelete(g_manual_cache.mutex);
    g_manual_cache.mutex = NULL;
    return ESP_FAIL;
  }
  manual_response_task_exited = false;
  tr = create_app_task_psram(manual_response_task, "manual_response", 16384,
                             NULL, 15, &g_manual_cache.response_task,
                             SX_WORK_CORE_ID);
  if (tr != pdPASS) {
    manual_response_task_exited = true;
    stop_manual_cache_tasks();
    return ESP_FAIL;
  }
  ESP_LOGI(TAG_MANUAL, "手动双表缓存系统初始化完成");
  return ESP_OK;
}

static void manual_polling_task(void *pvParameter) {
  ESP_LOGI(TAG_MANUAL, "手动轮询任务启动");
  manual_polling_task_exited = false;
  while (!manual_cache_stopping) {
    if (manual_poll_suspended) {
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
    uint64_t now_ms = manual_get_current_time_ms();
    for (int i = 0; i < g_manual_cache.real_table_count; i++) {
      if (manual_cache_stopping) {
        break;
      }
      ManualRealDataTable_t *rt = &g_manual_cache.real_tables[i];
      if (now_ms - rt->last_update_time >= rt->poll_interval_ms) {
        uint8_t req[8];
        req[0] = rt->real_slave_addr;
        req[1] = rt->function_code;
        req[2] = (rt->start_register >> 8) & 0xFF;
        req[3] = rt->start_register & 0xFF;
        req[4] = (rt->register_count >> 8) & 0xFF;
        req[5] = rt->register_count & 0xFF;
        uint16_t crc = calculate_crc(req, 6);
        req[6] = crc & 0xFF;
        req[7] = (crc >> 8) & 0xFF;
        xSemaphoreGive(g_manual_cache.mutex);

        if (send_data_with_temp_config(3, &rt->uart_config, req, sizeof(req)) ==
            ESP_OK) {
          uint8_t rsp[1024];
          uint64_t ts;
          int waited = 0;
          bool ok = false;
          while (!manual_cache_stopping && waited < (int)rt->timeout_ms && !ok) {
            int n = pop_channel_data(3, rsp, sizeof(rsp), &ts);
            if (n > 0) {
              if (n >= 5 && rsp[0] == rt->real_slave_addr &&
                  rsp[1] == rt->function_code) {
                uint8_t dl = rsp[2];
                size_t frame_len = 3U + (size_t)dl + 2U;
                if (dl == rt->data_length && (size_t)n >= frame_len) {
                  if (manual_verify_crc(rsp, (int)frame_len)) {
                    xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
                    memcpy(rt->data_buffer, &rsp[3], dl);
                    manual_mask_unused_bits(rt);
                    rt->data_valid = true;
                    rt->data_healthy = true;
                    rt->fail_count = 0;
                    rt->last_update_time = manual_get_current_time_ms();
                    xSemaphoreGive(g_manual_cache.mutex);
                    ok = true;
                  } else {
                    ESP_LOGW(TAG_MANUAL,
                             "清理CRC错误手动轮询响应: len=%d frame_len=%zu "
                             "slave=%u fc=0x%02X",
                             n, frame_len, rt->real_slave_addr,
                             rt->function_code);
                  }
                } else {
                  ESP_LOGW(TAG_MANUAL,
                           "手动轮询回包长度不匹配: slave=%u fc=0x%02X "
                           "expect=%u actual=%u n=%d",
                           rt->real_slave_addr, rt->function_code,
                           (unsigned)rt->data_length, dl, n);
                }
              } else {
                ESP_LOGW(TAG_MANUAL,
                         "清理非目标手动轮询响应: len=%d got_slave=%u "
                         "got_fc=0x%02X expect_slave=%u expect_fc=0x%02X",
                         n, (n > 0) ? rsp[0] : 0, (n > 1) ? rsp[1] : 0,
                         rt->real_slave_addr, rt->function_code);
              }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            waited += 10;
          }
          if (!manual_cache_stopping && !ok) {
            xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
            if (rt->fail_count < 255)
              rt->fail_count++;
            if (rt->fail_count >= 3) {
              rt->data_healthy = false;
              rt->data_valid = false;
            }
            xSemaphoreGive(g_manual_cache.mutex);
          }
          uint32_t safety = rt->timeout_ms / 20;
          if (safety < 30)
            safety = 30;
          if (safety > 300)
            safety = 300;
          if (!manual_cache_stopping) {
            vTaskDelay(pdMS_TO_TICKS(safety));
          }
        } else {
          xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
          rt->last_update_time = now_ms;
          if (rt->fail_count < 255)
            rt->fail_count++;
          if (rt->fail_count >= 3) {
            rt->data_healthy = false;
            rt->data_valid = false;
          }
          xSemaphoreGive(g_manual_cache.mutex);
        }
        xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
      }
    }
    xSemaphoreGive(g_manual_cache.mutex);
    if (!manual_cache_stopping) {
      vTaskDelay(pdMS_TO_TICKS(150));
    }
  }

  manual_polling_task_exited = true;
  ESP_LOGI(TAG_MANUAL, "手动轮询任务退出");
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

static void check_manual_write_response(void) {
  if (manual_cache_stopping)
    return;
  if (manual_write_state.state == WRITE_STATE_IDLE)
    return;
  uint64_t now = manual_get_current_time_ms();
  if (now - manual_write_state.start_time > (uint64_t)manual_write_state.timeout_ms) {
    uint8_t timeout_rsp[5];
    timeout_rsp[0] = manual_write_state.slave_id;
    timeout_rsp[1] = manual_write_state.function_code | 0x80;
    timeout_rsp[2] = 0x0B;
    uint16_t crc = calculate_crc(timeout_rsp, 3);
    timeout_rsp[3] = crc & 0xFF;
    timeout_rsp[4] = (crc >> 8) & 0xFF;
    tx_tasks_to_channel(timeout_rsp, 5, manual_write_state.source_channel);
    manual_write_state.state = WRITE_STATE_IDLE;
    manual_poll_suspended = false;
    return;
  }
  uint8_t rsp[1024];
  uint64_t ts;
  int n = pop_channel_data(3, rsp, sizeof(rsp), &ts);
  if (n > 0 && manual_write_state.state == WRITE_STATE_WAITING_RESPONSE) {
    if (n < 2) {
      ESP_LOGW(TAG_MANUAL, "清理过短手动写响应: len=%d", n);
      return;
    }
    if (rsp[0] == manual_write_state.slave_id &&
        (rsp[1] == manual_write_state.function_code ||
         rsp[1] == (manual_write_state.function_code | 0x80))) {
      int frame_len =
          (rsp[1] & 0x80)
              ? 5
              : ((rsp[1] == MANUAL_WRITE_SINGLE_COIL ||
                  rsp[1] == MANUAL_WRITE_SINGLE_REG ||
                  rsp[1] == MANUAL_WRITE_MULTIPLE_COILS ||
                  rsp[1] == MANUAL_WRITE_MULTIPLE_REGS)
                     ? 8
                     : n);
      if (frame_len <= n && manual_verify_crc(rsp, frame_len)) {
        // 直接透传响应，不做地址或寄存器回写
        if (!(rsp[1] & 0x80)) {
          manual_invalidate_written_range(
              manual_write_state.slave_id, manual_write_state.function_code,
              manual_write_state.start_addr, manual_write_state.register_count);
        }
        tx_tasks_to_channel(rsp, frame_len, manual_write_state.source_channel);
        manual_write_state.state = WRITE_STATE_IDLE;
        manual_poll_suspended = false;
      } else {
        manual_send_exception(manual_write_state.slave_id,
                              manual_write_state.function_code, 0x04,
                              manual_write_state.source_channel);
        manual_write_state.state = WRITE_STATE_IDLE;
        manual_poll_suspended = false;
      }
    } else {
      ESP_LOGW(TAG_MANUAL,
               "清理非目标手动写响应: len=%d got_slave=%u got_fc=0x%02X "
               "expect_slave=%u expect_fc=0x%02X",
               n, rsp[0], rsp[1], manual_write_state.slave_id,
               manual_write_state.function_code);
    }
  }
}

static void manual_response_task(void *pvParameter) {
  ESP_LOGI(TAG_MANUAL, "手动响应任务启动");
  manual_response_task_exited = false;
  uint8_t buffer[1024];
  uint64_t ts;
  while (!manual_cache_stopping) {
    check_manual_write_response();
    int n1 = pop_channel_data(1, buffer, sizeof(buffer), &ts);
    if (n1 >= 8) {
      process_manual_modbus_request(buffer, n1, 1);
    }
    if (manual_cache_stopping) {
      break;
    }
    int n2 = pop_channel_data(2, buffer, sizeof(buffer), &ts);
    if (n2 >= 8) {
      process_manual_modbus_request(buffer, n2, 2);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }

  manual_response_task_exited = true;
  ESP_LOGI(TAG_MANUAL, "手动响应任务退出");
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

static void process_manual_modbus_request(uint8_t *request, int request_len,
                                          int source_channel) {
  if (manual_cache_stopping)
    return;
  if (request_len < 8)
    return;
  if (!manual_verify_crc(request, request_len))
    return;
  uint8_t slave_addr = request[0];
  uint8_t function_code = request[1];
  uint16_t start_register = (request[2] << 8) | request[3];
  uint16_t register_count = (request[4] << 8) | request[5];

  // 写命令（仅允许访问已配置的真实从机与已配置地址范围）
  if (function_code == MANUAL_WRITE_SINGLE_COIL ||
      function_code == MANUAL_WRITE_SINGLE_REG ||
      function_code == MANUAL_WRITE_MULTIPLE_COILS ||
      function_code == MANUAL_WRITE_MULTIPLE_REGS) {
    if (manual_write_state.state != WRITE_STATE_IDLE) {
      manual_send_exception(slave_addr, function_code, 0x06, source_channel);
      return;
    }

    channel_uart_config_t uart_cfg;
    uint32_t to_ms = 1000;
    bool found = false;
    manual_write_mapped = false; // 写路径地址映射已删除
    // 仅当写入地址范围在已配置的真实表范围内才允许
    uint16_t w_count = manual_write_quantity(function_code, register_count);
    if (!manual_valid_u16_range(start_register, w_count)) {
      manual_send_exception(slave_addr, function_code, 0x03, source_channel);
      return;
    }
    xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
    // 直接尝试命中真实表（不做任何映射）
    for (int i = 0; i < g_manual_cache.real_table_count && !found; i++) {
      ManualRealDataTable_t *rt = &g_manual_cache.real_tables[i];
      if (manual_write_matches_table(function_code, rt->function_code) &&
          rt->real_slave_addr == slave_addr &&
          start_register >= rt->start_register &&
          (uint32_t)start_register + (uint32_t)w_count <= (uint32_t)rt->start_register + (uint32_t)rt->register_count) {
        uart_cfg = rt->uart_config;
        to_ms = rt->timeout_ms;
        found = true;
        manual_write_mapped = false;
      }
    }
    xSemaphoreGive(g_manual_cache.mutex);
    if (!found) {
      // 非白名单写：按来源主机参数转发到CH3
      uart_config_mode_t wm = get_uart_config_mode();
      // 清空CH3残留，避免粘包
      clear_channel_data(3);
      (void)smart_send_data_to_ch3(wm, source_channel, request, (size_t)request_len);
      // 等待CH3响应并透传
      uint8_t ch3r[1024];
      uint64_t ts3;
      int waited = 0;
      while (!manual_cache_stopping && waited < timeout3_m) {
        int n3 = pop_channel_data(3, ch3r, sizeof(ch3r), &ts3);
        if (n3 > 0) {
          int frame_len = manual_valid_passthrough_response_len(
              ch3r, n3, slave_addr, function_code);
          if (frame_len > 0) {
            tx_tasks_to_channel(ch3r, frame_len, source_channel);
            return;
          }
          ESP_LOGW(TAG_MANUAL,
                   "丢弃非目标/CRC错误写透传响应: len=%d slave=%u fc=0x%02X",
                   n3, ch3r[0], ch3r[1]);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
      }
      // 超时：回0x0B
      if (!manual_cache_stopping) {
        manual_send_exception(slave_addr, function_code, 0x0B, source_channel);
      }
      return;
    }
    manual_write_state.state = WRITE_STATE_WAITING_RESPONSE;
    manual_write_state.slave_id = request[0];
    manual_write_state.function_code = function_code;
    manual_write_state.source_channel = source_channel;
    manual_write_state.start_time = manual_get_current_time_ms();
    manual_write_state.timeout_ms = (int)to_ms;
    manual_write_state.start_addr = start_register;
    manual_write_state.register_count = w_count;
    if (function_code == MANUAL_WRITE_SINGLE_REG && request_len >= 8)
      manual_write_state.write_value = (request[4] << 8) | request[5];
    manual_poll_suspended = true;
    // 等待轮询彻底停下并清空CH3残留，避免与写冲突
    for (int waited = 0; !manual_cache_stopping && waited < 125; waited += 25) {
      vTaskDelay(pdMS_TO_TICKS(25));
    }
    if (manual_cache_stopping) {
      manual_write_state.state = WRITE_STATE_IDLE;
      manual_poll_suspended = false;
      return;
    }
    clear_channel_data(3);
    if (send_data_with_temp_config(3, &uart_cfg, request, request_len) != ESP_OK) {
      manual_write_state.state = WRITE_STATE_IDLE;
      manual_poll_suspended = false;
    }
    return;
  }

  // 读命令：01/02按bit缓存，03/04按寄存器字节缓存
  if (manual_is_supported_read(function_code)) {
    if (!manual_valid_read_range(function_code, start_register,
                                 register_count)) {
      manual_send_exception(slave_addr, function_code, 0x03, source_channel);
      return;
    }

    bool all_filled = true;
    bool blocked = false;
    bool waiting_refresh = false;
    uint8_t combined[1024] = {0};

    xSemaphoreTake(g_manual_cache.mutex, portMAX_DELAY);
    for (uint16_t idx = 0; idx < register_count; idx++) {
      uint16_t reg = start_register + idx;
      bool filled = false;

      // 1) 优先用虚拟映射
      for (int i = 0; i < g_manual_cache.mapping_table_count; i++) {
        ManualMappingTable_t *mp = &g_manual_cache.mapping_tables[i];
        if (mp->virtual_slave_addr == slave_addr &&
            reg >= mp->virtual_start_reg &&
            (uint32_t)reg < (uint32_t)mp->virtual_start_reg +
                                (uint32_t)mp->register_count) {
          ManualRealDataTable_t *rt = mp->real_table_ref;

          if (rt == NULL || rt->function_code != function_code) {
            continue;
          }

          if (!rt->data_healthy) {
            blocked = true;
            break;
          }

          if (!rt->data_valid) {
            waiting_refresh = true;
            break;
          }

          uint16_t v_off = reg - mp->virtual_start_reg;
          uint16_t real_index = mp->real_offset + v_off;
          filled = manual_copy_cached_value(rt, real_index, combined, idx);
          break; // 同功能码映射命中后退出
        }
      }

      if (blocked || waiting_refresh)
        break;
      if (filled)
        continue;

      // 2) 未命中映射，则尝试直接真实表
      for (int i = 0; i < g_manual_cache.real_table_count; i++) {
        ManualRealDataTable_t *rt = &g_manual_cache.real_tables[i];
        if (rt->real_slave_addr == slave_addr &&
            rt->function_code == function_code && reg >= rt->start_register &&
            (uint32_t)reg <
                (uint32_t)rt->start_register + (uint32_t)rt->register_count) {
          if (!rt->data_healthy) {
            blocked = true;
            break;
          }

          if (!rt->data_valid) {
            waiting_refresh = true;
            break;
          }

          uint16_t real_index = reg - rt->start_register;
          if (manual_copy_cached_value(rt, real_index, combined, idx)) {
            filled = true;
            break;
          }
        }
      }

      if (blocked || waiting_refresh)
        break;
      if (!filled) {
        all_filled = false;
        break;
      }
    }
    if (blocked || waiting_refresh) {
      xSemaphoreGive(g_manual_cache.mutex);
      return; // 已失效或等待轮询刷新，避免返回旧值或错误透传
    }
    if (all_filled) {
      size_t dl = manual_payload_len(function_code, register_count);
      uint8_t resp[1024];
      if (3U + dl + 2U > sizeof(resp)) {
        xSemaphoreGive(g_manual_cache.mutex);
        manual_send_exception(slave_addr, function_code, 0x04, source_channel);
        return;
      }
      resp[0] = slave_addr;
      resp[1] = function_code;
      resp[2] = (uint8_t)dl;
      memcpy(&resp[3], combined, dl);
      uint16_t c = calculate_crc(resp, 3 + dl);
      resp[3 + dl] = c & 0xFF;
      resp[3 + dl + 1] = (c >> 8) & 0xFF;
      xSemaphoreGive(g_manual_cache.mutex);
      tx_tasks_to_channel(resp, 3 + dl + 2, source_channel);
      return;
    }
    // 不能覆盖完整范围：若当前存在待完成写请求，则拒绝读透传，返回设备忙
    xSemaphoreGive(g_manual_cache.mutex);
    if (manual_write_state.state == WRITE_STATE_WAITING_RESPONSE) {
      manual_send_exception(slave_addr, function_code, 0x06, source_channel);
      return;
    }
    // 无写等待：透传到CH3（非白名单读）
    {
      uart_config_mode_t wm = get_uart_config_mode();
      clear_channel_data(3);
      (void)smart_send_data_to_ch3(wm, source_channel, request, (size_t)request_len);
      uint8_t ch3r[1024];
      uint64_t ts3;
      int waited = 0;
      while (!manual_cache_stopping && waited < timeout3_m) {
        int n3 = pop_channel_data(3, ch3r, sizeof(ch3r), &ts3);
        if (n3 > 0) {
          int frame_len = manual_valid_passthrough_response_len(
              ch3r, n3, slave_addr, function_code);
          if (frame_len > 0) {
            tx_tasks_to_channel(ch3r, frame_len, source_channel);
            return;
          }
          ESP_LOGW(TAG_MANUAL,
                   "丢弃非目标/CRC错误读透传响应: len=%d slave=%u fc=0x%02X",
                   n3, ch3r[0], ch3r[1]);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
      }
      // 超时：回0x0B
      if (!manual_cache_stopping) {
        manual_send_exception(slave_addr, function_code, 0x0B, source_channel);
      }
      return;
    }
  }

  // 其他功能码：全部拒绝（未在白名单范围内）
  manual_send_exception(slave_addr, function_code, 0x02, source_channel);
  return;
}

// forward_request_task 已删除（手动模式禁止自动跟随，固定使用条目配置）

// 无融合策略可配置
