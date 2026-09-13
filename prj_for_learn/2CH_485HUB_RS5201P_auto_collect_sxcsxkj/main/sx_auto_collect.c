#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sx_auto_collect.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 数据结构与全局 ====================
static const char *TAG_AC = "AUTO_COLLECT";
#define AC_STOP_WAIT_MS 1500
#define AC_VIRTUAL_REG_MAX 49000
#define AC_STATUS_REG_START 49001
#define AC_STATUS_REG_COUNT 60
#define AC_STATUS_REG_END (AC_STATUS_REG_START + AC_STATUS_REG_COUNT - 1)
#define AC_MAPPED_REG_UNSET 0xFFFF
#define AC_READ_WRITE_HOLDING 0x36
#define AC_DEFAULT_ERROR_CLEAR_COUNT 50
#define AC_DEFAULT_ERROR_MARKER_BASE 0x8000U

_Static_assert(sizeof("ac3_item59_baud") <= NVS_KEY_NAME_MAX_SIZE,
               "automatic collection NVS item key is too long");
_Static_assert(sizeof("ac3_item59_" SX_AC_NVS_ERROR_CLEAR_SUFFIX) <=
                   NVS_KEY_NAME_MAX_SIZE,
               "automatic collection error-clear NVS key is too long");

typedef struct {
  uint8_t real_slave_addr;
  uint8_t function_code;
  uint8_t config_item_index;
  uint16_t start_register;
  uint16_t register_count;
  uint8_t *data_buffer;
  size_t data_length;
  uint64_t last_update_time;
  uint32_t poll_interval_ms;
  uint32_t timeout_ms;
  channel_uart_config_t uart_config;
  bool data_valid;
  bool data_healthy;
  uint16_t fail_count;
  uint16_t error_clear_count;
  uint16_t error_marker;
} ac_real_data_table_t;

typedef struct {
  uint8_t virtual_slave_addr;
  uint16_t virtual_start_reg;
  uint16_t register_count;
  ac_real_data_table_t *real_table_ref;
  uint16_t real_offset;
} ac_mapping_table_t;

// 每通道管理器
typedef struct {
  ac_real_data_table_t *real_tables;
  int real_table_count;
  ac_mapping_table_t *mapping_tables;
  int mapping_table_count;
  SemaphoreHandle_t mutex;
  SemaphoreHandle_t channel_lock;
  TaskHandle_t polling_task;
  TaskHandle_t response_task;
  bool started;
  volatile bool stopping;
  volatile bool polling_task_exited;
  int host_channel;      // 采集通道（2或3）
  int slave_channel;     // 采集通道（与host_channel一致）
  uint8_t mapped_slave_addr;
  bool allow_exception_response;
  int config_item_count;
  uint16_t config_error_markers[AC_STATUS_REG_COUNT];
} ac_channel_mgr_t;

static EXT_RAM_BSS_ATTR ac_channel_mgr_t s_ac_mgr_ch2;
static EXT_RAM_BSS_ATTR ac_channel_mgr_t s_ac_mgr_ch3;
typedef struct {
  ac_channel_mgr_t *mgrs[2];
  int mgr_count;
  int resp_channel; // 上位机读取通道（CH1）
} ac_response_ctx_t;

typedef struct {
  ac_channel_mgr_t *mgr;
  ac_real_data_table_t *real_table;
  uint8_t virtual_slave;
  uint8_t real_slave;
  uint16_t virtual_base;
  uint16_t real_base;
  uint16_t real_start;
  uint16_t real_index;
  channel_uart_config_t uart_config;
  uint32_t timeout_ms;
} ac_write_target_t;

static EXT_RAM_BSS_ATTR ac_response_ctx_t s_resp_ctx;
static TaskHandle_t s_ac_response_task = NULL;
static volatile bool s_ac_response_stopping = false;
static volatile bool s_ac_response_task_exited = true;

static void ac_init_idle_state(void) {
  if (s_ac_mgr_ch2.polling_task == NULL && !s_ac_mgr_ch2.started) {
    s_ac_mgr_ch2.polling_task_exited = true;
  }
  if (s_ac_mgr_ch3.polling_task == NULL && !s_ac_mgr_ch3.started) {
    s_ac_mgr_ch3.polling_task_exited = true;
  }
}

// ==================== 工具函数 ====================
static inline uint64_t ac_now_ms(void) { return (uint64_t)(esp_timer_get_time() / 1000); }

static inline uint16_t ac_default_error_marker(int item_index) {
  return (uint16_t)(AC_DEFAULT_ERROR_MARKER_BASE + item_index + 1U);
}

static uint16_t ac_calculate_crc(const uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; bit++) {
      crc = (crc & 1U) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
    }
  }
  return crc;
}

#define AC_READ_COILS 0x01
#define AC_READ_DISCRETE 0x02
#define AC_READ_HOLDING 0x03
#define AC_READ_INPUT 0x04
#define AC_WRITE_SINGLE_REGISTER 0x06
#define AC_MAX_BIT_READ_COUNT 2000
#define AC_MAX_REGISTER_READ_COUNT 125

static inline uint8_t ac_poll_function_code(uint8_t function_code) {
  return function_code == AC_READ_WRITE_HOLDING ? AC_READ_HOLDING
                                                : function_code;
}

static inline bool ac_supports_single_register_write(uint8_t function_code) {
  return function_code == AC_READ_WRITE_HOLDING ||
         function_code == AC_WRITE_SINGLE_REGISTER;
}

static inline bool ac_is_supported_read(uint8_t function_code) {
  return function_code == AC_READ_COILS || function_code == AC_READ_DISCRETE ||
         function_code == AC_READ_HOLDING || function_code == AC_READ_INPUT;
}

static inline bool ac_is_bit_read(uint8_t function_code) {
  return function_code == AC_READ_COILS || function_code == AC_READ_DISCRETE;
}

static inline size_t ac_payload_len(uint8_t function_code, uint16_t quantity) {
  if (quantity == 0)
    return 0;
  return ac_is_bit_read(function_code) ? ((size_t)quantity + 7U) / 8U
                                       : (size_t)quantity * 2U;
}

static inline uint16_t ac_max_quantity(uint8_t function_code) {
  return ac_is_bit_read(function_code) ? AC_MAX_BIT_READ_COUNT
                                       : AC_MAX_REGISTER_READ_COUNT;
}

static inline bool ac_valid_read_quantity(uint8_t function_code,
                                          uint16_t quantity) {
  return ac_is_supported_read(function_code) && quantity > 0 &&
         quantity <= ac_max_quantity(function_code);
}

static inline bool ac_valid_u16_range(uint16_t start_addr, uint16_t quantity) {
  return quantity > 0 &&
         (uint32_t)start_addr + (uint32_t)quantity <= 0x10000U;
}

static inline bool ac_valid_read_range(uint8_t function_code,
                                       uint16_t start_addr,
                                       uint16_t quantity) {
  return ac_valid_read_quantity(function_code, quantity) &&
         ac_valid_u16_range(start_addr, quantity);
}

static inline bool ac_valid_item_range(uint8_t function_code,
                                       uint16_t start_addr,
                                       uint16_t quantity) {
  if (function_code == AC_WRITE_SINGLE_REGISTER) {
    return quantity == 1 && ac_valid_u16_range(start_addr, quantity);
  }
  return ac_valid_read_range(ac_poll_function_code(function_code), start_addr,
                             quantity);
}

static inline bool ac_get_bit(const uint8_t *payload, uint16_t bit_index) {
  return ((payload[bit_index / 8] >> (bit_index % 8)) & 0x01) != 0;
}

static inline void ac_set_bit(uint8_t *payload, uint16_t bit_index,
                              bool value) {
  uint8_t mask = (uint8_t)(1U << (bit_index % 8));
  if (value) {
    payload[bit_index / 8] |= mask;
  } else {
    payload[bit_index / 8] &= (uint8_t)~mask;
  }
}

static inline void ac_mask_unused_bits(ac_real_data_table_t *rt) {
  if (!rt || !rt->data_buffer || !ac_is_bit_read(rt->function_code) ||
      rt->data_length == 0)
    return;
  uint8_t used_bits = rt->register_count % 8;
  if (used_bits != 0) {
    rt->data_buffer[rt->data_length - 1] &=
        (uint8_t)((1U << used_bits) - 1U);
  }
}

static inline bool ac_copy_cached_value(const ac_real_data_table_t *rt,
                                        uint16_t real_index,
                                        uint8_t *combined,
                                        uint16_t combined_index) {
  if (!rt || !rt->data_buffer || !combined || real_index >= rt->register_count)
    return false;

  if (ac_is_bit_read(rt->function_code)) {
    if ((size_t)(real_index / 8) >= rt->data_length)
      return false;
    ac_set_bit(combined, combined_index,
               ac_get_bit(rt->data_buffer, real_index));
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

static inline bool ac_failure_zero_active(const ac_real_data_table_t *rt) {
  if (!rt)
    return false;
  uint16_t threshold = rt->error_clear_count
                           ? rt->error_clear_count
                           : AC_DEFAULT_ERROR_CLEAR_COUNT;
  return rt->fail_count >= threshold;
}

static bool ac_get_status_value(ac_channel_mgr_t *owner, uint16_t reg,
                                uint16_t *out_value) {
  if (!owner || !out_value || reg < AC_STATUS_REG_START ||
      reg > AC_STATUS_REG_END) {
    return false;
  }

  uint16_t index = reg - AC_STATUS_REG_START;
  uint16_t value = 0;
  if (index >= (uint16_t)owner->config_item_count) {
    *out_value = value;
    return true;
  }

  value = owner->config_error_markers[index];
  if (!owner->mutex) {
    *out_value = value;
    return true;
  }
  if (xSemaphoreTake(owner->mutex, sx_ms_to_ticks(100)) != pdTRUE)
    return false;

  for (int i = 0; i < owner->real_table_count; i++) {
    ac_real_data_table_t *rt = &owner->real_tables[i];
    if (rt->config_item_index != index)
      continue;
    bool ok = rt->register_count > 0 && rt->data_valid &&
              rt->data_healthy && rt->fail_count == 0;
    value = ok ? 1 : rt->error_marker;
    break;
  }
  xSemaphoreGive(owner->mutex);

  *out_value = value;
  return true;
}

static bool ac_validate_channel_config(const ac_channel_config_t *cfg,
                                       const char *channel_name) {
  if (!cfg)
    return false;
  if (cfg->mapped_slave_addr < 1 || cfg->mapped_slave_addr > 247) {
    ESP_LOGE(TAG_AC, "%s映射从机地址必须在1-247范围内: %u",
             channel_name, cfg->mapped_slave_addr);
    return false;
  }

  for (int i = 0; i < cfg->items_count; i++) {
    const ac_item_config_t *it = &cfg->items[i];
    if (!it->enabled)
      continue;

    if (it->error_marker < 2) {
      ESP_LOGE(TAG_AC, "%s条目%d错误标记必须在2-65535范围内",
               channel_name, i + 1);
      return false;
    }

    uint16_t virtual_start =
        (it->mapped_register_addr == AC_MAPPED_REG_UNSET)
            ? it->register_addr
            : it->mapped_register_addr;
    if (!ac_valid_item_range(it->function_code, it->register_addr,
                             it->register_num) ||
        !ac_valid_item_range(it->function_code, virtual_start,
                             it->register_num)) {
      ESP_LOGE(TAG_AC,
               "%s条目%d地址或数量无效: fc=0x%02X real=%u mapped=%u "
               "qty=%u",
               channel_name, i + 1, it->function_code, it->register_addr,
               virtual_start, it->register_num);
      return false;
    }

    uint32_t virtual_end =
        (uint32_t)virtual_start + (uint32_t)it->register_num - 1U;
    if (virtual_end > AC_VIRTUAL_REG_MAX) {
      ESP_LOGE(TAG_AC,
               "%s条目%d映射范围超出%d: start=%u qty=%u end=%" PRIu32,
               channel_name, i + 1, AC_VIRTUAL_REG_MAX, virtual_start,
               it->register_num, virtual_end);
      return false;
    }

    for (int j = 0; j < i; j++) {
      const ac_item_config_t *previous = &cfg->items[j];
      if (!previous->enabled)
        continue;
      uint16_t previous_start =
          (previous->mapped_register_addr == AC_MAPPED_REG_UNSET)
              ? previous->register_addr
              : previous->mapped_register_addr;
      uint32_t previous_end =
          (uint32_t)previous_start + (uint32_t)previous->register_num - 1U;
      if (!((uint32_t)virtual_start > previous_end ||
            (uint32_t)previous_start > virtual_end)) {
        ESP_LOGE(TAG_AC, "%s条目%d与条目%d映射范围重叠", channel_name,
                 j + 1, i + 1);
        return false;
      }
    }
  }
  return true;
}

static void ac_send_exception(const ac_channel_mgr_t *owner,
                              uint8_t function_code, uint8_t exception_code,
                              int channel) {
  if (!owner || !owner->allow_exception_response) {
    return;
  }

  uint8_t err[5];
  err[0] = owner->mapped_slave_addr;
  err[1] = function_code | 0x80;
  err[2] = exception_code;
  uint16_t crc = ac_calculate_crc(err, 3);
  err[3] = crc & 0xFF;
  err[4] = (crc >> 8) & 0xFF;
  tx_tasks_to_channel(err, sizeof(err), channel);
}

static ac_channel_mgr_t *ac_find_address_owner(ac_response_ctx_t *ctx,
                                               uint8_t slave_addr) {
  if (!ctx || slave_addr == 0) {
    return NULL;
  }

  for (int i = 0; i < ctx->mgr_count; i++) {
    ac_channel_mgr_t *mgr = ctx->mgrs[i];
    if (mgr && mgr->mapped_slave_addr == slave_addr) {
      return mgr;
    }
  }
  return NULL;
}

static void ac_free_tables(ac_channel_mgr_t *mgr) {
  if (mgr->real_tables) {
    for (int i = 0; i < mgr->real_table_count; i++) {
      free(mgr->real_tables[i].data_buffer);
    }
    free(mgr->real_tables);
    mgr->real_tables = NULL;
  }
  if (mgr->mapping_tables) {
    free(mgr->mapping_tables);
    mgr->mapping_tables = NULL;
  }
  mgr->real_table_count = 0;
  mgr->mapping_table_count = 0;
  mgr->config_item_count = 0;
  memset(mgr->config_error_markers, 0,
         sizeof(mgr->config_error_markers));
}

static bool ac_wait_task_exit(volatile bool *exited, uint32_t timeout_ms) {
  uint32_t waited = 0;
  while (!*exited && waited < timeout_ms) {
    vTaskDelay(sx_ms_to_ticks(10));
    waited += 10;
  }
  return *exited;
}

// CRC 校验
static inline bool ac_verify_crc(const uint8_t *frame, int total_length) {
  if (total_length < 3)
    return false;
  uint16_t recv_crc = (frame[total_length - 1] << 8) | frame[total_length - 2];
  uint16_t calc_crc = ac_calculate_crc(frame, (size_t)total_length - 2);
  return recv_crc == calc_crc;
}

static bool ac_find_write_target(ac_channel_mgr_t *mgr, uint8_t virtual_slave,
                                 uint16_t virtual_register,
                                 ac_write_target_t *target) {
  if (!mgr || !target || !mgr->mutex)
    return false;

  if (xSemaphoreTake(mgr->mutex, sx_ms_to_ticks(100)) != pdTRUE)
    return false;

  for (int i = 0; i < mgr->mapping_table_count; i++) {
    ac_mapping_table_t *mp = &mgr->mapping_tables[i];
    ac_real_data_table_t *rt = mp->real_table_ref;
    if (!rt || !ac_supports_single_register_write(rt->function_code) ||
        mp->virtual_slave_addr != virtual_slave ||
        virtual_register < mp->virtual_start_reg ||
        (uint32_t)virtual_register >=
            (uint32_t)mp->virtual_start_reg + mp->register_count) {
      continue;
    }

    uint16_t offset = virtual_register - mp->virtual_start_reg;
    if ((uint32_t)mp->real_offset + offset >= rt->register_count) {
      continue;
    }

    target->mgr = mgr;
    target->real_table = rt;
    target->virtual_slave = virtual_slave;
    target->real_slave = rt->real_slave_addr;
    target->virtual_base = mp->virtual_start_reg;
    target->real_base = rt->start_register + mp->real_offset;
    target->real_start = target->real_base + offset;
    target->real_index = mp->real_offset + offset;
    target->uart_config = rt->uart_config;
    target->uart_config.channel = mgr->host_channel;
    target->timeout_ms = rt->timeout_ms ? rt->timeout_ms : 1000;
    xSemaphoreGive(mgr->mutex);
    return true;
  }

  xSemaphoreGive(mgr->mutex);
  return false;
}

static bool ac_handle_write_request(ac_response_ctx_t *ctx,
                                    ac_channel_mgr_t *owner, uint8_t *request,
                                    int request_len) {
  if (!ctx || !owner || !request || request_len != 8 ||
      request[1] != AC_WRITE_SINGLE_REGISTER) {
    return false;
  }

  const uint8_t virtual_slave = request[0];
  const uint16_t virtual_register =
      ((uint16_t)request[2] << 8) | request[3];
  ac_write_target_t target = {0};
  if (!ac_find_write_target(owner, virtual_slave, virtual_register, &target)) {
    ac_send_exception(owner, AC_WRITE_SINGLE_REGISTER, 0x02,
                      ctx->resp_channel);
    return true;
  }

  uint8_t forward[8];
  memcpy(forward, request, sizeof(forward));
  forward[0] = target.real_slave;
  forward[2] = (uint8_t)(target.real_start >> 8);
  forward[3] = (uint8_t)target.real_start;
  uint16_t crc = ac_calculate_crc(forward, 6);
  forward[6] = (uint8_t)crc;
  forward[7] = (uint8_t)(crc >> 8);

  if (target.mgr->channel_lock)
    xSemaphoreTake(target.mgr->channel_lock, portMAX_DELAY);
  clear_channel_data(target.mgr->slave_channel);

  bool success = false;
  if (send_data_with_temp_config(target.mgr->slave_channel,
                                 &target.uart_config, forward,
                                 sizeof(forward)) == ESP_OK) {
    uint8_t response[128];
    uint64_t timestamp;
    int waited = 0;
    while (!s_ac_response_stopping && waited < (int)target.timeout_ms) {
      int response_len =
          pop_channel_data(target.mgr->slave_channel, response,
                           sizeof(response), &timestamp);
      if (response_len > 0) {
        bool normal_response =
            response_len == 8 && response[1] == AC_WRITE_SINGLE_REGISTER &&
            memcmp(&response[2], &forward[2], 4) == 0;
        bool exception_response =
            response_len == 5 &&
            response[1] == (AC_WRITE_SINGLE_REGISTER | 0x80);
        if ((normal_response || exception_response) &&
            ac_verify_crc(response, response_len) &&
            response[0] == target.real_slave) {
          response[0] = target.virtual_slave;
          if (normal_response) {
            response[2] = (uint8_t)(virtual_register >> 8);
            response[3] = (uint8_t)virtual_register;
          }
          uint16_t response_crc =
              ac_calculate_crc(response, (size_t)response_len - 2);
          response[response_len - 2] = (uint8_t)response_crc;
          response[response_len - 1] = (uint8_t)(response_crc >> 8);
          success = true;

          if (normal_response || owner->allow_exception_response) {
            tx_tasks_to_channel(response, (size_t)response_len,
                                ctx->resp_channel);
          }

          if (normal_response && target.mgr->mutex && target.real_table) {
            if (xSemaphoreTake(target.mgr->mutex, sx_ms_to_ticks(100)) ==
                pdTRUE) {
              size_t offset = (size_t)target.real_index * 2U;
              if (offset + 1U < target.real_table->data_length) {
                target.real_table->data_buffer[offset] = request[4];
                target.real_table->data_buffer[offset + 1U] = request[5];
                target.real_table->data_valid = true;
                target.real_table->data_healthy = true;
              }
              xSemaphoreGive(target.mgr->mutex);
            }
          }
          break;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      waited += 10;
    }
  }

  clear_channel_data(target.mgr->slave_channel);
  if (target.mgr->channel_lock)
    xSemaphoreGive(target.mgr->channel_lock);

  if (!success) {
    ac_send_exception(owner, AC_WRITE_SINGLE_REGISTER, 0x0B,
                      ctx->resp_channel);
  }
  return true;
}

// ==================== NVS 配置读写 ====================
static esp_err_t ac_load_channel_config(nvs_handle_t nvs, const char *prefix,
                                        ac_channel_config_t *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  char key[32];

  // 虚拟从机地址
  snprintf(key, sizeof(key), "%s_maddr", prefix);
  if (nvs_get_u8(nvs, key, &cfg->mapped_slave_addr) != ESP_OK) {
    cfg->mapped_slave_addr = (strcmp(prefix, "ac3") == 0) ? 3 : 2;
  }

  uint8_t allow_exception_response = 0;
  snprintf(key, sizeof(key), "%s_err_rsp", prefix);
  if (nvs_get_u8(nvs, key, &allow_exception_response) == ESP_OK) {
    cfg->allow_exception_response = allow_exception_response != 0;
  }

  // 条目数量
  int32_t count = 0;
  snprintf(key, sizeof(key), "%s_count", prefix);
  if (nvs_get_i32(nvs, key, &count) != ESP_OK || count < 0 || count > 60) {
    cfg->items_count = 0;
    return ESP_OK;
  }
  cfg->items_count = count;

  for (int i = 0; i < cfg->items_count; i++) {
    ac_item_config_t *it = &cfg->items[i];
    sx_ac_nvs_item_t stored = {0};
    size_t stored_size = sizeof(stored);
    snprintf(key, sizeof(key), "%s_i%d", prefix, i);
    if (nvs_get_blob(nvs, key, &stored, &stored_size) == ESP_OK &&
        stored_size == sizeof(stored) &&
        stored.version == SX_AC_NVS_ITEM_VERSION) {
      it->enabled = stored.enabled != 0;
      it->real_slave_addr = stored.real_slave_addr;
      it->function_code = stored.function_code;
      it->register_addr = stored.register_addr;
      it->mapped_register_addr = stored.mapped_register_addr;
      it->register_num = stored.register_num;
      it->interval_ms = stored.interval_ms;
      it->timeout_ms = stored.timeout_ms;
      it->error_marker = stored.error_marker >= 2
                             ? stored.error_marker
                             : ac_default_error_marker(i);
      it->error_clear_count = stored.error_clear_count
                                  ? stored.error_clear_count
                                  : AC_DEFAULT_ERROR_CLEAR_COUNT;
      it->uart.baudrate = stored.baudrate ? stored.baudrate : 9600;
      it->uart.data_bits =
          (stored.data_bits == 5)   ? UART_DATA_5_BITS
          : (stored.data_bits == 6) ? UART_DATA_6_BITS
          : (stored.data_bits == 7) ? UART_DATA_7_BITS
                                    : UART_DATA_8_BITS;
      it->uart.parity =
          (stored.parity == 1)   ? UART_PARITY_ODD
          : (stored.parity == 2) ? UART_PARITY_EVEN
                                 : UART_PARITY_DISABLE;
      it->uart.stop_bits = (stored.stop_bits == 2) ? UART_STOP_BITS_2
                                                    : UART_STOP_BITS_1;
      it->uart.frame_len = 512;
      it->uart.channel = 3;
      it->uart.frame_time = 50;
      continue;
    }

    snprintf(key, sizeof(key), "%s_item%d_en", prefix, i);
    uint8_t en = 0;
    nvs_get_u8(nvs, key, &en);
    it->enabled = en != 0;

    snprintf(key, sizeof(key), "%s_item%d_rs", prefix, i);
    nvs_get_u8(nvs, key, &it->real_slave_addr);

    snprintf(key, sizeof(key), "%s_item%d_fc", prefix, i);
    nvs_get_u8(nvs, key, &it->function_code);

    snprintf(key, sizeof(key), "%s_item%d_ra", prefix, i);
    uint16_t reg = 0;
    if (nvs_get_u16(nvs, key, &reg) == ESP_OK)
      it->register_addr = reg;

    snprintf(key, sizeof(key), "%s_item%d_mra", prefix, i);
    uint16_t mreg = AC_MAPPED_REG_UNSET;
    if (nvs_get_u16(nvs, key, &mreg) == ESP_OK)
      it->mapped_register_addr = mreg;
    else
      it->mapped_register_addr = AC_MAPPED_REG_UNSET;

    snprintf(key, sizeof(key), "%s_item%d_rn", prefix, i);
    uint16_t rn = 0;
    if (nvs_get_u16(nvs, key, &rn) == ESP_OK)
      it->register_num = rn;

    snprintf(key, sizeof(key), "%s_item%d_int", prefix, i);
    uint32_t iv = 0;
    if (nvs_get_u32(nvs, key, &iv) == ESP_OK)
      it->interval_ms = iv;
    else
      it->interval_ms = 100;

    snprintf(key, sizeof(key), "%s_item%d_to", prefix, i);
    uint32_t to = 0;
    if (nvs_get_u32(nvs, key, &to) == ESP_OK)
      it->timeout_ms = to;
    else
      it->timeout_ms = 1000;

    snprintf(key, sizeof(key), "%s_item%d_err", prefix, i);
    uint16_t err_marker = ac_default_error_marker(i);
    if (nvs_get_u16(nvs, key, &err_marker) != ESP_OK || err_marker < 2) {
      err_marker = ac_default_error_marker(i);
    }
    it->error_marker = err_marker;

    snprintf(key, sizeof(key), "%s_item%d_" SX_AC_NVS_ERROR_CLEAR_SUFFIX,
             prefix, i);
    uint16_t error_clear_count = AC_DEFAULT_ERROR_CLEAR_COUNT;
    if (nvs_get_u16(nvs, key, &error_clear_count) == ESP_OK &&
        error_clear_count > 0) {
      it->error_clear_count = error_clear_count;
    } else {
      it->error_clear_count = AC_DEFAULT_ERROR_CLEAR_COUNT;
    }

    // UART
    snprintf(key, sizeof(key), "%s_item%d_baud", prefix, i);
    uint32_t baud = 9600;
    nvs_get_u32(nvs, key, &baud);
    it->uart.baudrate = baud;

    snprintf(key, sizeof(key), "%s_item%d_db", prefix, i);
    uint8_t db = 8;
    nvs_get_u8(nvs, key, &db);
    it->uart.data_bits = (db == 5) ? UART_DATA_5_BITS : (db == 6) ? UART_DATA_6_BITS : (db == 7) ? UART_DATA_7_BITS : UART_DATA_8_BITS;

    snprintf(key, sizeof(key), "%s_item%d_par", prefix, i);
    uint8_t par = 0;
    nvs_get_u8(nvs, key, &par);
    it->uart.parity = (par == 1) ? UART_PARITY_ODD : (par == 2) ? UART_PARITY_EVEN : UART_PARITY_DISABLE;

    snprintf(key, sizeof(key), "%s_item%d_sb", prefix, i);
    uint8_t sb = 1;
    nvs_get_u8(nvs, key, &sb);
    it->uart.stop_bits = (sb == 2) ? UART_STOP_BITS_2 : UART_STOP_BITS_1;

    it->uart.frame_len = 512;
    it->uart.channel = 3;
    it->uart.frame_time = 50;
  }

  return ESP_OK;
}

// ==================== 表构建与销毁 ====================
static esp_err_t ac_build_tables(const ac_channel_config_t *cfg,
                                 ac_channel_mgr_t *mgr) {
  ac_free_tables(mgr);
  mgr->mapped_slave_addr = cfg->mapped_slave_addr;
  mgr->allow_exception_response = cfg->allow_exception_response;
  mgr->config_item_count = cfg->items_count;
  if (mgr->config_item_count > AC_STATUS_REG_COUNT) {
    mgr->config_item_count = AC_STATUS_REG_COUNT;
  }
  for (int i = 0; i < mgr->config_item_count; i++) {
    mgr->config_error_markers[i] =
        cfg->items[i].error_marker >= 2
            ? cfg->items[i].error_marker
            : ac_default_error_marker(i);
  }

  int valid_count = 0;
  for (int i = 0; i < cfg->items_count; i++) {
    const ac_item_config_t *it = &cfg->items[i];
    uint16_t virtual_start =
        (it->mapped_register_addr == AC_MAPPED_REG_UNSET)
            ? it->register_addr
            : it->mapped_register_addr;
    if (it->enabled &&
        ac_valid_item_range(it->function_code, it->register_addr,
                            it->register_num) &&
        ac_valid_item_range(it->function_code, virtual_start,
                            it->register_num)) {
      valid_count++;
    }
  }

  mgr->real_table_count = valid_count;
  mgr->mapping_table_count = valid_count;

  if (valid_count == 0) {
    if (!mgr->mutex)
      mgr->mutex = xSemaphoreCreateMutex();
    if (!mgr->mutex)
      return ESP_ERR_NO_MEM;
    if (!mgr->channel_lock)
      mgr->channel_lock = xSemaphoreCreateMutex();
    return mgr->channel_lock ? ESP_OK : ESP_ERR_NO_MEM;
  }

  mgr->real_tables = calloc(valid_count, sizeof(ac_real_data_table_t));
  mgr->mapping_tables = calloc(valid_count, sizeof(ac_mapping_table_t));
  if (!mgr->real_tables || !mgr->mapping_tables) {
    ac_free_tables(mgr);
    return ESP_ERR_NO_MEM;
  }

  int out = 0;
  for (int i = 0; i < cfg->items_count; i++) {
    const ac_item_config_t *it = &cfg->items[i];
    if (!it->enabled)
      continue;
    uint16_t virtual_start =
        (it->mapped_register_addr == AC_MAPPED_REG_UNSET)
            ? it->register_addr
            : it->mapped_register_addr;
    if (!ac_valid_item_range(it->function_code, it->register_addr,
                             it->register_num) ||
        !ac_valid_item_range(it->function_code, virtual_start,
                             it->register_num)) {
      ESP_LOGW(TAG_AC,
               "跳过无效采集项: slave=%u fc=0x%02X addr=%u mapped=%u qty=%u",
               it->real_slave_addr, it->function_code, it->register_addr,
               virtual_start, it->register_num);
      continue;
    }

    ac_real_data_table_t *rt = &mgr->real_tables[out];
    rt->real_slave_addr = it->real_slave_addr;
    rt->function_code = it->function_code;
    rt->config_item_index = (uint8_t)i;
    rt->start_register = it->register_addr;
    rt->register_count = it->register_num;
    rt->data_length =
        ac_payload_len(ac_poll_function_code(rt->function_code),
                       rt->register_count);
    rt->uart_config = it->uart;
    rt->uart_config.channel = mgr->host_channel; // 绑定当前通道
    rt->poll_interval_ms = it->interval_ms ? it->interval_ms : 100;
    rt->timeout_ms = it->timeout_ms ? it->timeout_ms : 1000;
    rt->data_valid = false;
    rt->data_healthy = true;
    rt->fail_count = 0;
    rt->error_clear_count = it->error_clear_count
                                ? it->error_clear_count
                                : AC_DEFAULT_ERROR_CLEAR_COUNT;
    rt->error_marker = it->error_marker >= 2
                           ? it->error_marker
                           : ac_default_error_marker(i);
    rt->last_update_time = 0;
    rt->data_buffer = calloc(rt->data_length, 1);
    if (!rt->data_buffer) {
      ac_free_tables(mgr);
      return ESP_ERR_NO_MEM;
    }

    ac_mapping_table_t *mp = &mgr->mapping_tables[out];
    mp->virtual_slave_addr = cfg->mapped_slave_addr;
    mp->virtual_start_reg = virtual_start;
    mp->register_count = it->register_num;
    mp->real_table_ref = rt;
    mp->real_offset = 0;
    out++;
  }

  if (!mgr->mutex)
    mgr->mutex = xSemaphoreCreateMutex();
  if (!mgr->mutex)
    return ESP_ERR_NO_MEM;
  if (!mgr->channel_lock)
    mgr->channel_lock = xSemaphoreCreateMutex();
  if (!mgr->channel_lock) {
    ac_free_tables(mgr);
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

static void ac_destroy_tables(ac_channel_mgr_t *mgr) {
  ac_free_tables(mgr);
  if (mgr->channel_lock) {
    vSemaphoreDelete(mgr->channel_lock);
    mgr->channel_lock = NULL;
  }
  if (mgr->mutex) {
    vSemaphoreDelete(mgr->mutex);
    mgr->mutex = NULL;
  }
  mgr->started = false;
  mgr->stopping = false;
  mgr->mapped_slave_addr = 0;
  mgr->allow_exception_response = false;
}

// ==================== 任务实现 ====================
static void ac_polling_task(void *param) {
  ac_channel_mgr_t *mgr = (ac_channel_mgr_t *)param;
  ESP_LOGI(TAG_AC, "通道%d轮询任务启动", mgr->host_channel);
  mgr->polling_task_exited = false;
  while (!mgr->stopping) {
    if (xSemaphoreTake(mgr->mutex, sx_ms_to_ticks(100)) != pdTRUE) {
      continue;
    }
    bool lock_held = true;
    uint64_t now = ac_now_ms();
    for (int i = 0; i < mgr->real_table_count; i++) {
      if (mgr->stopping)
        break;
      ac_real_data_table_t *rt = &mgr->real_tables[i];
      if (!rt->register_count)
        continue;
      if (!ac_is_supported_read(ac_poll_function_code(rt->function_code)))
        continue;
      if (now - rt->last_update_time < rt->poll_interval_ms)
        continue;
      uint8_t req[8];
      uint8_t poll_fc = ac_poll_function_code(rt->function_code);
      req[0] = rt->real_slave_addr;
      req[1] = poll_fc;
      req[2] = (rt->start_register >> 8) & 0xFF;
      req[3] = rt->start_register & 0xFF;
      req[4] = (rt->register_count >> 8) & 0xFF;
      req[5] = rt->register_count & 0xFF;
      uint16_t crc = ac_calculate_crc(req, 6);
      req[6] = crc & 0xFF;
      req[7] = (crc >> 8) & 0xFF;
      xSemaphoreGive(mgr->mutex);
      lock_held = false;

      if (mgr->channel_lock)
        xSemaphoreTake(mgr->channel_lock, portMAX_DELAY);
      clear_channel_data(mgr->slave_channel);

      if (send_data_with_temp_config(mgr->slave_channel, &rt->uart_config, req, sizeof(req)) == ESP_OK) {
        uint8_t rsp[1024];
        uint64_t ts;
        int waited = 0;
        bool ok = false;
        while (!mgr->stopping && waited < (int)rt->timeout_ms && !ok) {
          int n = pop_channel_data(mgr->slave_channel, rsp, sizeof(rsp), &ts);
          if (n > 0) {
            if (n >= 5 && rsp[0] == rt->real_slave_addr &&
                rsp[1] == poll_fc) {
              uint8_t dl = rsp[2];
              size_t frame_len = 3U + (size_t)dl + 2U;
              if (dl == rt->data_length && (size_t)n == frame_len) {
                if (ac_verify_crc(rsp, (int)frame_len)) {
                  xSemaphoreTake(mgr->mutex, portMAX_DELAY);
                  memcpy(rt->data_buffer, &rsp[3], dl);
                  ac_mask_unused_bits(rt);
                  rt->data_valid = true;
                  rt->data_healthy = true;
                  rt->fail_count = 0;
                  rt->last_update_time = ac_now_ms();
                  xSemaphoreGive(mgr->mutex);
                  ok = true;
                } else {
                  ESP_LOGW(TAG_AC,
                           "清理CRC错误采集响应: len=%d frame_len=%zu "
                           "slave=%u fc=0x%02X",
                           n, frame_len, rt->real_slave_addr,
                           poll_fc);
                }
              } else {
                ESP_LOGW(TAG_AC,
                         "采集回包长度不匹配: slave=%u fc=0x%02X "
                         "expect=%u actual=%u n=%d",
                         rt->real_slave_addr, poll_fc,
                         (unsigned)rt->data_length, dl, n);
              }
            } else {
              ESP_LOGW(TAG_AC,
                       "清理非目标/CRC错误采集响应: len=%d expect_slave=%u "
                       "expect_fc=0x%02X",
                       n, rt->real_slave_addr, poll_fc);
            }
          }
          vTaskDelay(pdMS_TO_TICKS(10));
          waited += 10;
        }
        if (!mgr->stopping && !ok) {
          xSemaphoreTake(mgr->mutex, portMAX_DELAY);
          if (rt->fail_count < UINT16_MAX)
            rt->fail_count++;
          if (rt->fail_count >= rt->error_clear_count) {
            memset(rt->data_buffer, 0, rt->data_length);
            rt->data_healthy = false;
            rt->data_valid = false;
          }
          xSemaphoreGive(mgr->mutex);
        }
      } else if (!mgr->stopping) {
        xSemaphoreTake(mgr->mutex, portMAX_DELAY);
        rt->last_update_time = now;
        if (rt->fail_count < UINT16_MAX)
          rt->fail_count++;
        if (rt->fail_count >= rt->error_clear_count) {
          memset(rt->data_buffer, 0, rt->data_length);
          rt->data_healthy = false;
          rt->data_valid = false;
        }
        xSemaphoreGive(mgr->mutex);
      }
      clear_channel_data(mgr->slave_channel);
      if (mgr->channel_lock)
        xSemaphoreGive(mgr->channel_lock);
      if (mgr->stopping) {
        break;
      }
      xSemaphoreTake(mgr->mutex, portMAX_DELAY);
      lock_held = true;
    }
    if (lock_held) {
      xSemaphoreGive(mgr->mutex);
    }
    if (!mgr->stopping) {
      vTaskDelay(pdMS_TO_TICKS(150));
    }
  }

  mgr->polling_task_exited = true;
  mgr->polling_task = NULL;
  ESP_LOGI(TAG_AC, "通道%d轮询任务退出", mgr->host_channel);
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

static void ac_response_task(void *param) {
  ac_response_ctx_t *ctx = (ac_response_ctx_t *)param;
  ESP_LOGI(TAG_AC, "CH%d 响应任务启动", ctx->resp_channel);
  s_ac_response_task_exited = false;
  while (!s_ac_response_stopping) {
    uint8_t req[128];
    uint64_t ts;
    int n = pop_channel_data(ctx->resp_channel, req, sizeof(req), &ts);
    if (n <= 0) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (n < 8 || !ac_verify_crc(req, n))
      continue;

    uint8_t slave = req[0];
    uint8_t fc = req[1];
    uint16_t addr = (req[2] << 8) | req[3];
    uint16_t cnt = (req[4] << 8) | req[5];
    ac_channel_mgr_t *owner = ac_find_address_owner(ctx, slave);
    if (!owner) {
      continue;
    }

    if (fc == AC_WRITE_SINGLE_REGISTER) {
      ac_handle_write_request(ctx, owner, req, n);
      continue;
    }

    if (ac_is_supported_read(fc)) {
      if (n != 8) {
        continue;
      }
      if (!ac_valid_read_range(fc, addr, cnt)) {
        ac_send_exception(owner, fc, 0x03, ctx->resp_channel);
        continue;
      }

      uint8_t combined[1024] = {0};
      bool all_filled = true;

      for (uint16_t idx = 0; idx < cnt; idx++) {
        uint16_t point = addr + idx;
        bool filled = false;

        if ((fc == AC_READ_HOLDING || fc == AC_READ_INPUT) &&
            point >= AC_STATUS_REG_START && point <= AC_STATUS_REG_END) {
          uint16_t status_value = 0;
          if (ac_get_status_value(owner, point, &status_value)) {
            size_t offset = (size_t)idx * 2U;
            combined[offset] = (uint8_t)(status_value >> 8);
            combined[offset + 1U] = (uint8_t)status_value;
            filled = true;
          }
        }

        if (!filled && owner->mutex &&
            xSemaphoreTake(owner->mutex, sx_ms_to_ticks(100)) == pdTRUE) {
          for (int i = 0; i < owner->mapping_table_count && !filled; i++) {
            ac_mapping_table_t *mp = &owner->mapping_tables[i];
            if (mp->virtual_slave_addr == slave &&
                point >= mp->virtual_start_reg &&
                (uint32_t)point < (uint32_t)mp->virtual_start_reg +
                                      (uint32_t)mp->register_count) {
              ac_real_data_table_t *rt = mp->real_table_ref;
              if (rt && ac_poll_function_code(rt->function_code) == fc) {
                if (ac_failure_zero_active(rt)) {
                  filled = true;
                } else if (rt->data_valid && rt->data_healthy) {
                  uint16_t voff = point - mp->virtual_start_reg;
                  uint16_t real_index = mp->real_offset + voff;
                  filled =
                      ac_copy_cached_value(rt, real_index, combined, idx);
                }
              }
              break; // 同功能码映射命中后退出
            }
          }
          xSemaphoreGive(owner->mutex);
        }

        if (!filled) {
          all_filled = false;
          break;
        }
      }

      if (all_filled) {
        size_t dl = ac_payload_len(fc, cnt);
        uint8_t resp[1024];
        if (3U + dl + 2U > sizeof(resp)) {
          ac_send_exception(owner, fc, 0x04, ctx->resp_channel);
          continue;
        }

        resp[0] = slave;
        resp[1] = fc;
        resp[2] = (uint8_t)dl;
        memcpy(&resp[3], combined, dl);
        uint16_t crc = ac_calculate_crc(resp, 3 + dl);
        resp[3 + dl] = crc & 0xFF;
        resp[3 + dl + 1] = (crc >> 8) & 0xFF;
        tx_tasks_to_channel(resp, 3 + dl + 2, ctx->resp_channel);
      } else {
        ac_send_exception(owner, fc, 0x02, ctx->resp_channel);
      }
    } else {
      ac_send_exception(owner, fc, 0x01, ctx->resp_channel);
    }
  }

  s_ac_response_task_exited = true;
  s_ac_response_task = NULL;
  ESP_LOGI(TAG_AC, "CH%d 响应任务退出", ctx->resp_channel);
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

esp_err_t sx_auto_collect_stop(void) {
  ac_init_idle_state();
  bool cleanup_allowed = true;

  s_ac_response_stopping = true;
  s_ac_mgr_ch2.stopping = true;
  s_ac_mgr_ch3.stopping = true;

  if (s_ac_response_task != NULL) {
    if (!ac_wait_task_exit(&s_ac_response_task_exited, AC_STOP_WAIT_MS)) {
      ESP_LOGW(TAG_AC, "自动采集响应任务未及时退出，强制删除");
      delete_app_task_with_caps(s_ac_response_task);
      s_ac_response_task_exited = true;
      cleanup_allowed = false;
    }
    s_ac_response_task = NULL;
  }

  ac_channel_mgr_t *mgrs[] = {&s_ac_mgr_ch2, &s_ac_mgr_ch3};
  for (int i = 0; i < 2; i++) {
    ac_channel_mgr_t *mgr = mgrs[i];
    if (mgr->polling_task != NULL) {
      if (!ac_wait_task_exit(&mgr->polling_task_exited, AC_STOP_WAIT_MS)) {
        ESP_LOGW(TAG_AC, "CH%d自动采集轮询任务未及时退出，强制删除",
                 mgr->host_channel);
        delete_app_task_with_caps(mgr->polling_task);
        mgr->polling_task_exited = true;
        cleanup_allowed = false;
      }
      mgr->polling_task = NULL;
    }
  }

  s_resp_ctx.mgr_count = 0;

  if (!cleanup_allowed) {
    ESP_LOGW(TAG_AC, "自动采集任务为强制删除，跳过共享资源释放以避免并发释放");
    return ESP_ERR_TIMEOUT;
  }

  ac_destroy_tables(&s_ac_mgr_ch2);
  ac_destroy_tables(&s_ac_mgr_ch3);
  s_ac_response_stopping = false;
  ESP_LOGI(TAG_AC, "自动采集模式已停止");
  return ESP_OK;
}

// ==================== 初始化入口 ====================
esp_err_t sx_auto_collect_init(void) {
  ESP_LOGI(TAG_AC, "初始化自动采集模式（双通道）");
  ac_init_idle_state();

  nvs_handle_t nvs;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG_AC, "打开NVS失败: %s", esp_err_to_name(err));
    return err;
  }

  char mode[32] = {0};
  size_t len = sizeof(mode);
  err = nvs_get_str(nvs, "w_mode", mode, &len);
  if (err != ESP_OK || strcmp(mode, "auto_collect") != 0) {
    ESP_LOGW(TAG_AC, "当前工作模式[%s]非auto_collect，跳过启动",
             (err == ESP_OK) ? mode : esp_err_to_name(err));
    nvs_close(nvs);
    return ESP_OK;
  }

  ac_config_t *cfg = calloc(1, sizeof(ac_config_t));
  if (!cfg) {
    nvs_close(nvs);
    ESP_LOGE(TAG_AC, "分配配置缓冲失败");
    return ESP_ERR_NO_MEM;
  }

  ac_load_channel_config(nvs, "ac2", &cfg->ch2);
  ac_load_channel_config(nvs, "ac3", &cfg->ch3);
  nvs_close(nvs);

  if (!ac_validate_channel_config(&cfg->ch2, "CH2") ||
      !ac_validate_channel_config(&cfg->ch3, "CH3")) {
    ESP_LOGE(TAG_AC, "自动采集配置校验失败，停止启动");
    free(cfg);
    return ESP_ERR_INVALID_ARG;
  }

  if (cfg->ch2.mapped_slave_addr == cfg->ch3.mapped_slave_addr) {
    ESP_LOGE(TAG_AC, "CH2和CH3的映射从机地址不能相同: %u",
             cfg->ch2.mapped_slave_addr);
    free(cfg);
    return ESP_ERR_INVALID_ARG;
  }

  err = sx_auto_collect_stop();
  if (err != ESP_OK) {
    free(cfg);
    return err;
  }

  // 初始化 CH2 管理器
  s_ac_mgr_ch2.host_channel = 2;
  s_ac_mgr_ch2.slave_channel = 2;
  esp_err_t ret = ac_build_tables(&cfg->ch2, &s_ac_mgr_ch2);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG_AC, "CH2 表构建失败: %s", esp_err_to_name(ret));
    free(cfg);
    return ret;
  }
  s_ac_mgr_ch2.stopping = false;
  s_ac_mgr_ch2.polling_task_exited = false;
  BaseType_t task_ret =
      create_app_task_psram(ac_polling_task, "ac_poll_ch2", 20480,
                            &s_ac_mgr_ch2, 2, &s_ac_mgr_ch2.polling_task,
                            SX_WORK_CORE_ID);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG_AC, "CH2 轮询任务创建失败");
    s_ac_mgr_ch2.polling_task_exited = true;
    ac_destroy_tables(&s_ac_mgr_ch2);
    free(cfg);
    return ESP_FAIL;
  }
  s_ac_mgr_ch2.started = true;

  // 初始化 CH3 管理器
  s_ac_mgr_ch3.host_channel = 3;
  s_ac_mgr_ch3.slave_channel = 3;
  ret = ac_build_tables(&cfg->ch3, &s_ac_mgr_ch3);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG_AC, "CH3 表构建失败: %s", esp_err_to_name(ret));
    sx_auto_collect_stop();
    free(cfg);
    return ret;
  }
  s_ac_mgr_ch3.stopping = false;
  s_ac_mgr_ch3.polling_task_exited = false;
  task_ret = create_app_task_psram(ac_polling_task, "ac_poll_ch3", 20480,
                                   &s_ac_mgr_ch3, 2,
                                   &s_ac_mgr_ch3.polling_task,
                                   SX_WORK_CORE_ID);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG_AC, "CH3 轮询任务创建失败");
    s_ac_mgr_ch3.polling_task_exited = true;
    sx_auto_collect_stop();
    free(cfg);
    return ESP_FAIL;
  }
  s_ac_mgr_ch3.started = true;

  // 启动共用的响应任务（监听CH1，聚合两通道的映射表）
  s_resp_ctx.mgr_count = 0;
  s_resp_ctx.mgrs[s_resp_ctx.mgr_count++] = &s_ac_mgr_ch2;
  s_resp_ctx.mgrs[s_resp_ctx.mgr_count++] = &s_ac_mgr_ch3;
  s_resp_ctx.resp_channel = 1;
  s_ac_response_stopping = false;
  s_ac_response_task_exited = false;
  task_ret = create_app_task_psram(ac_response_task, "ac_resp_ch1", 16384,
                                   &s_resp_ctx, 15, &s_ac_response_task,
                                   SX_WORK_CORE_ID);
  if (task_ret != pdPASS) {
    ESP_LOGE(TAG_AC, "CH1 响应任务创建失败");
    s_ac_response_task_exited = true;
    sx_auto_collect_stop();
    free(cfg);
    return ESP_FAIL;
  }

  free(cfg);
  ESP_LOGI(TAG_AC, "自动采集模式启动完成");
  return ESP_OK;
}
