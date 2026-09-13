#include "sx_modbus_cache.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ==================== 自动智能缓存配置宏定义 ====================
#define AUTO_CACHE_TIMEOUT_MS 86400000 // 自动模式缓存超时时间（毫秒，1天）
#define AUTO_CACHE_DATA_FRESH_TIME_MS                                          \
  2000 // 缓存数据新鲜度检查时间（毫秒，2秒）
#define AUTO_CACHE_POLL_RESPONSE_TIMEOUT_MS 200 // 轮询响应超时时间（毫秒）
#define AUTO_CACHE_GC_INTERVAL_MS 300000        // 垃圾回收间隔（毫秒，5分钟）
#define MODBUS_MAX_REGISTER_COUNT 125 // Modbus RTU单次最大读取寄存器数量

// 自动智能缓存调度设计：
// 1. 轮询任务不再按固定周期扫表；只要有可刷新的条目，就连续选择最高优先级条目，
//    尽量填满CH3总线。
// 2. 主站读请求只记录最近窗口内的访问次数。每个调度窗口结束时，将旧热度衰减后
//    合并新访问，从而得到“最近一段时间内的相对访问比例”，不是永久累计次数。
// 3. 每个条目的最小刷新间隔由刷新成本决定。刷新成本取“按帧长/波特率估算的
//    传输时间”和“实测轮询耗时EWMA”的较大值，避免长帧被强行按不现实频率刷新。
// 4. CH3空闲时所有条目尽量按成本允许的最快速度刷新；CH3忙时再按访问比例倾斜
//    给高频条目，同时通过最大陈旧时间保证低频条目不会饿死。
// 5. last_poll_start_time用于限制两次轮询尝试之间的间隔；last_poll_time只在成功
//    更新缓存后写入，用于判断数据新鲜度。这两个时间的语义不能混用。
#define AUTO_CACHE_POLL_DELAY_FAST_MS 5
#define AUTO_CACHE_POLL_DELAY_NORMAL_MS 10
#define AUTO_CACHE_POLL_DELAY_IDLE_MS 20
#define AUTO_CACHE_READ_TASK_DELAY_MS 5
#define AUTO_CACHE_SCHED_WINDOW_MS 1000
#define AUTO_CACHE_HEAT_SCALE 1000U
#define AUTO_CACHE_DECAY_NUM 7U
#define AUTO_CACHE_DECAY_DEN 10U
#define AUTO_CACHE_COST_EWMA_OLD 3U
#define AUTO_CACHE_COST_EWMA_NEW 1U
#define AUTO_CACHE_UTIL_EWMA_OLD 3U
#define AUTO_CACHE_UTIL_EWMA_NEW 1U
#define AUTO_CACHE_BASE_MIN_INTERVAL_MS 20U
#define AUTO_CACHE_FULL_REFRESH_MIN_MS 20U
#define AUTO_CACHE_COST_MULTIPLIER 1U
#define AUTO_CACHE_MAX_STALE_MIN_MS 2000U
#define AUTO_CACHE_EMPTY_BUS_UTIL 500U   // 50.0%
#define AUTO_CACHE_BUSY_BUS_UTIL 850U    // 85.0%
#define AUTO_CACHE_HIGH_BUS_UTIL 900U    // 90.0%
#define AUTO_CACHE_URGENT_PRIORITY 1000000U
#define AUTO_CACHE_STARVE_PRIORITY 500000U
#define AUTO_CACHE_MAX_PRIORITY 2000000000U
#define AUTO_CACHE_FAIL_BACKOFF_BASE_MS 5000U
#define AUTO_CACHE_FAIL_BACKOFF_MAX_MS 60000U
#define AUTO_CACHE_RTU_TURNAROUND_MS 5U
#define AUTO_CACHE_SLAVE_MARGIN_MS 10U
//
// ==================== 本文件通用硬编码宏集中 ====================
// 缓冲区大小/帧长度
#define RESPONSE_BUFFER_SIZE 1024
#define REQUEST_BUFFER_SIZE 64
#define MODBUS_RTU_READ_REQ_LEN 8
// 时间与延时（毫秒）
#define UART_TASK_SWITCH_DELAY_MS 50
#define CH3_RESPONSE_CHECK_INTERVAL_MS 10
#define SHORT_DELAY_MS 5
// 写命令前等待轮询停止/总线空闲的延时（毫秒）
#define WRITE_PRE_SEND_DELAY_MS 125
// 自动模式任务周期/等待
#define AUTO_POLL_CYCLE_MS 20000
#define AUTO_NO_ITEMS_WAIT_MS 5000
#define AUTO_LOG_STATUS_INTERVAL_MS 30000
#define AUTO_PROCESS_YIELD_STEP 5
// 默认串口/超时
#define DEFAULT_REPLY_TIMEOUT_MS 1000
#define DEFAULT_UART_BAUDRATE 9600
#define DEFAULT_UART_FRAME_TIME_MS 50
#define DEFAULT_UART_FRAME_LEN 512
// Modbus异常/阈值（保留必要的语义宏）
#define MODBUS_EXCEPTION_GATEWAY_TARGET_NO_RESPONSE 0x0B
#define AUTO_CACHE_REMOVE_FAIL_THRESHOLD 3
// 最小/固定帧长度及安全间隔
#define MIN_MODBUS_FRAME_LEN 8
#define SAFETY_INTERVAL_DIVISOR 10
#define MIN_SAFETY_INTERVAL_MS 50

static inline bool modbus_u16_range_valid(uint16_t start_addr,
                                          uint16_t quantity) {
  return quantity > 0 &&
         (uint32_t)start_addr + (uint32_t)quantity <= 0x10000U;
}

// 函数前置声明

// 缓存系统清理和重新初始化函数
// void cleanup_and_reinit_modbus_cache(void);

// 自动缓存轮询任务前置声明
void auto_cache_poll_task(void *pvParameter);
static void auto_cache_clear_all(void);
static void auto_cache_reset_runtime_state(void);
static bool auto_cache_wait_task_exit(volatile bool *exited,
                                      uint32_t timeout_ms);
static void auto_cache_notify_poll_task(void);

static const char *TAG = "MODBUS_CACHE";
static TaskHandle_t modbus_cache_task_handle = NULL;
static TaskHandle_t virtual_slave_task_handle = NULL;
static TaskHandle_t auto_cache_task_handle = NULL;
static TaskHandle_t auto_poll_task_handle = NULL;
static volatile bool auto_cache_tasks_stopping = false;
static volatile bool auto_cache_task_exited = true;
static volatile bool auto_poll_task_exited = true;
static bool poll_suspended = false; // 轮询暂停标志
// 移除不再使用的变量

// 全局缓存管理器
static ModbusCacheManager g_cache_manager = {0};

// 辅助函数：从NVS读取三个通道的配置
__attribute__((unused)) static void
read_uart_configs_from_nvs(int *baudrates, uart_word_length_t *data_bits,
                           uart_parity_t *parities, uart_stop_bits_t *stop_bits,
                           int *frame_times, int *frame_lens) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    return;
  }

  // 设置默认值
  for (int i = 0; i < 3; i++) {
    baudrates[i] = 9600;
    data_bits[i] = UART_DATA_8_BITS;
    parities[i] = UART_PARITY_DISABLE;
    stop_bits[i] = UART_STOP_BITS_1;
    frame_times[i] = 50;
    frame_lens[i] = 512;
  }

  // 读取三个通道的配置
  for (int ch = 1; ch <= 3; ch++) {
    char prefix[8];
    snprintf(prefix, sizeof(prefix), "ch%d", ch);
    int idx = ch - 1;

    // 读取波特率
    size_t size = 0;
    char *value = NULL;
    char key_name[32];

    snprintf(key_name, sizeof(key_name), "%s_baud_rate", prefix);
    if (nvs_get_str(nvs_handle, key_name, NULL, &size) == ESP_OK) {
      value = malloc(size);
      if (value && nvs_get_str(nvs_handle, key_name, value, &size) == ESP_OK) {
        baudrates[idx] = atoi(value);
      }
      free(value);
    }

    // 读取数据位
    snprintf(key_name, sizeof(key_name), "%s_data_bit", prefix);
    if (nvs_get_str(nvs_handle, key_name, NULL, &size) == ESP_OK) {
      value = malloc(size);
      if (value && nvs_get_str(nvs_handle, key_name, value, &size) == ESP_OK) {
        int data_bit = atoi(value);
        switch (data_bit) {
        case 5:
          data_bits[idx] = UART_DATA_5_BITS;
          break;
        case 6:
          data_bits[idx] = UART_DATA_6_BITS;
          break;
        case 7:
          data_bits[idx] = UART_DATA_7_BITS;
          break;
        default:
          data_bits[idx] = UART_DATA_8_BITS;
          break;
        }
      }
      free(value);
    }

    // 读取校验位
    snprintf(key_name, sizeof(key_name), "%s_check_bit", prefix);
    if (nvs_get_str(nvs_handle, key_name, NULL, &size) == ESP_OK) {
      value = malloc(size);
      if (value && nvs_get_str(nvs_handle, key_name, value, &size) == ESP_OK) {
        // 统一使用数字格式解析校验位
        if (strcmp(value, "0") == 0) {
          parities[idx] = UART_PARITY_DISABLE;
        } else if (strcmp(value, "1") == 0) {
          parities[idx] = UART_PARITY_ODD;
        } else if (strcmp(value, "2") == 0) {
          parities[idx] = UART_PARITY_EVEN;
        } else {
          parities[idx] = UART_PARITY_DISABLE; // 默认无校验
        }
      }
      free(value);
    }

    // 读取停止位
    snprintf(key_name, sizeof(key_name), "%s_stop_bit", prefix);
    if (nvs_get_str(nvs_handle, key_name, NULL, &size) == ESP_OK) {
      value = malloc(size);
      if (value && nvs_get_str(nvs_handle, key_name, value, &size) == ESP_OK) {
        if (strcmp(value, "1.5") == 0) {
          stop_bits[idx] = UART_STOP_BITS_1_5;
        } else if (strcmp(value, "2") == 0) {
          stop_bits[idx] = UART_STOP_BITS_2;
        } else {
          stop_bits[idx] = UART_STOP_BITS_1;
        }
      }
      free(value);
    }

    // 读取帧时间
    snprintf(key_name, sizeof(key_name), "%s_frame_time", prefix);
    if (nvs_get_str(nvs_handle, key_name, NULL, &size) == ESP_OK) {
      value = malloc(size);
      if (value && nvs_get_str(nvs_handle, key_name, value, &size) == ESP_OK) {
        frame_times[idx] = atoi(value);
      }
      free(value);
    }

    // 读取帧长度
    snprintf(key_name, sizeof(key_name), "%s_frame_len", prefix);
    if (nvs_get_str(nvs_handle, key_name, NULL, &size) == ESP_OK) {
      value = malloc(size);
      if (value && nvs_get_str(nvs_handle, key_name, value, &size) == ESP_OK) {
        frame_lens[idx] = atoi(value);
      }
      free(value);
    }
  }

  nvs_close(nvs_handle);
  ESP_LOGI(TAG,
           "Read UART configs from NVS - CH1: %d/%d/%d/%d/%d/%d, CH2: "
           "%d/%d/%d/%d/%d/%d, CH3: %d/%d/%d/%d/%d/%d",
           baudrates[0], data_bits[0], parities[0], stop_bits[0],
           frame_times[0], frame_lens[0], baudrates[1], data_bits[1],
           parities[1], stop_bits[1], frame_times[1], frame_lens[1],
           baudrates[2], data_bits[2], parities[2], stop_bits[2],
           frame_times[2], frame_lens[2]);
}

// CRC计算函数
uint16_t calculate_crc(uint8_t *data, size_t length) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void modbus_cache_poll_task(void *arg) {
  // 检查参数有效性
  if (arg == NULL) {
    ESP_LOGE(TAG, "无效的任务参数");
    delete_self_app_task_with_caps();
    return;
  }

  ModbusCacheTaskConfig *config = (ModbusCacheTaskConfig *)arg;
  if (config->items == NULL || config->items_count <= 0) {
    ESP_LOGE(TAG, "无效的 Modbus 配置，items=%p, count=%d", config->items,
             config->items_count);
    delete_self_app_task_with_caps();
    return;
  }

  ESP_LOGI(TAG, "轮询任务启动成功，配置项数量: %d", config->items_count);

  // 关键优化：一次性读取超时配置（优先读取storage命名空间下的chX_timeout，兼容旧的config/timeoutX）
  int ch1_timeout = 1000, ch2_timeout = 1000, ch3_timeout = 1000; // 默认值
  {
    nvs_handle_t nvs_handle_storage;
    if (nvs_open("storage", NVS_READONLY, &nvs_handle_storage) == ESP_OK) {
      char key[16];
      char buf[32];
      size_t len;
      // ch1_timeout
      snprintf(key, sizeof(key), "ch1_timeout");
      len = sizeof(buf);
      if (nvs_get_str(nvs_handle_storage, key, buf, &len) == ESP_OK)
        ch1_timeout = atoi(buf);
      // ch2_timeout
      snprintf(key, sizeof(key), "ch2_timeout");
      len = sizeof(buf);
      if (nvs_get_str(nvs_handle_storage, key, buf, &len) == ESP_OK)
        ch2_timeout = atoi(buf);
      // ch3_timeout
      snprintf(key, sizeof(key), "ch3_timeout");
      len = sizeof(buf);
      if (nvs_get_str(nvs_handle_storage, key, buf, &len) == ESP_OK)
        ch3_timeout = atoi(buf);
      nvs_close(nvs_handle_storage);
    }
    // 兼容旧命名空间/键名
    nvs_handle_t nvs_handle_legacy;
    if ((ch1_timeout == DEFAULT_REPLY_TIMEOUT_MS ||
         ch2_timeout == DEFAULT_REPLY_TIMEOUT_MS ||
         ch3_timeout == DEFAULT_REPLY_TIMEOUT_MS) &&
        nvs_open("config", NVS_READONLY, &nvs_handle_legacy) == ESP_OK) {
      char buf[32];
      size_t len;
      // timeout1
      len = sizeof(buf);
      if (ch1_timeout == DEFAULT_REPLY_TIMEOUT_MS &&
          nvs_get_str(nvs_handle_legacy, "timeout1", buf, &len) == ESP_OK)
        ch1_timeout = atoi(buf);
      // timeout2
      len = sizeof(buf);
      if (ch2_timeout == DEFAULT_REPLY_TIMEOUT_MS &&
          nvs_get_str(nvs_handle_legacy, "timeout2", buf, &len) == ESP_OK)
        ch2_timeout = atoi(buf);
      // timeout3
      len = sizeof(buf);
      if (ch3_timeout == DEFAULT_REPLY_TIMEOUT_MS &&
          nvs_get_str(nvs_handle_legacy, "timeout3", buf, &len) == ESP_OK)
        ch3_timeout = atoi(buf);
      nvs_close(nvs_handle_legacy);
    }
    ESP_LOGI(TAG, "读取超时配置(兼容): CH1=%dms, CH2=%dms, CH3=%dms",
             ch1_timeout, ch2_timeout, ch3_timeout);
  }

  static const char *TAG = "MODBUS_CACHE_POLL";

  // 设置固定的轮询间隔 - 20秒
  const int poll_time = AUTO_POLL_CYCLE_MS;

  while (1) {
    TickType_t cycle_start_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "开始轮询周期，共有 %d 个真实数据表项",
             g_cache_manager.real_data_count);

    // 检查是否有数据表需要轮询
    if (g_cache_manager.real_data_count == 0 ||
        g_cache_manager.real_data_table == NULL) {
      ESP_LOGW(TAG, "没有数据表项需要轮询，等待下个周期");
      // 等待5秒
      vTaskDelay(pdMS_TO_TICKS(AUTO_NO_ITEMS_WAIT_MS));
      continue;
    }

    // 基于原始配置项进行轮询，每个配置项使用自己的超时时间
    for (int i = 0; i < config->items_count; i++) {
      ModbusCacheItemConfig *item = &config->items[i];

      // 跳过未启用的配置项
      if (!item->enabled) {
        continue;
      }

      // 解析配置项参数
      uint8_t slave_addr = (uint8_t)atoi(item->slave_addr);
      uint8_t function_code = (uint8_t)atoi(item->function_code);
      uint16_t start_register = (uint16_t)atoi(item->register_addr);
      uint16_t register_count = (uint16_t)atoi(item->register_num);
      if ((function_code != 0x03 && function_code != 0x04) ||
          register_count > MODBUS_MAX_REGISTER_COUNT ||
          !modbus_u16_range_valid(start_register, register_count)) {
        ESP_LOGW(TAG,
                 "跳过无效旧缓存轮询项 %d: slave=%u fc=0x%02X addr=%u qty=%u",
                 i + 1, slave_addr, function_code, start_register,
                 register_count);
        continue;
      }

      // 关键修复：根据实际工作模式使用正确的"回复超时时间"（使用预读取的配置）
      int timeout;
      uart_config_mode_t current_mode = get_uart_config_mode();

      if (current_mode == UART_CONFIG_MODE_SLAVE_FOLLOW) {
        // 从机跟随模式：使用CH1/CH2的较大超时时间
        timeout = (ch1_timeout > ch2_timeout) ? ch1_timeout : ch2_timeout;
        ESP_LOGD(TAG, "从机跟随模式，使用CH1/CH2最大回复超时时间: %d ms",
                 timeout);
      } else {
        // 正常模式：使用CH3回复超时时间，如果未配置则使用配置项超时时间
        timeout = ch3_timeout > 0 ? ch3_timeout : atoi(item->timeout);
        if (ch3_timeout > 0) {
          ESP_LOGD(TAG, "正常模式，使用CH3回复超时时间: %d ms", timeout);
        } else {
          ESP_LOGD(TAG, "正常模式，CH3超时未配置，使用配置项超时时间: %d ms",
                   timeout);
        }
      }

      ESP_LOGI(TAG, "轮询配置项 %d: 从机%d, 功能码%d, 寄存器%d+%d, 超时%dms",
               i + 1, slave_addr, function_code, start_register, register_count,
               timeout);

      // 使用async_uart的任务管理接口暂停接收任务
      suspend_all_uart_rx_tasks();
      vTaskDelay(pdMS_TO_TICKS(UART_TASK_SWITCH_DELAY_MS));

      // 不在缓存实现中配置 UART；发送时由 smart_send_data_to_ch3/临时参数承担

      // 构建Modbus请求
      uint8_t request[MODBUS_RTU_READ_REQ_LEN];
      request[0] = slave_addr;
      request[1] = function_code;
      request[2] = (start_register >> 8) & 0xFF;
      request[3] = start_register & 0xFF;
      request[4] = (register_count >> 8) & 0xFF;
      request[5] = register_count & 0xFF;

      uint16_t crc = calculate_crc(request, 6);
      request[6] = crc & 0xFF;
      request[7] = (crc >> 8) & 0xFF;

      ESP_LOGI(TAG, "发送请求: 从机%d, 寄存器%d+%d", slave_addr, start_register,
               register_count);

      // 使用智能发送函数，自动适应工作模式和通道参数
      // 注意：配置轮询没有明确的来源通道，在从机跟随模式下使用CH1作为默认来源
      uint8_t source_channel =
          (current_mode == UART_CONFIG_MODE_SLAVE_FOLLOW) ? 1 : 0;
      esp_err_t send_ret = smart_send_data_to_ch3(current_mode, source_channel,
                                                  request, sizeof(request));
      if (send_ret != ESP_OK) {
        ESP_LOGE(TAG, "智能发送轮询请求失败: %s (从机%d, 来源通道CH%d)",
                 esp_err_to_name(send_ret), slave_addr, source_channel);
      }

      // 恢复接收任务以接收响应
      resume_all_uart_rx_tasks();

      ESP_LOGI(TAG, "等待CH3响应，超时时间: %d ms", timeout);

      // 轮询检查CH3通道缓冲区是否有响应数据
      uint8_t response_buffer[RESPONSE_BUFFER_SIZE];
      uint64_t response_timestamp;
      int response_received = 0;
      int wait_time = 0;
      const int check_interval =
          CH3_RESPONSE_CHECK_INTERVAL_MS; // 每10ms检查一次

      while (wait_time < timeout && !response_received) {
        int data_len = pop_channel_data(
            3, response_buffer, sizeof(response_buffer), &response_timestamp);
        if (data_len > 0) {
          ESP_LOGI(TAG, "从CH3接收到响应数据，长度: %d 字节", data_len);
          ESP_LOG_BUFFER_HEXDUMP(TAG, response_buffer, data_len, ESP_LOG_INFO);

          // 严格响应校验：地址/功能码/字节数/CRC/异常帧
          bool frame_valid = true;
          // 1) 地址与功能码匹配
          if (data_len < 5 || response_buffer[0] != slave_addr) {
            frame_valid = false;
            ESP_LOGW(TAG, "响应从机地址不匹配或长度不足");
          } else {
            uint8_t resp_fc = response_buffer[1];
            if ((resp_fc & 0x80) != 0) {
              // 异常响应帧，记录并跳过缓存更新
              frame_valid = false;
              ESP_LOGW(TAG, "收到异常响应: 从机%d 异常码=0x%02X",
                       response_buffer[0],
                       (data_len >= 3 ? response_buffer[2] : 0xFF));
            } else if (resp_fc != function_code) {
              frame_valid = false;
              ESP_LOGW(TAG, "功能码不匹配: 期望=%u 实际=%u", function_code,
                       resp_fc);
            }
          }

          // 2) 字节数一致性与期望帧长
          size_t expected_payload_len = (size_t)register_count * 2;
          size_t expected_total_len =
              3 + expected_payload_len + 2; // addr/fc/bytes + data + CRC
          if (frame_valid) {
            if ((size_t)data_len < expected_total_len) {
              frame_valid = false;
              ESP_LOGW(TAG, "响应长度不足: 期望=%zu 实际=%d",
                       expected_total_len, data_len);
            } else {
              uint8_t byte_count = response_buffer[2];
              if (byte_count != expected_payload_len) {
                frame_valid = false;
                ESP_LOGW(TAG, "字节数字段不一致: 期望=%zu 实际=%u",
                         expected_payload_len, byte_count);
              }
            }
          }

          // 3) 对精确帧长做CRC校验（即使收到粘包，只校验前 expected_total_len）
          if (frame_valid) {
            uint16_t rx_crc = (uint16_t)response_buffer[expected_total_len - 1]
                                  << 8 |
                              (uint16_t)response_buffer[expected_total_len - 2];
            uint16_t calc_crc =
                calculate_crc(response_buffer, expected_total_len - 2);
            if (rx_crc != calc_crc) {
              frame_valid = false;
              ESP_LOGW(TAG, "CRC校验失败: 期望=0x%04X 实际=0x%04X", calc_crc,
                       rx_crc);
            }
          }

          if (frame_valid) {
            // 仅用精确帧长更新缓存，避免粘包干扰
            esp_err_t cache_update_result =
                update_real_data(slave_addr, start_register, register_count,
                                 response_buffer, expected_total_len);
            if (cache_update_result == ESP_OK) {
              ESP_LOGI(TAG, "配置项 %d 数据更新成功", i + 1);
            } else {
              ESP_LOGW(TAG, "配置项 %d 数据更新失败: %s", i + 1,
                       esp_err_to_name(cache_update_result));
            }
          } else {
            ESP_LOGW(TAG, "忽略无效响应，不更新缓存");
          }

          response_received = 1;
          break;
        }

        vTaskDelay(pdMS_TO_TICKS(check_interval));
        wait_time += check_interval;
      }

      if (!response_received) {
        ESP_LOGW(TAG, "配置项 %d 响应超时 (%d ms)", i + 1, timeout);
      } else {
        ESP_LOGI(TAG, "配置项 %d 处理完成", i + 1);
      }

      // 关键修复：配置项间安全间隔，防止连续请求冲突
      // 使用超时时间的10%作为安全间隔，最少50ms，最多500ms
      uint32_t safety_interval = timeout / 10;
      if (safety_interval < 50)
        safety_interval = 50;
      if (safety_interval > 500)
        safety_interval = 500;

      ESP_LOGI(TAG, "配置项间安全间隔: %u ms (基于超时时间 %d ms)",
               safety_interval, timeout);
      vTaskDelay(pdMS_TO_TICKS(safety_interval));
    }

    // 计算本次循环实际耗时
    TickType_t current_time = xTaskGetTickCount();
    int elapsed_ms = (current_time - cycle_start_time) * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "循环完成，耗时 %d 毫秒", elapsed_ms);

    // 打印缓存状态（每轮询周期打印一次）
    print_cache_status();

    // 计算剩余时间
    int remaining_time = poll_time - elapsed_ms;

    if (remaining_time > 0) {
      // 等待剩余时间
      vTaskDelay(pdMS_TO_TICKS(remaining_time));
    }
  }

  // 任务退出（通常不会执行到这里）
  ESP_LOGI(TAG, "Modbus缓存轮询任务退出");
}

// 停止Modbus缓存任务
void stop_modbus_cache_tasks(void) {
  bool deleted_task = false;
  bool auto_cleanup_allowed = true;

  if (modbus_cache_task_handle != NULL) {
    delete_app_task_with_caps(modbus_cache_task_handle);
    modbus_cache_task_handle = NULL;
    deleted_task = true;
  }

  auto_cache_tasks_stopping = true;
  poll_suspended = false;

  if (auto_cache_task_handle != NULL) {
    if (!auto_cache_wait_task_exit(&auto_cache_task_exited, 1500)) {
      ESP_LOGW(TAG, "自动缓存主任务未及时退出，强制删除");
      delete_app_task_with_caps(auto_cache_task_handle);
      auto_cache_task_exited = true;
      auto_cleanup_allowed = false;
    }
    auto_cache_task_handle = NULL;
  }

  if (auto_poll_task_handle != NULL) {
    if (!auto_cache_wait_task_exit(&auto_poll_task_exited, 1500)) {
      ESP_LOGW(TAG, "自动缓存轮询任务未及时退出，强制删除");
      delete_app_task_with_caps(auto_poll_task_handle);
      auto_poll_task_exited = true;
      auto_cleanup_allowed = false;
    }
    auto_poll_task_handle = NULL;
  }

  if (deleted_task) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  auto_cache_reset_runtime_state();
  if (auto_cleanup_allowed) {
    auto_cache_clear_all();
  } else {
    ESP_LOGW(TAG, "自动缓存任务为强制删除，跳过共享资源释放以避免并发释放");
  }
  auto_cache_tasks_stopping = false;

  // 同时停止虚拟从机响应任务
  stop_virtual_slave_response_task();

  // 停止手动缓存任务
  stop_manual_cache_tasks();

  // 销毁缓存管理器
  destroy_cache_manager();
}

// 启动Modbus缓存任务
esp_err_t start_modbus_cache_task(ModbusCacheTaskConfig *config) {
  if (config == NULL || config->items == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // 创建任务 - 固定到工作核心，使用低优先级避免影响实时响应
  BaseType_t ret = create_app_task_psram(
      modbus_cache_poll_task, "modbus_cache_task", 12288, (void *)config,
      2, // 使用低优先级（2）避免影响实时任务
      &modbus_cache_task_handle, SX_WORK_CORE_ID);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Modbus cache task");
    return ESP_FAIL;
  }

  return ESP_OK;
}

// modbus_cache_workmode初始化函数
esp_err_t modbus_cache_init(void) {
  static const char *TAG = "MODBUS_CACHE_INIT";
  ESP_LOGI(TAG, "=== 初始化Modbus缓存系统 ===");

  // 初始化NVS - 不要清除整个NVS，避免丢失串口配置等其他重要数据
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(TAG, "NVS需要重新初始化，但不清除现有配置");
    // 不调用nvs_flash_erase()，避免清除串口配置等重要数据
    ret = nvs_flash_init();
    if (ret != ESP_OK) {
      ESP_LOGE(TAG, "NVS初始化失败: %s", esp_err_to_name(ret));
      return ret;
    }
  }
  ESP_ERROR_CHECK(ret);

  // 打开NVS存储
  nvs_handle_t storage_handle;
  ret = nvs_open("storage", NVS_READONLY, &storage_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "无法打开NVS存储: %s", esp_err_to_name(ret));
    return ret;
  }

  // 读取自动透明缓存开关状态
  uint8_t auto_cache = 1; // 默认启用自动透明缓存
  ret = nvs_get_u8(storage_handle, "auto_cache", &auto_cache);
  if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "读取自动透明缓存开关失败: %s，使用默认值",
             esp_err_to_name(ret));
  }

  ESP_LOGI(TAG, "自动透明缓存模式: %s", auto_cache ? "启用" : "禁用");

  if (auto_cache) {
    // 启动自动智能缓存模式
    ESP_LOGI(TAG, "启动自动智能透明缓存模式");
    ESP_LOGW(TAG, "当前为自动模式，不会读取手动Modbus配置！如需手动配置，请在We"
                  "b界面关闭自动透明缓存");
    nvs_close(storage_handle);
    // 调用自动智能缓存初始化函数
    return init_auto_intelligent_cache();
  } else {
    ESP_LOGI(TAG, "启动手动配置轮询缓存模式");
    // 调用原有的手动轮询初始化函数
    esp_err_t result = init_manual_polling_cache(storage_handle);

    // 初始化完成后的简单日志
    if (result == ESP_OK) {
      ESP_LOGI(TAG, "手动轮询模式初始化成功");
      // 延迟1秒后运行测试，让任务有时间启动
      vTaskDelay(pdMS_TO_TICKS(1000));
      test_modbus_cache_config();
    } else {
      ESP_LOGE(TAG, "手动轮询模式初始化失败: %s", esp_err_to_name(result));
    }

    return result;
  }
}

// ==================== 智能透明缓存系统实现 ====================

// Modbus功能码定义
#define MODBUS_READ_COILS 0x01
#define MODBUS_READ_DISCRETE 0x02
#define MODBUS_READ_HOLDING 0x03
#define MODBUS_READ_INPUT 0x04
#define MODBUS_WRITE_SINGLE_COIL 0x05
#define MODBUS_WRITE_SINGLE_REG 0x06
#define MODBUS_WRITE_MULTIPLE_COILS 0x0F
#define MODBUS_WRITE_MULTIPLE_REGS 0x10
#define MODBUS_MAX_BIT_READ_COUNT 2000
#define MODBUS_AUTO_CACHE_BANK_COUNT 4

typedef enum {
  AUTO_CACHE_PAYLOAD_BITS = 0,
  AUTO_CACHE_PAYLOAD_REG16 = 1,
} auto_cache_payload_kind_t;

// 从机缓存结构体
typedef struct slave_cache {
  uint8_t slave_id;                // 从机号
  uint8_t function_code;           // 01/02/03/04，作为分表key
  auto_cache_payload_kind_t kind;  // bit型或16位寄存器型
  uint16_t start_addr;             // 起始地址
  uint16_t quantity;               // 点数：01/02为bit数量，03/04为寄存器数量
  size_t payload_len;              // payload字节数
  uint8_t *payload;                // 01/02为bit-packed，03/04为寄存器原始字节
  uint64_t last_access_time;       // 最后访问时间
  uint64_t last_poll_time;         // 最后轮询时间
  uint8_t source_channel;          // 创建此缓存的来源通道
  bool data_valid;                 // 数据是否有效（已从从机获取）
  bool delete_pending;             // 轮询失败达到阈值，等待遍历任务安全删除
  bool is_polling;                 // 后台轮询正在使用此节点，延迟释放
  bool refresh_urgent;             // 主站刚访问/新建/失效后优先刷新
  uint32_t access_hits_window;     // 当前调度窗口内的主站访问次数
  uint32_t access_heat;            // 历史窗口衰减后的访问热度
  uint64_t last_hit_time;          // 最近一次主站访问时间
  uint64_t last_poll_start_time;   // 最近一次刷新开始时间
  uint32_t avg_poll_cost_ms;       // 实测刷新耗时EWMA
  uint32_t est_wire_cost_ms;       // 按长度/波特率估算的传输成本
  uint32_t target_interval_ms;     // 本轮调度计算出的目标刷新间隔
  uint32_t min_refresh_interval_ms;// 由刷新成本决定的最小合理间隔
  uint64_t next_eligible_time;     // 长帧/失败退避后下次可调度时间
  bool needs_verification;  // 是否需要写后验证
  uint16_t verify_addr;     // 待验证的寄存器地址
  uint16_t verify_value;    // 预期的验证值
  uint8_t poll_fail_count;  // 轮询连续失败次数
  struct slave_cache *next; // 链表指针
} slave_cache_t;

typedef struct {
  slave_cache_t *banks[MODBUS_AUTO_CACHE_BANK_COUNT];
  SemaphoreHandle_t mutex;
} auto_cache_manager_t;

// 全局变量
static auto_cache_manager_t g_auto_cache = {0};   // 01/02/03/04独立缓存表
static uint64_t last_gc_time = 0;                 // 上次垃圾回收时间
static int timeout1, timeout2, timeout3;          // 通道超时配置
static uint32_t total_access_hits_window = 0;
static uint32_t total_access_heat = 0;
static uint32_t auto_cache_entry_count = 0; // 热度全为0时用于均分访问比例
static uint32_t ch3_busy_window_ms = 0;     // 当前调度窗口内CH3实际轮询耗时
static uint32_t ch3_utilization_permille = 0; // CH3利用率EWMA，千分比
static uint64_t scheduler_window_start = 0;

static void auto_cache_lock(void);
static void auto_cache_unlock(void);
static void remove_cache_entry(slave_cache_t *cache);
static bool auto_cache_is_bit_read(uint8_t function_code);

// 写请求状态管理
typedef enum {
  WRITE_STATE_IDLE,             // 空闲状态
  WRITE_STATE_WAITING_RESPONSE, // 等待写响应
} write_state_enum_t;

typedef struct {
  write_state_enum_t state; // 写请求状态
  uint8_t slave_id;         // 目标从机ID
  uint8_t function_code;    // 功能码
  int source_channel;       // 源通道
  uint64_t start_time;      // 开始时间
  int timeout_ms;           // 超时时间
  uint16_t start_addr;      // 写入地址
  uint16_t write_value;     // 写入值(单寄存器写入时)
  uint16_t register_count;  // 寄存器数量
} write_request_state_t;

static write_request_state_t write_state = {0};
static void auto_cache_reset_runtime_state(void) {
  poll_suspended = false;
  memset(&write_state, 0, sizeof(write_state));
  write_state.state = WRITE_STATE_IDLE;
  total_access_hits_window = 0;
  total_access_heat = 0;
  auto_cache_entry_count = 0;
  ch3_busy_window_ms = 0;
  ch3_utilization_permille = 0;
  scheduler_window_start = 0;
}

static bool auto_cache_wait_task_exit(volatile bool *exited,
                                      uint32_t timeout_ms) {
  uint32_t waited = 0;
  while (!*exited && waited < timeout_ms) {
    vTaskDelay(sx_ms_to_ticks(10));
    waited += 10;
  }
  return *exited;
}

// 获取当前时间（毫秒）
static uint64_t get_current_time_ms(void) {
  return esp_timer_get_time() / 1000;
}

static void auto_cache_notify_poll_task(void) {
  TaskHandle_t handle = auto_poll_task_handle;
  if (handle != NULL) {
    xTaskNotifyGive(handle);
  }
}

static uint32_t auto_cache_response_frame_len(uint8_t function_code,
                                              uint16_t quantity) {
  if (auto_cache_is_bit_read(function_code)) {
    return 5U + (((uint32_t)quantity + 7U) / 8U);
  }
  return 5U + ((uint32_t)quantity * 2U);
}

static uint32_t auto_cache_data_bits_count(uart_word_length_t data_bits) {
  switch (data_bits) {
  case UART_DATA_5_BITS:
    return 5;
  case UART_DATA_6_BITS:
    return 6;
  case UART_DATA_7_BITS:
    return 7;
  case UART_DATA_8_BITS:
  default:
    return 8;
  }
}

static uint32_t auto_cache_stop_bits_x2(uart_stop_bits_t stop_bits) {
  switch (stop_bits) {
  case UART_STOP_BITS_1_5:
    return 3;
  case UART_STOP_BITS_2:
    return 4;
  case UART_STOP_BITS_1:
  default:
    return 2;
  }
}

static uint32_t auto_cache_bits_per_char_x2(
    const channel_uart_config_t *config) {
  uint32_t data_bits = 8;
  uint32_t parity_bits = 0;
  uint32_t stop_bits_x2 = 2;

  if (config != NULL) {
    data_bits = auto_cache_data_bits_count(config->data_bits);
    parity_bits = (config->parity == UART_PARITY_DISABLE) ? 0 : 1;
    stop_bits_x2 = auto_cache_stop_bits_x2(config->stop_bits);
  }

  return (2U * (1U + data_bits + parity_bits)) + stop_bits_x2;
}

static void auto_cache_get_poll_uart_config(slave_cache_t *cache,
                                            channel_uart_config_t *config) {
  if (config == NULL)
    return;

  memset(config, 0, sizeof(*config));
  config->channel = 3;
  config->baudrate = DEFAULT_UART_BAUDRATE;
  config->data_bits = UART_DATA_8_BITS;
  config->parity = UART_PARITY_DISABLE;
  config->stop_bits = UART_STOP_BITS_1;
  config->frame_time = DEFAULT_UART_FRAME_TIME_MS;
  config->frame_len = DEFAULT_UART_FRAME_LEN;
  config->timeout = timeout3 > 0 ? timeout3 : AUTO_CACHE_POLL_RESPONSE_TIMEOUT_MS;

  uart_config_mode_t current_mode = get_uart_config_mode();
  int config_channel = 3;
  if (current_mode == UART_CONFIG_MODE_SLAVE_FOLLOW && cache != NULL &&
      (cache->source_channel == 1 || cache->source_channel == 2)) {
    config_channel = cache->source_channel;
  }

  channel_uart_config_t runtime_config;
  if (get_current_runtime_config(config_channel, &runtime_config) == ESP_OK) {
    *config = runtime_config;
    config->channel = 3;
  } else if (get_channel_uart_config_from_nvs(config_channel, &runtime_config) ==
             ESP_OK) {
    *config = runtime_config;
    config->channel = 3;
  }
}

static uint32_t auto_cache_estimate_wire_cost_ms(slave_cache_t *cache) {
  if (cache == NULL)
    return AUTO_CACHE_BASE_MIN_INTERVAL_MS;

  // 估算一次完整RTU读事务占用CH3的时间：
  // 请求帧 + 响应帧 + RTU 3.5字符间隔 + 主从周转/从机处理余量。
  // 串口参数优先使用当前运行配置；从机跟随模式下使用来源通道的串口参数。
  channel_uart_config_t config;
  auto_cache_get_poll_uart_config(cache, &config);
  uint32_t baudrate = config.baudrate > 0 ? (uint32_t)config.baudrate
                                          : DEFAULT_UART_BAUDRATE;
  uint32_t bits_per_char_x2 = auto_cache_bits_per_char_x2(&config);
  uint32_t frame_bytes =
      MODBUS_RTU_READ_REQ_LEN +
      auto_cache_response_frame_len(cache->function_code, cache->quantity);

  uint64_t wire_time_ms =
      (((uint64_t)frame_bytes * bits_per_char_x2 * 1000ULL) +
       ((uint64_t)baudrate * 2ULL) - 1ULL) /
      ((uint64_t)baudrate * 2ULL);
  uint64_t gap_ms =
      (((uint64_t)bits_per_char_x2 * 35ULL * 1000ULL) +
       ((uint64_t)baudrate * 20ULL) - 1ULL) /
      ((uint64_t)baudrate * 20ULL);
  uint64_t total_ms =
      wire_time_ms + gap_ms + AUTO_CACHE_RTU_TURNAROUND_MS +
      AUTO_CACHE_SLAVE_MARGIN_MS;

  if (total_ms < AUTO_CACHE_BASE_MIN_INTERVAL_MS) {
    total_ms = AUTO_CACHE_BASE_MIN_INTERVAL_MS;
  }
  if (total_ms > UINT32_MAX) {
    total_ms = UINT32_MAX;
  }
  return (uint32_t)total_ms;
}

static uint32_t auto_cache_refresh_cost_ms(slave_cache_t *cache) {
  if (cache == NULL)
    return AUTO_CACHE_BASE_MIN_INTERVAL_MS;

  uint32_t observed = cache->avg_poll_cost_ms;
  uint32_t estimated = cache->est_wire_cost_ms;
  uint32_t cost = observed > estimated ? observed : estimated;
  return (cost > 0) ? cost : AUTO_CACHE_BASE_MIN_INTERVAL_MS;
}

static uint32_t auto_cache_min_refresh_interval_ms(slave_cache_t *cache) {
  uint64_t min_interval =
      (uint64_t)auto_cache_refresh_cost_ms(cache) * AUTO_CACHE_COST_MULTIPLIER;
  if (min_interval < AUTO_CACHE_BASE_MIN_INTERVAL_MS) {
    min_interval = AUTO_CACHE_BASE_MIN_INTERVAL_MS;
  }
  if (min_interval > UINT32_MAX) {
    min_interval = UINT32_MAX;
  }
  return (uint32_t)min_interval;
}

static uint32_t auto_cache_max_stale_ms(slave_cache_t *cache) {
  // 低频条目的保底刷新时间。成本越高的长帧允许更长保底周期，避免为了
  // “公平”频繁打断短帧和高频条目的实时刷新。
  uint64_t by_cost = (uint64_t)auto_cache_refresh_cost_ms(cache) * 8ULL;
  uint64_t max_stale = by_cost > AUTO_CACHE_MAX_STALE_MIN_MS
                           ? by_cost
                           : AUTO_CACHE_MAX_STALE_MIN_MS;
  if (max_stale > UINT32_MAX) {
    max_stale = UINT32_MAX;
  }
  return (uint32_t)max_stale;
}

static void auto_cache_reset_window_stats_locked(void) {
  uint64_t total_heat = 0;

  // 窗口结算只在这里做，访问路径只累加access_hits_window。
  // 这样热度是“最近一段时间的相对比例”，不会因为设备运行时间变长而永久偏向旧热点。
  for (int bank = 0; bank < MODBUS_AUTO_CACHE_BANK_COUNT; bank++) {
    slave_cache_t *current = g_auto_cache.banks[bank];
    while (current != NULL) {
      uint64_t decayed =
          ((uint64_t)current->access_heat * AUTO_CACHE_DECAY_NUM) /
          AUTO_CACHE_DECAY_DEN;
      uint64_t next_heat = decayed + current->access_hits_window;
      current->access_heat =
          (next_heat > UINT32_MAX) ? UINT32_MAX : (uint32_t)next_heat;
      current->access_hits_window = 0;
      total_heat += current->access_heat;
      current = current->next;
    }
  }

  total_access_heat =
      (total_heat > UINT32_MAX) ? UINT32_MAX : (uint32_t)total_heat;
  total_access_hits_window = 0;
}

static void auto_cache_refresh_scheduler_stats(uint64_t now_ms) {
  if (scheduler_window_start == 0) {
    scheduler_window_start = now_ms;
    return;
  }

  uint64_t elapsed_ms = now_ms - scheduler_window_start;
  if (elapsed_ms < AUTO_CACHE_SCHED_WINDOW_MS)
    return;

  uint32_t window_util = 0;
  if (elapsed_ms > 0) {
    uint64_t util = ((uint64_t)ch3_busy_window_ms * 1000ULL) / elapsed_ms;
    window_util = (util > 1000U) ? 1000U : (uint32_t)util;
  }

  // CH3利用率也做EWMA，避免某一秒内的超时或突发请求让调度频繁抖动。
  ch3_utilization_permille =
      (ch3_utilization_permille * AUTO_CACHE_UTIL_EWMA_OLD +
       window_util * AUTO_CACHE_UTIL_EWMA_NEW) /
      (AUTO_CACHE_UTIL_EWMA_OLD + AUTO_CACHE_UTIL_EWMA_NEW);
  ch3_busy_window_ms = 0;

  auto_cache_lock();
  auto_cache_reset_window_stats_locked();
  auto_cache_unlock();

  scheduler_window_start = now_ms;
}

static void auto_cache_mark_access(slave_cache_t *cache, uint64_t now_ms,
                                   bool urgent) {
  if (cache == NULL)
    return;

  if (cache->access_hits_window < UINT32_MAX) {
    cache->access_hits_window++;
  }
  if (total_access_hits_window < UINT32_MAX) {
    total_access_hits_window++;
  }
  // last_access_time只用于GC存活判断；热度由access_hits_window/access_heat决定。
  cache->last_access_time = now_ms;
  cache->last_hit_time = now_ms;
  if (urgent) {
    cache->refresh_urgent = true;
  }
}

static uint32_t auto_cache_effective_total_heat(void) {
  uint64_t total = (uint64_t)total_access_heat + total_access_hits_window;
  return (total > UINT32_MAX) ? UINT32_MAX : (uint32_t)total;
}

static uint32_t auto_cache_access_ratio_permille(slave_cache_t *cache) {
  uint32_t total_heat = auto_cache_effective_total_heat();
  if (cache == NULL)
    return 0;

  if (total_heat == 0) {
    // 如果整段时间内主站几乎不读，热度可能全部衰减到0。此时不应把所有条目
    // 都当冷门保底处理，而是按条目数均分比例，让CH3空闲时做全量快速刷新。
    uint32_t count = auto_cache_entry_count > 0 ? auto_cache_entry_count : 1U;
    uint32_t ratio = AUTO_CACHE_HEAT_SCALE / count;
    return ratio > 0 ? ratio : 1U;
  }

  uint64_t entry_heat =
      (uint64_t)cache->access_heat + cache->access_hits_window;
  uint64_t ratio =
      (entry_heat * AUTO_CACHE_HEAT_SCALE) / total_heat;
  return (ratio > AUTO_CACHE_HEAT_SCALE) ? AUTO_CACHE_HEAT_SCALE
                                         : (uint32_t)ratio;
}

static uint32_t auto_cache_target_interval_ms(slave_cache_t *cache) {
  if (cache == NULL)
    return AUTO_CACHE_FULL_REFRESH_MIN_MS;

  uint32_t min_interval = auto_cache_min_refresh_interval_ms(cache);
  uint32_t ratio = auto_cache_access_ratio_permille(cache);
  uint64_t target = min_interval;

  // 总线空闲时不区分冷热，直接按每个条目的成本下限刷新，尽量提高整体实时性。
  if (ch3_utilization_permille < AUTO_CACHE_EMPTY_BUS_UTIL) {
    target = min_interval;
  } else {
    // 总线开始繁忙后才按相对访问比例分层。比例越高，越接近成本下限；
    // 比例越低，目标间隔越长，但仍会被max_stale兜底。
    if (ratio >= 300U) {
      target = min_interval;
    } else if (ratio >= 100U) {
      target = (uint64_t)min_interval * 2U;
    } else if (ratio > 0U) {
      target = (uint64_t)min_interval * 4U;
    } else {
      target = (uint64_t)min_interval * 6U;
    }
  }

  if (ch3_utilization_permille > AUTO_CACHE_BUSY_BUS_UTIL) {
    // 极忙时整体降速一档，给正在响应主站请求和写事务留出余量。
    target *= 2U;
  }

  uint32_t max_stale = auto_cache_max_stale_ms(cache);
  if (target < min_interval) {
    target = min_interval;
  }
  if (target < AUTO_CACHE_FULL_REFRESH_MIN_MS) {
    target = AUTO_CACHE_FULL_REFRESH_MIN_MS;
  }
  if (target > max_stale) {
    target = max_stale;
  }
  if (target > UINT32_MAX) {
    target = UINT32_MAX;
  }

  cache->min_refresh_interval_ms = min_interval;
  cache->target_interval_ms = (uint32_t)target;
  return cache->target_interval_ms;
}

static uint32_t auto_cache_poll_delay_ms(bool has_urgent) {
  if (has_urgent || ch3_utilization_permille < AUTO_CACHE_HIGH_BUS_UTIL) {
    return AUTO_CACHE_POLL_DELAY_FAST_MS;
  }
  return AUTO_CACHE_POLL_DELAY_NORMAL_MS;
}

static uint32_t auto_cache_poll_priority(slave_cache_t *cache, uint64_t now_ms,
                                         bool *has_urgent) {
  if (cache == NULL || cache->delete_pending || cache->is_polling)
    return 0;

  uint64_t since_attempt_ms = cache->last_poll_start_time == 0
                                  ? UINT64_MAX
                                  : now_ms - cache->last_poll_start_time;
  if (cache->poll_fail_count > 0) {
    // 失败退避按“上次尝试时间”计算，不按上次成功时间计算。否则长期失败的
    // 条目会因为数据很旧而持续抢占CH3。
    uint64_t backoff = (uint64_t)AUTO_CACHE_FAIL_BACKOFF_BASE_MS
                       << (cache->poll_fail_count - 1);
    if (backoff > AUTO_CACHE_FAIL_BACKOFF_MAX_MS) {
      backoff = AUTO_CACHE_FAIL_BACKOFF_MAX_MS;
    }
    if (since_attempt_ms < backoff)
      return 0;
  }

  if (now_ms < cache->next_eligible_time)
    return 0;

  uint32_t max_stale_ms = auto_cache_max_stale_ms(cache);
  uint64_t data_age_ms =
      (cache->last_poll_time == 0) ? max_stale_ms
                                   : now_ms - cache->last_poll_time;

  if (cache->refresh_urgent || !cache->data_valid) {
    // 新建、写后失效、主站读到过期数据都走紧急刷新。仍然受next_eligible_time
    // 和失败退避约束，避免长帧或故障设备连续占满总线。
    if (has_urgent != NULL) {
      *has_urgent = true;
    }
    uint64_t priority = AUTO_CACHE_URGENT_PRIORITY + (data_age_ms / 2U);
    return priority > AUTO_CACHE_MAX_PRIORITY ? AUTO_CACHE_MAX_PRIORITY
                                              : (uint32_t)priority;
  }

  uint64_t elapsed_ms =
      (since_attempt_ms == UINT64_MAX) ? data_age_ms : since_attempt_ms;
  uint32_t target_ms = auto_cache_target_interval_ms(cache);
  uint32_t cost_ms = auto_cache_refresh_cost_ms(cache);

  if (elapsed_ms < target_ms && data_age_ms < max_stale_ms)
    return 0;

  uint32_t ratio = auto_cache_access_ratio_permille(cache);
  // 优先级由三部分组成：
  // - stale_score：距离目标刷新间隔越久，越该刷新；
  // - ratio_score：最近访问比例越高，越偏向高频条目；
  // - starvation：超过最大陈旧时间后加大权重，保证低频条目也能更新。
  uint64_t stale_score = target_ms > 0
                             ? ((elapsed_ms * 10000ULL) / target_ms)
                             : 10000ULL;
  uint64_t ratio_score = (uint64_t)ratio * 100ULL;
  uint64_t starvation =
      (data_age_ms >= max_stale_ms) ? AUTO_CACHE_STARVE_PRIORITY : 0ULL;
  uint64_t fail_penalty = (uint64_t)cache->poll_fail_count * 50000ULL;
  uint64_t cost_penalty = (uint64_t)cost_ms * 20ULL;

  uint64_t priority = starvation + stale_score + ratio_score;
  if (priority > cost_penalty + fail_penalty) {
    priority -= cost_penalty + fail_penalty;
  } else {
    priority = 1;
  }

  return priority > AUTO_CACHE_MAX_PRIORITY ? AUTO_CACHE_MAX_PRIORITY
                                            : (uint32_t)priority;
}

static slave_cache_t *auto_cache_select_poll_candidate(uint64_t now_ms,
                                                       bool *has_urgent) {
  slave_cache_t *best = NULL;
  uint32_t best_priority = 0;

  if (has_urgent != NULL) {
    *has_urgent = false;
  }

  auto_cache_lock();
  for (int bank = 0; bank < MODBUS_AUTO_CACHE_BANK_COUNT; bank++) {
    slave_cache_t *current = g_auto_cache.banks[bank];
    while (current != NULL) {
      uint32_t priority =
          auto_cache_poll_priority(current, now_ms, has_urgent);
      if (priority > best_priority) {
        best_priority = priority;
        best = current;
      }
      current = current->next;
    }
  }

  if (best != NULL) {
    // 选中后先标记is_polling，防止GC或失败删除在轮询过程中释放节点。
    // 轮询结束后必须调用auto_cache_finish_poll_candidate()清标记/延迟删除。
    best->is_polling = true;
  }
  auto_cache_unlock();

  return best;
}

static void auto_cache_finish_poll_candidate(slave_cache_t *cache) {
  if (cache == NULL)
    return;

  auto_cache_lock();
  cache->is_polling = false;
  if (cache->delete_pending) {
    remove_cache_entry(cache);
  }
  auto_cache_unlock();
}

static void auto_cache_record_poll_cost(slave_cache_t *cache,
                                        uint32_t elapsed_ms) {
  if (cache == NULL)
    return;

  if (elapsed_ms == 0) {
    elapsed_ms = 1;
  }
  if (UINT32_MAX - ch3_busy_window_ms < elapsed_ms) {
    ch3_busy_window_ms = UINT32_MAX;
  } else {
    ch3_busy_window_ms += elapsed_ms;
  }
  // 每次轮询结束后同时更新“实测成本”和“按当前串口参数估算的线缆成本”。
  // 调度只使用二者较大值，避免估算偏小或从机响应慢时过度调度同一个条目。
  uint32_t estimated_cost = auto_cache_estimate_wire_cost_ms(cache);
  uint64_t now_ms = get_current_time_ms();

  auto_cache_lock();
  if (cache->avg_poll_cost_ms == 0) {
    cache->avg_poll_cost_ms = elapsed_ms;
  } else {
    cache->avg_poll_cost_ms =
        (cache->avg_poll_cost_ms * AUTO_CACHE_COST_EWMA_OLD +
         elapsed_ms * AUTO_CACHE_COST_EWMA_NEW) /
        (AUTO_CACHE_COST_EWMA_OLD + AUTO_CACHE_COST_EWMA_NEW);
  }
  cache->est_wire_cost_ms = estimated_cost;
  cache->min_refresh_interval_ms = auto_cache_min_refresh_interval_ms(cache);
  cache->target_interval_ms = auto_cache_target_interval_ms(cache);
  uint64_t schedule_base = cache->last_poll_start_time > 0
                               ? cache->last_poll_start_time
                               : now_ms;
  uint64_t min_interval = cache->min_refresh_interval_ms;
  // next_eligible_time按轮询开始时间计算，而不是按结束时间计算。这样长帧不会
  // 被并发重入，同时也不会因为本次耗时已经覆盖了间隔而额外等待一轮。
  cache->next_eligible_time =
      (UINT64_MAX - schedule_base < min_interval)
          ? UINT64_MAX
          : schedule_base + min_interval;
  auto_cache_unlock();
}

static void auto_cache_lock(void) {
  if (g_auto_cache.mutex != NULL) {
    xSemaphoreTake(g_auto_cache.mutex, portMAX_DELAY);
  }
}

static void auto_cache_unlock(void) {
  if (g_auto_cache.mutex != NULL) {
    xSemaphoreGive(g_auto_cache.mutex);
  }
}

static int auto_cache_bank_index(uint8_t function_code) {
  switch (function_code) {
  case MODBUS_READ_COILS:
    return 0;
  case MODBUS_READ_DISCRETE:
    return 1;
  case MODBUS_READ_HOLDING:
    return 2;
  case MODBUS_READ_INPUT:
    return 3;
  default:
    return -1;
  }
}

static bool auto_cache_is_supported_read(uint8_t function_code) {
  return auto_cache_bank_index(function_code) >= 0;
}

static bool auto_cache_is_bit_read(uint8_t function_code) {
  return function_code == MODBUS_READ_COILS ||
         function_code == MODBUS_READ_DISCRETE;
}

static uint8_t auto_cache_write_target_function(uint8_t function_code) {
  switch (function_code) {
  case MODBUS_WRITE_SINGLE_COIL:
  case MODBUS_WRITE_MULTIPLE_COILS:
    return MODBUS_READ_COILS;
  case MODBUS_WRITE_SINGLE_REG:
  case MODBUS_WRITE_MULTIPLE_REGS:
    return MODBUS_READ_HOLDING;
  default:
    return 0;
  }
}

static size_t auto_cache_payload_len(uint8_t function_code, uint16_t quantity) {
  if (quantity == 0)
    return 0;
  return auto_cache_is_bit_read(function_code) ? ((size_t)quantity + 7U) / 8U
                                               : (size_t)quantity * 2U;
}

static uint16_t auto_cache_max_quantity(uint8_t function_code) {
  return auto_cache_is_bit_read(function_code) ? MODBUS_MAX_BIT_READ_COUNT
                                               : MODBUS_MAX_REGISTER_COUNT;
}

static bool auto_cache_valid_quantity(uint8_t function_code, uint16_t quantity) {
  return auto_cache_is_supported_read(function_code) && quantity > 0 &&
         quantity <= auto_cache_max_quantity(function_code);
}

static bool auto_cache_valid_u16_range(uint16_t start_addr, uint16_t quantity) {
  return modbus_u16_range_valid(start_addr, quantity);
}

static bool auto_cache_valid_range(uint8_t function_code, uint16_t start_addr,
                                   uint16_t quantity) {
  return auto_cache_valid_quantity(function_code, quantity) &&
         auto_cache_valid_u16_range(start_addr, quantity);
}

static const char *auto_cache_function_name(uint8_t function_code) {
  switch (function_code) {
  case MODBUS_READ_COILS:
    return "01线圈";
  case MODBUS_READ_DISCRETE:
    return "02离散输入";
  case MODBUS_READ_HOLDING:
    return "03保持寄存器";
  case MODBUS_READ_INPUT:
    return "04输入寄存器";
  default:
    return "未知";
  }
}

static bool auto_cache_range_covers(const slave_cache_t *cache,
                                    uint16_t start_addr, uint16_t quantity) {
  uint32_t req_start = start_addr;
  uint32_t req_end = req_start + quantity;
  uint32_t cache_start = cache->start_addr;
  uint32_t cache_end = cache_start + cache->quantity;
  return req_start >= cache_start && req_end <= cache_end;
}

static void auto_cache_mask_unused_bits(slave_cache_t *cache) {
  if (cache == NULL || cache->kind != AUTO_CACHE_PAYLOAD_BITS ||
      cache->payload == NULL || cache->payload_len == 0)
    return;

  uint8_t used_bits = cache->quantity % 8;
  if (used_bits == 0)
    return;

  cache->payload[cache->payload_len - 1] &= (uint8_t)((1U << used_bits) - 1U);
}

static bool auto_cache_get_bit(const uint8_t *payload, uint16_t bit_index) {
  return ((payload[bit_index / 8] >> (bit_index % 8)) & 0x01) != 0;
}

static void auto_cache_set_bit(uint8_t *payload, uint16_t bit_index,
                               bool value) {
  uint8_t mask = (uint8_t)(1U << (bit_index % 8));
  if (value) {
    payload[bit_index / 8] |= mask;
  } else {
    payload[bit_index / 8] &= (uint8_t)~mask;
  }
}

static void auto_cache_entry_free(slave_cache_t *cache) {
  if (cache == NULL)
    return;
  free(cache->payload);
  free(cache);
}

static void auto_cache_invalidate_written_range(uint8_t slave_id,
                                                uint8_t write_function,
                                                uint16_t start_addr,
                                                uint16_t quantity) {
  uint8_t read_function = auto_cache_write_target_function(write_function);
  int bank = auto_cache_bank_index(read_function);
  if (bank < 0 || quantity == 0)
    return;

  uint32_t write_start = start_addr;
  uint32_t write_end = write_start + quantity;
  int invalidated = 0;

  auto_cache_lock();
  slave_cache_t *current = g_auto_cache.banks[bank];
  while (current != NULL) {
    uint32_t cache_start = current->start_addr;
    uint32_t cache_end = cache_start + current->quantity;
    if (!current->delete_pending && current->slave_id == slave_id &&
        write_start < cache_end && write_end > cache_start) {
      current->data_valid = false;
      current->last_poll_time = 0;
      current->refresh_urgent = true;
      invalidated++;
    }
    current = current->next;
  }
  auto_cache_unlock();

  if (invalidated > 0) {
    ESP_LOGI(TAG, "写操作后失效自动缓存: slave=%u fc=0x%02X addr=%u qty=%u "
                  "entries=%d",
             slave_id, read_function, start_addr, quantity, invalidated);
    auto_cache_notify_poll_task();
  }
}

static void auto_cache_clear_all_locked(void) {
  for (int i = 0; i < MODBUS_AUTO_CACHE_BANK_COUNT; i++) {
    slave_cache_t *current = g_auto_cache.banks[i];
    while (current != NULL) {
      slave_cache_t *next = current->next;
      auto_cache_entry_free(current);
      current = next;
    }
    g_auto_cache.banks[i] = NULL;
  }
  auto_cache_entry_count = 0;
}

static void auto_cache_clear_all(void) {
  if (g_auto_cache.mutex != NULL) {
    xSemaphoreTake(g_auto_cache.mutex, portMAX_DELAY);
    auto_cache_clear_all_locked();
    xSemaphoreGive(g_auto_cache.mutex);
    vSemaphoreDelete(g_auto_cache.mutex);
    g_auto_cache.mutex = NULL;
  } else {
    auto_cache_clear_all_locked();
  }
}

// 前向声明
static void add_crc_to_frame(uint8_t *frame, int data_length);
static bool verify_crc(const uint8_t *frame, int total_length);
static void check_write_response(void);
static int get_ch5_timeout_override(void);
// 使用现有的calculate_crc函数
static bool parse_modbus_request(const uint8_t *data, int length,
                                 uint8_t *slave_id, uint8_t *function_code,
                                 uint16_t *start_addr, uint16_t *count);
static slave_cache_t *create_slave_cache(uint8_t function_code,
                                         uint8_t slave_id, uint16_t start_addr,
                                         uint16_t quantity,
                                         uint8_t source_channel);
static void remove_cache_entry(slave_cache_t *cache);
static void poll_slave_registers(slave_cache_t *cache);
static void handle_read_request(int source_channel, const uint8_t *data,
                                int length);
static void handle_write_request(int source_channel, const uint8_t *data,
                                 int length);
static void garbage_collection(void);

// 检查并处理写请求响应
static void check_write_response(void) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  uint8_t response[RESPONSE_BUFFER_SIZE];

  if (write_state.state == WRITE_STATE_IDLE)
    return;

  uint64_t current_time = get_current_time_ms();

  // 检查超时
  if (current_time - write_state.start_time > write_state.timeout_ms) {
    ESP_LOGW(TAG, "⏰ 从机%d写请求超时（%dms），状态:%d", write_state.slave_id,
             write_state.timeout_ms, write_state.state);

    // 构造超时异常响应
    uint8_t timeout_response[5];
    timeout_response[0] = write_state.slave_id;
    timeout_response[1] = write_state.function_code | 0x80; // 设置异常位
    timeout_response[2] = 0x0B; // 网关目标设备无响应异常码
    add_crc_to_frame(timeout_response, 3);
    tx_tasks_to_channel(timeout_response, 5, write_state.source_channel);
    ESP_LOGI(TAG, "📤 超时异常响应已发送到CH%d", write_state.source_channel);

    // 清除写状态
    write_state.state = WRITE_STATE_IDLE;
    poll_suspended = false;
    ESP_LOGI(TAG, "▶️ 恢复轮询");
    return;
  }

  // 检查是否有从机响应
  uint64_t timestamp;
  int response_len =
      pop_channel_data(3, response, sizeof(response), &timestamp);

  if (response_len > 0) {
    ESP_LOGI(TAG, "📥 CH3收到数据，长度%d，从机ID=%d，功能码=0x%02X",
             response_len, response[0], response_len > 1 ? response[1] : 0);
    ESP_LOG_BUFFER_HEXDUMP(TAG, response, response_len, ESP_LOG_INFO);

    if (response_len < 2) {
      ESP_LOGW(TAG, "清理过短写响应: len=%d", response_len);
      return;
    }

    if (write_state.state == WRITE_STATE_WAITING_RESPONSE) {
      // 处理写响应
      if (response[0] == write_state.slave_id &&
          (response[1] == write_state.function_code ||
           response[1] == (write_state.function_code | 0x80))) {

        ESP_LOGI(TAG, "✅ 收到目标从机%d写响应，长度%d", write_state.slave_id,
                 response_len);

        // 计算实际帧长度（写单寄存器响应固定8字节，异常响应5字节）
        int actual_frame_len;
        if (response[1] & 0x80) {
          actual_frame_len = 5; // 异常响应
        } else if (response[1] == MODBUS_WRITE_SINGLE_COIL ||
                   response[1] == MODBUS_WRITE_SINGLE_REG ||
                   response[1] == MODBUS_WRITE_MULTIPLE_COILS ||
                   response[1] == MODBUS_WRITE_MULTIPLE_REGS) {
          actual_frame_len = MIN_MODBUS_FRAME_LEN; // 写响应固定8字节
        } else {
          actual_frame_len = response_len; // 其他情况使用接收长度
        }

        // 只验证实际帧的CRC，避免粘包问题
        if (actual_frame_len <= response_len &&
            verify_crc(response, actual_frame_len)) {
          // 如果有粘包，记录日志
          if (response_len > actual_frame_len) {
            ESP_LOGW(TAG, "检测到粘包：接收%d字节，实际帧%d字节，多余%d字节",
                     response_len, actual_frame_len,
                     response_len - actual_frame_len);
          }

          // 检查是否是异常响应
          if (response[1] & 0x80) {
            ESP_LOGW(TAG, "⚠️ 从机%d写操作返回异常码: 0x%02X",
                     write_state.slave_id, response[2]);
            // 转发异常响应给主站（只转发实际帧）
            tx_tasks_to_channel(response, actual_frame_len,
                                write_state.source_channel);
            ESP_LOGI(TAG, "📤 异常响应已转发到CH%d",
                     write_state.source_channel);
            // 清除写状态
            write_state.state = WRITE_STATE_IDLE;
            poll_suspended = false;
            ESP_LOGI(TAG, "▶️ 恢复轮询");
          } else {
            ESP_LOGI(TAG, "✅ 写操作成功，转发写响应到CH%d",
                     write_state.source_channel);
            auto_cache_invalidate_written_range(
                write_state.slave_id, write_state.function_code,
                write_state.start_addr, write_state.register_count);
            // 转发写成功响应给主站（只转发实际帧）
            tx_tasks_to_channel(response, actual_frame_len,
                                write_state.source_channel);

            // 清除写状态，恢复正常轮询
            write_state.state = WRITE_STATE_IDLE;
            poll_suspended = false;
            ESP_LOGI(TAG, "▶️ 写操作完成，恢复正常轮询");
          }
        } else {
          ESP_LOGW(TAG, "从机%d写响应CRC校验失败", write_state.slave_id);

          // 构造CRC错误异常响应
          uint8_t error_response[5];
          error_response[0] = write_state.slave_id;
          error_response[1] = write_state.function_code | 0x80; // 设置异常位
          error_response[2] = 0x04; // 设备故障异常码
          add_crc_to_frame(error_response, 3);
          tx_tasks_to_channel(error_response, 5, write_state.source_channel);

          // 清除写状态
          write_state.state = WRITE_STATE_IDLE;
          poll_suspended = false;
        }
      } else {
        // 不是目标响应，清除数据继续等待
        ESP_LOGW(TAG,
                 "⚠️ "
                 "收到非目标响应（期望从机%d功能码0x%02X，实际从机%d功能码0x%"
                 "02X），清除并继续等待",
                 write_state.slave_id, write_state.function_code, response[0],
                 response[1]);
      }
    } else {
      // 状态不是等待写响应，清除数据
      ESP_LOGW(TAG, "⚠️ 当前不在等待写响应状态（状态=%d），清除CH3数据",
               write_state.state);
    }
  }
}

// 注意：使用统一的calculate_crc函数，避免CRC计算不一致

// 添加CRC到Modbus帧
static void add_crc_to_frame(uint8_t *frame, int data_length) {
  uint16_t crc = calculate_crc(frame, data_length);
  frame[data_length] = crc & 0xFF;            // CRC低字节
  frame[data_length + 1] = (crc >> 8) & 0xFF; // CRC高字节
}

// 验证Modbus帧CRC
static bool verify_crc(const uint8_t *frame, int total_length) {
  if (total_length < 3)
    return false;

  int data_length = total_length - 2;
  uint16_t calculated_crc = calculate_crc((uint8_t *)frame, data_length);
  uint16_t received_crc = frame[data_length] | (frame[data_length + 1] << 8);

  return calculated_crc == received_crc;
}

// 解析Modbus请求帧
static bool parse_modbus_request(const uint8_t *data, int length,
                                 uint8_t *slave_id, uint8_t *function_code,
                                 uint16_t *start_addr, uint16_t *count) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  if (length < 8)
    return false; // 最小Modbus帧长度(包含CRC)

  // 验证CRC
  if (!verify_crc(data, length)) {
    ESP_LOGW(TAG, "Modbus帧CRC校验失败");
    return false;
  }

  *slave_id = data[0];
  *function_code = data[1];

  // 解析起始地址和数量（大端序）
  *start_addr = (data[2] << 8) | data[3];
  *count = (data[4] << 8) | data[5];

  return true;
}

// 查找从机缓存（精确匹配指定地址范围）
static slave_cache_t *find_slave_cache_for_range(uint8_t function_code,
                                                 uint8_t slave_id,
                                                 uint16_t start_addr,
                                                 uint16_t quantity) {
  int bank = auto_cache_bank_index(function_code);
  if (bank < 0)
    return NULL;

  slave_cache_t *current = g_auto_cache.banks[bank];

  while (current != NULL) {
    if (!current->delete_pending && current->slave_id == slave_id &&
        current->function_code == function_code) {
      // 检查当前缓存是否完全覆盖请求的地址范围
      if (auto_cache_range_covers(current, start_addr, quantity)) {
        return current;
      }
    }
    current = current->next;
  }

  // 未找到完全覆盖的缓存，返回NULL（将创建新的独立缓存）
  return NULL;
}

// 创建新的从机缓存
static slave_cache_t *create_slave_cache(uint8_t function_code,
                                         uint8_t slave_id, uint16_t start_addr,
                                         uint16_t quantity,
                                         uint8_t source_channel) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  int bank = auto_cache_bank_index(function_code);
  if (bank < 0) {
    ESP_LOGW(TAG, "创建缓存失败：不支持的功能码0x%02X", function_code);
    return NULL;
  }

  // 检查是否超过Modbus RTU协议限制
  if (!auto_cache_valid_range(function_code, start_addr, quantity)) {
    ESP_LOGW(TAG,
             "创建从机%d %s缓存失败：请求范围无效，地址=%u 数量=%u 上限=%u",
             slave_id, auto_cache_function_name(function_code), start_addr,
             quantity, auto_cache_max_quantity(function_code));
    return NULL; // 拒绝创建
  }

  slave_cache_t *cache = malloc(sizeof(slave_cache_t));
  if (cache == NULL) {
    ESP_LOGE(TAG, "创建从机%d缓存失败：内存不足", slave_id);
    return NULL;
  }

  size_t payload_len = auto_cache_payload_len(function_code, quantity);
  cache->payload = calloc(payload_len, sizeof(uint8_t));
  if (cache->payload == NULL) {
    ESP_LOGE(TAG, "为从机%d %s分配缓存失败，长度=%zu", slave_id,
             auto_cache_function_name(function_code), payload_len);
    free(cache);
    return NULL;
  }

  cache->slave_id = slave_id;
  cache->function_code = function_code;
  cache->kind = auto_cache_is_bit_read(function_code)
                    ? AUTO_CACHE_PAYLOAD_BITS
                    : AUTO_CACHE_PAYLOAD_REG16;
  cache->start_addr = start_addr;
  cache->quantity = quantity;
  cache->payload_len = payload_len;
  cache->last_access_time = get_current_time_ms();
  cache->last_poll_time = 0;
  cache->source_channel = source_channel; // 记录来源通道
  cache->data_valid = false;              // 初始化为无效，等待轮询更新
  cache->delete_pending = false;
  cache->is_polling = false;
  cache->refresh_urgent = true;
  cache->access_hits_window = 0;
  cache->access_heat = 0;
  cache->last_hit_time = cache->last_access_time;
  cache->last_poll_start_time = 0;
  cache->avg_poll_cost_ms = 0;
  cache->est_wire_cost_ms = AUTO_CACHE_BASE_MIN_INTERVAL_MS;
  cache->min_refresh_interval_ms = auto_cache_min_refresh_interval_ms(cache);
  cache->target_interval_ms = auto_cache_target_interval_ms(cache);
  cache->next_eligible_time = 0;
  cache->needs_verification = false;
  cache->verify_addr = 0;
  cache->verify_value = 0;
  cache->poll_fail_count = 0; // 初始化失败计数器

  // 插入对应功能码的独立缓存表
  cache->next = g_auto_cache.banks[bank];
  g_auto_cache.banks[bank] = cache;
  if (auto_cache_entry_count < UINT32_MAX) {
    auto_cache_entry_count++;
  }

  ESP_LOGI(TAG,
           "创建从机%d %s缓存：地址%d--%d，数量%d，payload=%zu字节，来源CH%d",
           slave_id, auto_cache_function_name(function_code), start_addr,
           start_addr + quantity - 1, quantity, payload_len, source_channel);
  return cache;
}

// 从缓存链表中移除并释放指定缓存条目
static void remove_cache_entry(slave_cache_t *cache) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";

  if (cache == NULL)
    return;

  int bank = auto_cache_bank_index(cache->function_code);
  if (bank < 0)
    return;

  slave_cache_t *current = g_auto_cache.banks[bank];
  slave_cache_t *prev = NULL;

  // 在链表中查找并移除
  while (current != NULL) {
    if (current == cache) {
      ESP_LOGI(TAG, "移除从机%d %s缓存：地址%d--%d (连续轮询失败)",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               cache->start_addr, cache->start_addr + cache->quantity - 1);

      if (prev == NULL) {
        g_auto_cache.banks[bank] = current->next;
      } else {
        prev->next = current->next;
      }

      auto_cache_entry_free(current);
      if (auto_cache_entry_count > 0) {
        auto_cache_entry_count--;
      }
      return;
    }
    prev = current;
    current = current->next;
  }
}

// 轮询从机寄存器
static void poll_slave_registers(slave_cache_t *cache) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  uint8_t response[1024];
  uint64_t poll_start_time = get_current_time_ms();

  if (cache == NULL || cache->payload == NULL)
    return;

  // 🔴 关键修复：发送前检查是否有写命令需要暂停轮询
  if (poll_suspended) {
    ESP_LOGW(TAG, "⏸️ 总线事务暂停中，跳过轮询从机%d %s",
             cache->slave_id, auto_cache_function_name(cache->function_code));
    return; // 立即返回，不发送轮询请求
  }

  uint32_t estimated_cost = auto_cache_estimate_wire_cost_ms(cache);
  auto_cache_lock();
  cache->last_poll_start_time = poll_start_time;
  cache->est_wire_cost_ms = estimated_cost;
  auto_cache_unlock();

  // 构造Modbus读取命令（功能码按缓存entry独立处理）
  uint8_t cmd[8];
  cmd[0] = cache->slave_id;
  cmd[1] = cache->function_code;
  cmd[2] = (cache->start_addr >> 8) & 0xFF;
  cmd[3] = cache->start_addr & 0xFF;
  cmd[4] = (cache->quantity >> 8) & 0xFF;
  cmd[5] = cache->quantity & 0xFF;

  // 添加正确的CRC16校验
  add_crc_to_frame(cmd, 6);

  ESP_LOGD(TAG, "轮询从机%d %s：地址%d，数量%d", cache->slave_id,
           auto_cache_function_name(cache->function_code), cache->start_addr,
           cache->quantity);

  // 使用智能发送函数，自动适应工作模式和通道参数，使用缓存记录的来源通道
  uart_config_mode_t current_mode = get_uart_config_mode();
  esp_err_t send_ret =
      smart_send_data_to_ch3(current_mode, cache->source_channel, cmd, 8);
  if (send_ret != ESP_OK) {
    ESP_LOGE(TAG, "智能发送轮询请求失败: %s (从机%d %s, 来源通道CH%d)",
             esp_err_to_name(send_ret), cache->slave_id,
             auto_cache_function_name(cache->function_code),
             cache->source_channel);
    auto_cache_record_poll_cost(cache,
                                (uint32_t)(get_current_time_ms() -
                                           poll_start_time));
    return; // 发送失败就不等待响应了
  }
  // 等待响应并更新缓存 - 改为循环等待确保收到正确响应
  uint64_t timestamp;
  int response_len = 0;
  uint64_t start_time = get_current_time_ms();

  // 关键修复：根据实际工作模式使用正确的"回复超时时间"
  uint32_t actual_timeout;

  if (current_mode == UART_CONFIG_MODE_SLAVE_FOLLOW) {
    // 从机跟随模式：使用CH1/CH2的较大超时时间
    actual_timeout = (timeout1 > timeout2) ? timeout1 : timeout2;
    ESP_LOGI(TAG, "从机跟随模式，轮询从机%d %s使用CH1/CH2最大超时时间: %u ms",
             cache->slave_id, auto_cache_function_name(cache->function_code),
             actual_timeout);
  } else {
    // 正常模式：使用CH3超时时间
    actual_timeout =
        timeout3 > 0 ? timeout3 : AUTO_CACHE_POLL_RESPONSE_TIMEOUT_MS;
    if (timeout3 > 0) {
      ESP_LOGI(TAG, "正常模式，轮询从机%d %s使用CH3超时时间: %u ms",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               actual_timeout);
    } else {
      ESP_LOGI(TAG, "正常模式，轮询从机%d %s使用默认超时时间: %u ms",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               actual_timeout);
    }
  }

  // 循环等待目标从机的响应，使用实际配置的超时时间
  while (get_current_time_ms() - start_time < actual_timeout) {
    // 🔴 关键修复：检查写命令暂停标志，立即退出轮询
    if (poll_suspended) {
      ESP_LOGW(TAG, "⏸️ 检测到总线事务暂停轮询，立即退出轮询从机%d %s",
               cache->slave_id, auto_cache_function_name(cache->function_code));
      auto_cache_record_poll_cost(cache,
                                  (uint32_t)(get_current_time_ms() -
                                             poll_start_time));
      return; // 立即退出，让出CH3通道给写命令
    }

    response_len = pop_channel_data(3, response, sizeof(response), &timestamp);
    if (response_len > 4 && response[0] == cache->slave_id &&
        response[1] == cache->function_code) {
      ESP_LOGD(TAG, "收到从机%d %s轮询响应，长度%d", cache->slave_id,
               auto_cache_function_name(cache->function_code), response_len);
      break; // 找到目标响应
    } else if (response_len >= 5 && response[0] == cache->slave_id &&
	               (response[1] & 0x80)) {
      // 检测到异常响应
      uint8_t exception_code = response[2];
      ESP_LOGW(TAG,
               "轮询从机%d %s地址%d收到异常响应: 功能码0x%02X, 异常码0x%02X",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               cache->start_addr, response[1], exception_code);
      // 增加失败计数
      auto_cache_lock();
      cache->poll_fail_count++;
      ESP_LOGW(TAG, "从机%d %s地址%d轮询失败次数: %d", cache->slave_id,
               auto_cache_function_name(cache->function_code), cache->start_addr,
               cache->poll_fail_count);

      // 连续失败3次后，立即删除该缓存条目
      if (cache->poll_fail_count >= 3) {
        ESP_LOGW(TAG, "从机%d %s地址%d连续失败3次，立即删除缓存条目",
                 cache->slave_id, auto_cache_function_name(cache->function_code),
                 cache->start_addr);
        cache->delete_pending = true;
      }
      auto_cache_unlock();
      auto_cache_record_poll_cost(cache,
                                  (uint32_t)(get_current_time_ms() -
                                             poll_start_time));
      return; // 退出轮询
    } else if (response_len > 0) {
      ESP_LOGD(TAG, "清除非目标响应：从机%d，功能码0x%02X", response[0],
               response[1]);
    }
    vTaskDelay(sx_ms_to_ticks(5)); // 短暂等待
  }

  if (response_len > 4 && response[0] == cache->slave_id &&
      response[1] == cache->function_code) {
    size_t data_len = response[2];
    size_t expected_len = cache->payload_len;
    size_t expected_frame_len = 3 + expected_len + 2;

    if (data_len == expected_len && (size_t)response_len >= expected_frame_len &&
        verify_crc(response, (int)expected_frame_len)) {
      if ((size_t)response_len > expected_frame_len) {
        ESP_LOGW(TAG, "检测到粘包：接收%d字节，实际帧%zu字节，多余%zu字节",
                 response_len, expected_frame_len,
                 (size_t)response_len - expected_frame_len);
      }

      auto_cache_lock();
      memcpy(cache->payload, &response[3], expected_len);
      auto_cache_mask_unused_bits(cache);
      cache->last_poll_time = get_current_time_ms(); // 更新轮询时间
      cache->data_valid = true;                      // 标记数据有效
      cache->refresh_urgent = false;                 // 已完成本轮紧急刷新
      cache->poll_fail_count = 0;                    // 成功后重置失败计数
      auto_cache_unlock();
      ESP_LOGI(TAG,
               "✅ 更新从机%d %s缓存成功，地址%d，数量%d，payload=%zu字节，数据现已有效",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               cache->start_addr, cache->quantity, expected_len);
    } else {
      if (data_len != expected_len || (size_t)response_len < expected_frame_len) {
        ESP_LOGW(TAG,
                 "从机%d %s地址%d响应数据长度异常：期望payload=%zu/帧=%zu，实际payload=%zu/帧=%d",
                 cache->slave_id, auto_cache_function_name(cache->function_code),
                 cache->start_addr, expected_len, expected_frame_len, data_len,
                 response_len);
      } else {
        uint16_t calculated_crc =
            calculate_crc(response, expected_frame_len - 2);
        uint16_t received_crc =
            response[expected_frame_len - 2] | (response[expected_frame_len - 1] << 8);
        ESP_LOGW(TAG,
                 "从机%d %s地址%d响应CRC校验失败 - 帧长:%zu, 计算CRC:0x%04X, 接收CRC:0x%04X",
                 cache->slave_id, auto_cache_function_name(cache->function_code),
                 cache->start_addr, expected_frame_len, calculated_crc,
                 received_crc);
        ESP_LOG_BUFFER_HEXDUMP(TAG, response, response_len, ESP_LOG_WARN);
      }

      auto_cache_lock();
      cache->poll_fail_count++;
      ESP_LOGW(TAG, "从机%d %s地址%d轮询失败次数: %d",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               cache->start_addr, cache->poll_fail_count);

      if (cache->poll_fail_count >= 3) {
        ESP_LOGW(TAG, "从机%d %s地址%d连续失败3次，立即删除缓存条目",
                 cache->slave_id, auto_cache_function_name(cache->function_code),
                 cache->start_addr);
        cache->delete_pending = true;
        auto_cache_unlock();
        auto_cache_record_poll_cost(cache,
                                    (uint32_t)(get_current_time_ms() -
                                               poll_start_time));
        return;
      }
      auto_cache_unlock();
    }
  } else {
    // 关键修复：超时无响应也应该增加失败计数
    ESP_LOGW(TAG, "从机%d %s地址%d轮询超时或无响应", cache->slave_id,
             auto_cache_function_name(cache->function_code), cache->start_addr);

    // 增加失败计数
    auto_cache_lock();
    cache->poll_fail_count++;
    ESP_LOGW(TAG, "从机%d %s地址%d轮询失败次数: %d", cache->slave_id,
             auto_cache_function_name(cache->function_code), cache->start_addr,
             cache->poll_fail_count);

    // 连续失败3次后，立即删除该缓存条目
    if (cache->poll_fail_count >= 3) {
      ESP_LOGW(TAG, "从机%d %s地址%d连续超时3次，立即删除缓存条目",
               cache->slave_id, auto_cache_function_name(cache->function_code),
               cache->start_addr);
      cache->delete_pending = true;
    }
    auto_cache_unlock();
  }

  auto_cache_record_poll_cost(cache,
                              (uint32_t)(get_current_time_ms() -
                                         poll_start_time));
}

static bool auto_cache_build_read_response(const slave_cache_t *cache,
                                           uint16_t start_addr,
                                           uint16_t quantity,
                                           uint8_t *response,
                                           int *response_len) {
  if (cache == NULL || response == NULL || response_len == NULL ||
      cache->payload == NULL || !auto_cache_range_covers(cache, start_addr, quantity))
    return false;

  size_t payload_len = auto_cache_payload_len(cache->function_code, quantity);
  if (payload_len == 0 || 3 + payload_len + 2 > RESPONSE_BUFFER_SIZE)
    return false;

  response[0] = cache->slave_id;
  response[1] = cache->function_code;
  response[2] = (uint8_t)payload_len;

  uint16_t offset = start_addr - cache->start_addr;
  if (cache->kind == AUTO_CACHE_PAYLOAD_BITS) {
    memset(&response[3], 0, payload_len);
    for (uint16_t i = 0; i < quantity; i++) {
      bool bit = auto_cache_get_bit(cache->payload, offset + i);
      auto_cache_set_bit(&response[3], i, bit);
    }
  } else {
    size_t byte_offset = (size_t)offset * 2U;
    memcpy(&response[3], cache->payload + byte_offset, payload_len);
  }

  int data_length = 3 + (int)payload_len;
  add_crc_to_frame(response, data_length);
  *response_len = data_length + 2;
  return true;
}

// 处理读请求 - 缓存未命中时创建条目但不回复主机
static void handle_read_request(int source_channel, const uint8_t *data,
                                int length) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  uint8_t response[1024];

  uint8_t slave_id, function_code;
  uint16_t start_addr, count;

  if (!parse_modbus_request(data, length, &slave_id, &function_code,
                            &start_addr, &count)) {
    ESP_LOGW(TAG, "解析CH%d读请求失败", source_channel);
    return;
	  }

  if (!auto_cache_valid_range(function_code, start_addr, count)) {
    ESP_LOGW(TAG, "CH%d读请求数量无效或功能码不支持：从机%d 功能码0x%02X 地址%d 数量%d",
             source_channel, slave_id, function_code, start_addr, count);
    return;
  }

  ESP_LOGI(TAG, "CH%d读请求：从机%d %s，地址%d，数量%d", source_channel,
           slave_id, auto_cache_function_name(function_code), start_addr, count);

  uint64_t current_time = get_current_time_ms();
  auto_cache_refresh_scheduler_stats(current_time);

  // 查找缓存（优先查找覆盖请求范围的缓存条目）
  auto_cache_lock();
  slave_cache_t *cache =
      find_slave_cache_for_range(function_code, slave_id, start_addr, count);

  // 判定是否需要创建缓存（缓存缺失 / 范围不覆盖 / 数据无效或过旧）
  bool need_create_cache = false;
  bool can_reply = false;

  if (cache == NULL) {
    need_create_cache = true;
  } else {
    bool not_covered = !auto_cache_range_covers(cache, start_addr, count);
    if (not_covered) {
      need_create_cache = true;
    } else {
      // 检查数据是否有效
      if (!cache->data_valid) {
        // 数据尚未从从机获取，不回复，等待轮询任务更新
        ESP_LOGI(TAG, "缓存数据尚未从从机获取，丢弃主机请求，等待轮询更新");
        auto_cache_mark_access(cache, current_time, true);
        auto_cache_unlock();
        auto_cache_notify_poll_task();
        return;
      }

      // 数据新鲜度检查
      if (current_time - cache->last_poll_time >
          AUTO_CACHE_DATA_FRESH_TIME_MS) {
        // 数据过旧，不回复，等待轮询任务更新
        ESP_LOGI(TAG,
                 "缓存数据过旧（%llu ms前更新），丢弃主机请求，等待轮询更新",
                 current_time - cache->last_poll_time);
        auto_cache_mark_access(cache, current_time, true);
        auto_cache_unlock();
        auto_cache_notify_poll_task();
        return;
      } else {
        can_reply = true;
      }
    }
  }

  if (need_create_cache) {
    // 缓存未命中或范围不覆盖：直接创建缓存条目但不回复主机
    ESP_LOGI(TAG, "缓存未命中，创建%s缓存条目但不回复主机（从机%d，地址%d+%d）",
             auto_cache_function_name(function_code), slave_id, start_addr,
             count);

    // 创建新的独立缓存条目
    cache = create_slave_cache(function_code, slave_id, start_addr, count,
                               source_channel);
    if (cache == NULL) {
      ESP_LOGE(TAG, "创建%s缓存失败（从机%d，地址%d+%d）",
               auto_cache_function_name(function_code), slave_id, start_addr,
               count);
      auto_cache_unlock();
      return;
    }
    auto_cache_mark_access(cache, current_time, true);

    ESP_LOGI(
        TAG,
        "✅ 已创建%s缓存条目：从机%d，地址%d--%d，数量%d，等待轮询任务更新数据",
        auto_cache_function_name(function_code), slave_id, start_addr,
        start_addr + count - 1, count);

    // 丢弃主机请求，不回复
    // 当轮询任务从从机获取到数据后，主机再次请求时才会回复
    auto_cache_unlock();
    auto_cache_notify_poll_task();
    return;
  }

  // 缓存命中且数据有效新鲜：直接从缓存返回
  if (can_reply) {
    int response_len = 0;
    if (!auto_cache_build_read_response(cache, start_addr, count, response,
                                        &response_len)) {
      ESP_LOGE(TAG, "构建从机%d %s缓存响应失败：地址%d，数量%d", slave_id,
               auto_cache_function_name(function_code), start_addr, count);
      auto_cache_unlock();
      return;
    }

    // 更新访问时间
    auto_cache_mark_access(cache, current_time, false);
    auto_cache_unlock();

    tx_tasks_to_channel(response, response_len, source_channel);
    ESP_LOGI(TAG, "📤 向CH%d返回从机%d %s缓存数据（有效且新鲜）",
             source_channel, slave_id, auto_cache_function_name(function_code));
  } else {
    auto_cache_unlock();
  }
}

// 处理写请求
static void handle_write_request(int source_channel, const uint8_t *data,
                                 int length) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  uint8_t slave_id, function_code;
  uint16_t start_addr, count;

  if (!parse_modbus_request(data, length, &slave_id, &function_code,
                            &start_addr, &count)) {
    ESP_LOGW(TAG, "解析CH%d写请求失败", source_channel);
    return;
  }

  // 检查是否有其他写请求正在进行
  if (write_state.state != WRITE_STATE_IDLE) {
    ESP_LOGW(TAG, "写请求冲突：从机%d正在处理写请求，拒绝新的写请求",
             write_state.slave_id);

    // 构造忙碌异常响应
    uint8_t busy_response[5];
    busy_response[0] = slave_id;
    busy_response[1] = function_code | 0x80; // 设置异常位
    busy_response[2] = 0x06;                 // 设备忙异常码
    add_crc_to_frame(busy_response, 3);
    tx_tasks_to_channel(busy_response, 5, source_channel);
    return;
  }

  ESP_LOGI(TAG, "CH%d写请求：从机%d，地址%d，数量%d", source_channel, slave_id,
           start_addr, count);

  uint16_t write_count =
      (function_code == MODBUS_WRITE_SINGLE_COIL ||
       function_code == MODBUS_WRITE_SINGLE_REG)
          ? 1
          : count;
  if (write_count == 0 ||
      !auto_cache_valid_u16_range(start_addr, write_count)) {
    ESP_LOGW(TAG, "CH%d写请求范围无效：从机%d 功能码0x%02X 地址%d 数量%d",
             source_channel, slave_id, function_code, start_addr, write_count);
    uint8_t error_response[5];
    error_response[0] = slave_id;
    error_response[1] = function_code | 0x80;
    error_response[2] = 0x03;
    add_crc_to_frame(error_response, 3);
    tx_tasks_to_channel(error_response, 5, source_channel);
    return;
  }

  // 设置写请求状态
  write_state.state = WRITE_STATE_WAITING_RESPONSE;
  write_state.slave_id = slave_id;
  write_state.function_code = function_code;
  write_state.source_channel = source_channel;
  write_state.start_time = get_current_time_ms();
  write_state.timeout_ms = (source_channel == 1) ? timeout1 : timeout2;
  write_state.start_addr = start_addr;
  write_state.register_count = write_count;

  // 提取写入值（单寄存器写入时）
  if (function_code == MODBUS_WRITE_SINGLE_REG && length >= 8) {
    write_state.write_value = (data[4] << 8) | data[5];
  }

  // 立即停止轮询以避免冲突
  poll_suspended = true;
  ESP_LOGI(TAG, "⏸️ 暂停后台轮询，准备发送写命令");

  // 等待正在进行的轮询检查到暂停标志并释放CH3总线。
  // 调度任务空闲等待为5~20ms，poll_slave_registers内部也每5ms检查暂停标志。
  vTaskDelay(pdMS_TO_TICKS(WRITE_PRE_SEND_DELAY_MS)); // 等待轮询停止/总线空闲

  // 清除CH3通道可能残留的数据
  clear_channel_data(3);
  ESP_LOGI(TAG, "✅ CH3通道已清空，轮询任务已暂停");

  // 使用智能发送函数转发写请求到CH3（不等待响应，异步处理）
  uart_config_mode_t current_mode = get_uart_config_mode();
  esp_err_t send_ret = smart_send_data_to_ch3(current_mode, source_channel,
                                              data, (size_t)length);
  if (send_ret != ESP_OK) {
    ESP_LOGE(TAG, "❌ 智能发送写请求失败: %s (从机%d, 来源通道CH%d)",
             esp_err_to_name(send_ret), slave_id, source_channel);
    // 发送失败，清除写状态并恢复轮询
    write_state.state = WRITE_STATE_IDLE;
    poll_suspended = false;
    return;
  }

  ESP_LOGI(TAG, "📤 写请求已发送到CH3，等待从机%d异步响应（超时%dms）",
           slave_id, write_state.timeout_ms);
}

// 垃圾回收：清理过期缓存
static void garbage_collection(void) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  uint64_t current_time = get_current_time_ms();

  // 每配置间隔执行一次垃圾回收
  if (current_time - last_gc_time < AUTO_CACHE_GC_INTERVAL_MS)
    return;

  int freed_count = 0;
  auto_cache_lock();

  for (int bank = 0; bank < MODBUS_AUTO_CACHE_BANK_COUNT; bank++) {
    slave_cache_t *current = g_auto_cache.banks[bank];
    slave_cache_t *prev = NULL;

    while (current != NULL) {
      if (current_time - current->last_access_time > AUTO_CACHE_TIMEOUT_MS) {
        ESP_LOGI(TAG, "回收从机%d %s缓存：超时未访问", current->slave_id,
                 auto_cache_function_name(current->function_code));

        if (current->is_polling) {
          current->delete_pending = true;
          prev = current;
          current = current->next;
          continue;
        }

        if (prev == NULL) {
          g_auto_cache.banks[bank] = current->next;
        } else {
          prev->next = current->next;
        }

        slave_cache_t *to_free = current;
        current = current->next;
        auto_cache_entry_free(to_free);
        if (auto_cache_entry_count > 0) {
          auto_cache_entry_count--;
        }
        freed_count++;
      } else {
        prev = current;
        current = current->next;
      }
    }
  }
  auto_cache_unlock();

  if (freed_count > 0) {
    ESP_LOGI(TAG, "垃圾回收完成：释放%d个缓存", freed_count);
  }

  last_gc_time = current_time;
}

// 自动智能缓存模式初始化函数
esp_err_t init_auto_intelligent_cache(void) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE";
  ESP_LOGI(TAG, "初始化透明智能缓存系统");

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法打开NVS存储: %s", esp_err_to_name(err));
    return err;
  }

  // 获取timout值的内存大小
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

  // 读出值并释放内存
  timeout1 = timeouts_values[0] ? atoi(timeouts_values[0]) : 1000;
  timeout2 = timeouts_values[1] ? atoi(timeouts_values[1]) : 1000;
  timeout3 = timeouts_values[2] ? atoi(timeouts_values[2]) : 1000;

  for (int i = 0; i < 3; i++) {
    if (timeouts_values[i]) {
      free(timeouts_values[i]);
    }
  }

  nvs_close(nvs_handle);

  ESP_LOGI(TAG, "超时配置: CH1=%dms, CH2=%dms, CH3平均=%dms", timeout1,
           timeout2, timeout3);

  // 初始化缓存系统：01/02/03/04四张独立缓存表
  auto_cache_clear_all();
  g_auto_cache.mutex = xSemaphoreCreateMutex();
  if (g_auto_cache.mutex == NULL) {
    ESP_LOGE(TAG, "创建自动缓存互斥锁失败");
    return ESP_ERR_NO_MEM;
  }
  last_gc_time = get_current_time_ms();

  ESP_LOGI(TAG,
           "缓存配置: 动态调度=%u/%u/%ums, 缓存超时=%ums，缓存表=01/02/03/04独立",
           AUTO_CACHE_POLL_DELAY_FAST_MS, AUTO_CACHE_POLL_DELAY_NORMAL_MS,
           AUTO_CACHE_POLL_DELAY_IDLE_MS, AUTO_CACHE_TIMEOUT_MS);

  auto_cache_tasks_stopping = false;
  auto_cache_task_exited = false;

  // 创建主请求处理任务 - 固定到工作核心，使用低优先级
  BaseType_t ret = create_app_task_psram(
      auto_intelligent_cache_task, "autoCacheTask", 20480, NULL, 2,
      &auto_cache_task_handle, SX_WORK_CORE_ID);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "创建主请求处理任务失败");
    auto_cache_clear_all();
    auto_cache_task_exited = true;
    return ESP_FAIL;
  }

  // 创建独立的从机轮询任务 - 固定到工作核心，使用低优先级
  auto_poll_task_exited = false;
  ret = create_app_task_psram(auto_cache_poll_task, "autoPollTask", 16384,
                              NULL, 2, &auto_poll_task_handle, SX_WORK_CORE_ID);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "创建轮询任务失败");
    auto_cache_tasks_stopping = true;
    if (!auto_cache_wait_task_exit(&auto_cache_task_exited, 1500)) {
      delete_app_task_with_caps(auto_cache_task_handle);
      auto_cache_task_exited = true;
    }
    auto_cache_task_handle = NULL;
    auto_cache_clear_all();
    auto_cache_tasks_stopping = false;
    auto_poll_task_exited = true;
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "透明智能缓存系统启动成功（主任务+轮询任务）");
  return ESP_OK;
}

// 独立的从机轮询任务
void auto_cache_poll_task(void *pvParameter) {
  static const char *TAG = "AUTO_CACHE_POLL";

  ESP_LOGI(TAG, "自动缓存轮询任务启动");
  static uint64_t last_status_time = 0;

  auto_poll_task_exited = false;
  while (!auto_cache_tasks_stopping) {
    uint64_t current_time = get_current_time_ms();
    auto_cache_refresh_scheduler_stats(current_time);

    // 写操作期间暂停轮询
    if (poll_suspended) {
      ulTaskNotifyTake(pdTRUE, sx_ms_to_ticks(AUTO_CACHE_POLL_DELAY_IDLE_MS));
      continue;
    }

    bool has_urgent = false;
    slave_cache_t *candidate =
        auto_cache_select_poll_candidate(current_time, &has_urgent);
    if (candidate != NULL) {
      // 有候选就立即轮询，轮询结束后继续下一轮选择，不做固定周期等待。
      // 只有没有候选时才按CH3利用率短暂睡眠，因此缓存条目充足时会尽量占满CH3。
      ESP_LOGD(TAG,
               "调度轮询从机%d %s 地址%d+%d heat=%u ratio=%u cost=%ums "
               "target=%ums util=%u urgent=%d valid=%d",
               candidate->slave_id,
               auto_cache_function_name(candidate->function_code),
               candidate->start_addr, candidate->quantity,
               candidate->access_heat,
               auto_cache_access_ratio_permille(candidate),
               auto_cache_refresh_cost_ms(candidate),
               candidate->target_interval_ms, ch3_utilization_permille,
               candidate->refresh_urgent, candidate->data_valid);
      poll_slave_registers(candidate);
      auto_cache_finish_poll_candidate(candidate);
      taskYIELD();
      continue;
    }

    // 垃圾回收
    garbage_collection();

    // 打印缓存状态（每30秒一次）
    if (current_time - last_status_time > AUTO_LOG_STATUS_INTERVAL_MS) {
      int cache_count = 0;
      int bank_counts[MODBUS_AUTO_CACHE_BANK_COUNT] = {0};
      auto_cache_lock();
      for (int bank = 0; bank < MODBUS_AUTO_CACHE_BANK_COUNT; bank++) {
        slave_cache_t *current_cache = g_auto_cache.banks[bank];
        while (current_cache != NULL) {
          bank_counts[bank]++;
          cache_count++;
          current_cache = current_cache->next;
        }
      }
      auto_cache_unlock();
      ESP_LOGI(TAG,
               "缓存状态: 活跃条目=%d (01=%d, 02=%d, 03=%d, 04=%d), "
               "访问热度=%u, CH3利用率=%u.%u%%",
               cache_count, bank_counts[0], bank_counts[1], bank_counts[2],
               bank_counts[3], total_access_heat,
               ch3_utilization_permille / 10,
               ch3_utilization_permille % 10);
      last_status_time = current_time;
    }

    uint32_t delay_ms = auto_cache_poll_delay_ms(has_urgent);
    ulTaskNotifyTake(pdTRUE, sx_ms_to_ticks(delay_ms));
  }

  auto_poll_task_exited = true;
  auto_poll_task_handle = NULL;
  ESP_LOGI(TAG, "自动缓存轮询任务退出");
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

// 自动智能缓存任务函数 - 仅处理主机请求
void auto_intelligent_cache_task(void *pvParameter) {
  static const char *TAG = "AUTO_INTELLIGENT_CACHE_TASK";

  ESP_LOGI(TAG, "透明智能缓存任务启动");
  auto_cache_task_exited = false;

  // 关键修复：检查当前工作模式，防止多任务冲突
  char work_mode[32] = {0};
  nvs_handle_t nvs_check;
  if (nvs_open("storage", NVS_READONLY, &nvs_check) == ESP_OK) {
    size_t len = sizeof(work_mode);
    nvs_get_str(nvs_check, "w_mode", work_mode, &len);
    nvs_close(nvs_check);

    if (strcmp(work_mode, "modbus_cache") != 0) {
      ESP_LOGW(TAG,
               "⚠️  当前工作模式不是modbus_cache（当前：%s），任务退出防止冲突",
               work_mode);
      auto_cache_task_exited = true;
      auto_cache_task_handle = NULL;
      delete_self_app_task_with_caps();
      return;
    }
  }

  uint8_t buffer[1024];
  uint64_t timestamp;
  int data_len;

  ESP_LOGI(TAG, "工作模式: CH1/CH2主站 <-> 智能缓存 <-> CH3从机总线");

  while (!auto_cache_tasks_stopping) {
    // 1. 检查写请求响应（异步处理）
    check_write_response();

    // 2. 处理CH1请求
    data_len = pop_channel_data(1, buffer, sizeof(buffer), &timestamp);
    if (data_len > 0) {
      uint8_t function_code = (data_len > 1) ? buffer[1] : 0;
      if (auto_cache_is_supported_read(function_code)) {
        handle_read_request(1, buffer, data_len);
      } else if (function_code == MODBUS_WRITE_SINGLE_COIL ||
                 function_code == MODBUS_WRITE_SINGLE_REG ||
                 function_code == MODBUS_WRITE_MULTIPLE_COILS ||
                 function_code == MODBUS_WRITE_MULTIPLE_REGS) {
        handle_write_request(1, buffer, data_len);
      } else {
        ESP_LOGD(TAG, "CH1未知功能码: 0x%02X", function_code);
      }
    }

    // 3. 处理CH2请求
    data_len = pop_channel_data(2, buffer, sizeof(buffer), &timestamp);
    if (data_len > 0) {
      uint8_t function_code = (data_len > 1) ? buffer[1] : 0;
      if (auto_cache_is_supported_read(function_code)) {
        handle_read_request(2, buffer, data_len);
      } else if (function_code == MODBUS_WRITE_SINGLE_COIL ||
                 function_code == MODBUS_WRITE_SINGLE_REG ||
                 function_code == MODBUS_WRITE_MULTIPLE_COILS ||
                 function_code == MODBUS_WRITE_MULTIPLE_REGS) {
        handle_write_request(2, buffer, data_len);
      } else {
        ESP_LOGD(TAG, "CH2未知功能码: 0x%02X", function_code);
      }
    }

    // 高频检查主站请求，降低自动缓存建项和命中的调度延迟
    vTaskDelay(sx_ms_to_ticks(AUTO_CACHE_READ_TASK_DELAY_MS));
  }

  auto_cache_task_exited = true;
  auto_cache_task_handle = NULL;
  ESP_LOGI(TAG, "透明智能缓存任务退出");
  vTaskDelay(sx_ms_to_ticks(1));
  delete_self_app_task_with_caps();
}

// ================== 缓存管理功能实现 ==================

// 分析配置，构建融合的真实数据表
static esp_err_t build_real_data_table(ModbusCacheTaskConfig *config,
                                       RealDataEntry **real_table,
                                       int *real_count) {
  static const char *TAG = "BUILD_REAL_TABLE";

  // 临时存储每个从机的寄存器范围
  typedef struct {
    uint8_t slave_addr;
    uint8_t function_code;
    uint16_t min_register;
    uint16_t max_register;
    bool has_data;
  } SlaveRegisterRange;

  SlaveRegisterRange *ranges = (SlaveRegisterRange *)calloc(
      config->items_count, sizeof(SlaveRegisterRange));
  if (!ranges) {
    return ESP_ERR_NO_MEM;
  }

  int unique_slaves = 0;

  // 分析配置，找出每个从机的寄存器范围
  for (int i = 0; i < config->items_count; i++) {
    if (!config->items[i].enabled)
      continue;

    ESP_LOGI(TAG,
             "配置项 %d: slave_addr='%s', function_code='%s', "
             "register_addr='%s', register_num='%s'",
             i, config->items[i].slave_addr, config->items[i].function_code,
             config->items[i].register_addr, config->items[i].register_num);

    uint8_t slave_addr = atoi(config->items[i].slave_addr);
    uint8_t function_code = atoi(config->items[i].function_code);
    uint16_t start_reg = atoi(config->items[i].register_addr);
    uint16_t reg_count = atoi(config->items[i].register_num);

    ESP_LOGI(
        TAG,
        "解析结果: slave_addr=%d, function_code=%d, start_reg=%d, reg_count=%d",
        slave_addr, function_code, start_reg, reg_count);

    // 检查配置有效性
    if (reg_count == 0) {
      ESP_LOGW(TAG, "配置项 %d 的寄存器数量为0，跳过", i);
      continue;
    }
    if (slave_addr == 0) {
      ESP_LOGW(TAG, "配置项 %d 的从机地址为0，跳过", i);
      continue;
    }
    if ((function_code != MODBUS_READ_HOLDING &&
         function_code != MODBUS_READ_INPUT) ||
        reg_count > MODBUS_MAX_REGISTER_COUNT ||
        !modbus_u16_range_valid(start_reg, reg_count)) {
      ESP_LOGW(TAG,
               "配置项 %d 的功能码或地址范围无效，跳过: fc=0x%02X addr=%u qty=%u",
               i, function_code, start_reg, reg_count);
      continue;
    }
    uint16_t end_reg = (uint16_t)(start_reg + reg_count - 1);

    // 查找是否已有相同从机地址和功能码的范围
    int found_index = -1;
    for (int j = 0; j < unique_slaves; j++) {
      if (ranges[j].slave_addr == slave_addr &&
          ranges[j].function_code == function_code) {
        found_index = j;
        break;
      }
    }

    if (found_index >= 0) {
      // 扩展现有范围
      if (start_reg < ranges[found_index].min_register) {
        ranges[found_index].min_register = start_reg;
      }
      if (end_reg > ranges[found_index].max_register) {
        ranges[found_index].max_register = end_reg;
      }
    } else {
      // 添加新的从机范围
      ranges[unique_slaves].slave_addr = slave_addr;
      ranges[unique_slaves].function_code = function_code;
      ranges[unique_slaves].min_register = start_reg;
      ranges[unique_slaves].max_register = end_reg;
      ranges[unique_slaves].has_data = true;
      unique_slaves++;
    }
  }

  ESP_LOGI(TAG, "分析完成，发现 %d 个唯一的从机/功能码组合", unique_slaves);

  // 检查是否有有效的配置
  if (unique_slaves == 0) {
    ESP_LOGW(TAG, "没有找到有效的配置项，无法构建真实数据表");
    free(ranges);
    *real_table = NULL;
    *real_count = 0;
    return ESP_ERR_INVALID_ARG;
  }

  // 分配真实数据表
  ESP_LOGI(TAG, "尝试分配真实数据表: %d 个表项, 共 %d 字节", unique_slaves,
           unique_slaves * sizeof(RealDataEntry));
  *real_table = (RealDataEntry *)calloc(unique_slaves, sizeof(RealDataEntry));
  if (!*real_table) {
    ESP_LOGE(TAG, "无法分配真实数据表内存: %d 字节",
             unique_slaves * sizeof(RealDataEntry));
    free(ranges);
    return ESP_ERR_NO_MEM;
  }

  // 构建真实数据表
  for (int i = 0; i < unique_slaves; i++) {
    RealDataEntry *entry = &(*real_table)[i];
    entry->slave_addr = ranges[i].slave_addr;
    entry->function_code = ranges[i].function_code;
    entry->start_register = ranges[i].min_register;
    entry->register_count = ranges[i].max_register - ranges[i].min_register + 1;
    entry->data_length = entry->register_count * 2; // 每个寄存器2字节

    // 检查内存分配大小并限制最大分配
    ESP_LOGI(TAG, "尝试为表项 %d 分配 %d 字节 (%d 个寄存器)", i,
             entry->data_length, entry->register_count);

    // 限制单次最大分配为 1024 字节，如果需要更多则分段处理
    if (entry->data_length > 1024) {
      ESP_LOGW(TAG, "寄存器范围过大 (%d 字节)，限制为1024字节",
               entry->data_length);
      entry->data_length = 1024;
      entry->register_count = entry->data_length / 2;
    }

    entry->data = (uint8_t *)calloc(entry->data_length, sizeof(uint8_t));
    entry->data_valid = false;
    entry->timestamp = 0;

    if (!entry->data) {
      ESP_LOGE(TAG, "无法分配 %d 字节内存", entry->data_length);
      // 清理已分配的内存
      for (int j = 0; j < i; j++) {
        free((*real_table)[j].data);
      }
      free(*real_table);
      free(ranges);
      return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "真实数据表项 %d: 从机%d, 功能码%d, 寄存器%d-%d (%d个)", i,
             entry->slave_addr, entry->function_code, entry->start_register,
             entry->start_register + entry->register_count - 1,
             entry->register_count);
  }

  *real_count = unique_slaves;
  free(ranges);
  return ESP_OK;
}

// 构建映射数据表
static esp_err_t build_mapped_data_table(ModbusCacheTaskConfig *config,
                                         RealDataEntry *real_table,
                                         int real_count,
                                         MappedDataEntry **mapped_table,
                                         int *mapped_count) {
  static const char *TAG = "BUILD_MAPPED_TABLE";

  // 计算启用的映射项数量
  int enabled_count = 0;
  for (int i = 0; i < config->items_count; i++) {
    if (config->items[i].enabled) {
      enabled_count++;
    }
  }

  *mapped_table =
      (MappedDataEntry *)calloc(enabled_count, sizeof(MappedDataEntry));
  if (!*mapped_table) {
    return ESP_ERR_NO_MEM;
  }

  int mapped_index = 0;
  for (int i = 0; i < config->items_count; i++) {
    if (!config->items[i].enabled)
      continue;

    MappedDataEntry *mapped = &(*mapped_table)[mapped_index];

    // 映射地址信息
    mapped->mapped_slave_addr = atoi(config->items[i].mapped_slave_addr);
    mapped->mapped_register = atoi(config->items[i].mapped_register_addr);
    mapped->register_count = atoi(config->items[i].register_num);

    // 查找对应的真实数据表项
    uint8_t real_slave = atoi(config->items[i].slave_addr);
    uint8_t real_function = atoi(config->items[i].function_code);
    uint16_t real_register = atoi(config->items[i].register_addr);
    if ((real_function != MODBUS_READ_HOLDING &&
         real_function != MODBUS_READ_INPUT) ||
        mapped->register_count > MODBUS_MAX_REGISTER_COUNT ||
        !modbus_u16_range_valid(real_register, mapped->register_count) ||
        !modbus_u16_range_valid(mapped->mapped_register,
                                mapped->register_count)) {
      ESP_LOGE(TAG, "映射项 %d 的功能码或地址范围无效", i);
      free(*mapped_table);
      *mapped_table = NULL;
      *mapped_count = 0;
      return ESP_ERR_INVALID_ARG;
    }

    mapped->real_data_ref = NULL;
    for (int j = 0; j < real_count; j++) {
      RealDataEntry *real = &real_table[j];
      uint32_t real_req_end =
          (uint32_t)real_register + (uint32_t)mapped->register_count;
      uint32_t real_table_end =
          (uint32_t)real->start_register + (uint32_t)real->register_count;
      if (real->slave_addr == real_slave &&
          real->function_code == real_function &&
          real_register >= real->start_register &&
          real_req_end <= real_table_end) {

        mapped->real_data_ref = real;
        mapped->real_offset = real_register - real->start_register;
        break;
      }
    }

    if (!mapped->real_data_ref) {
      ESP_LOGE(TAG, "无法找到映射项 %d 对应的真实数据表项", i);
      free(*mapped_table);
      return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "映射表项 %d: 映射[%d:%d+%d] -> 真实[%d:%d+%d] 偏移%d",
             mapped_index, mapped->mapped_slave_addr, mapped->mapped_register,
             mapped->register_count, mapped->real_data_ref->slave_addr,
             mapped->real_data_ref->start_register + mapped->real_offset,
             mapped->register_count, mapped->real_offset);

    mapped_index++;
  }

  *mapped_count = enabled_count;
  return ESP_OK;
}

// 初始化缓存管理器
esp_err_t init_cache_manager(ModbusCacheTaskConfig *config) {
  static const char *CACHE_TAG = "CACHE_MANAGER";

  ESP_LOGI(CACHE_TAG, "基于新架构初始化缓存管理器");

  // 创建互斥锁
  g_cache_manager.cache_mutex = xSemaphoreCreateMutex();
  if (g_cache_manager.cache_mutex == NULL) {
    ESP_LOGE(CACHE_TAG, "创建缓存互斥锁失败");
    return ESP_ERR_NO_MEM;
  }

  // 构建真实数据表
  esp_err_t ret =
      build_real_data_table(config, &g_cache_manager.real_data_table,
                            &g_cache_manager.real_data_count);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_INVALID_ARG) {
      ESP_LOGW(CACHE_TAG, "没有有效的配置项，使用空的数据表");
      g_cache_manager.real_data_table = NULL;
      g_cache_manager.real_data_count = 0;
      g_cache_manager.mapped_data_table = NULL;
      g_cache_manager.mapped_data_count = 0;
      return ESP_OK; // 继续执行，但使用空配置
    } else {
      ESP_LOGE(CACHE_TAG, "构建真实数据表失败: %s", esp_err_to_name(ret));
      vSemaphoreDelete(g_cache_manager.cache_mutex);
      g_cache_manager.cache_mutex = NULL;
      return ret;
    }
  }

  // 构建映射数据表（仅当有真实数据表时）
  if (g_cache_manager.real_data_count > 0) {
    ret = build_mapped_data_table(config, g_cache_manager.real_data_table,
                                  g_cache_manager.real_data_count,
                                  &g_cache_manager.mapped_data_table,
                                  &g_cache_manager.mapped_data_count);
    if (ret != ESP_OK) {
      ESP_LOGE(CACHE_TAG, "构建映射数据表失败: %s", esp_err_to_name(ret));
      // 清理真实数据表
      for (int i = 0; i < g_cache_manager.real_data_count; i++) {
        free(g_cache_manager.real_data_table[i].data);
      }
      free(g_cache_manager.real_data_table);
      g_cache_manager.real_data_table = NULL;
      g_cache_manager.real_data_count = 0;
      g_cache_manager.mapped_data_table = NULL;
      g_cache_manager.mapped_data_count = 0;
      vSemaphoreDelete(g_cache_manager.cache_mutex);
      g_cache_manager.cache_mutex = NULL;
      return ret;
    }
  }

  ESP_LOGI(CACHE_TAG, "缓存管理器初始化完成 - 真实表项: %d, 映射表项: %d",
           g_cache_manager.real_data_count, g_cache_manager.mapped_data_count);
  return ESP_OK;
}

// 销毁缓存管理器
void destroy_cache_manager(void) {
  static const char *CACHE_TAG = "CACHE_MANAGER";

  if (g_cache_manager.cache_mutex != NULL) {
    xSemaphoreTake(g_cache_manager.cache_mutex, portMAX_DELAY);

    // 释放真实数据表
    if (g_cache_manager.real_data_table != NULL) {
      for (int i = 0; i < g_cache_manager.real_data_count; i++) {
        if (g_cache_manager.real_data_table[i].data != NULL) {
          free(g_cache_manager.real_data_table[i].data);
        }
      }
      free(g_cache_manager.real_data_table);
      g_cache_manager.real_data_table = NULL;
    }

    // 释放映射数据表
    if (g_cache_manager.mapped_data_table != NULL) {
      free(g_cache_manager.mapped_data_table);
      g_cache_manager.mapped_data_table = NULL;
    }

    xSemaphoreGive(g_cache_manager.cache_mutex);
    vSemaphoreDelete(g_cache_manager.cache_mutex);
    g_cache_manager.cache_mutex = NULL;
  }

  g_cache_manager.real_data_count = 0;
  g_cache_manager.mapped_data_count = 0;
  ESP_LOGI(CACHE_TAG, "缓存管理器已销毁");
}

// 更新真实数据
esp_err_t update_real_data(uint8_t slave_addr, uint16_t start_register,
                           uint16_t register_count, uint8_t *response_data,
                           size_t data_length) {
  static const char *CACHE_TAG = "UPDATE_REAL_DATA";

  if (response_data == NULL ||
      data_length < 5) { // 至少需要5字节：地址+功能码+字节数+CRC(2字节)
    ESP_LOGE(CACHE_TAG, "无效的响应数据，长度:%zu", data_length);
    return ESP_ERR_INVALID_ARG;
  }
  if (register_count > MODBUS_MAX_REGISTER_COUNT ||
      !modbus_u16_range_valid(start_register, register_count)) {
    ESP_LOGE(CACHE_TAG, "无效更新范围: 从机%d, 寄存器%d+%d", slave_addr,
             start_register, register_count);
    return ESP_ERR_INVALID_ARG;
  }

  // 验证CRC
  uint16_t received_crc =
      (response_data[data_length - 1] << 8) | response_data[data_length - 2];
  uint16_t calculated_crc = calculate_crc(response_data, data_length - 2);

  if (received_crc != calculated_crc) {
    ESP_LOGE(CACHE_TAG, "CRC验证失败: 接收=%04X, 计算=%04X", received_crc,
             calculated_crc);
    ESP_LOG_BUFFER_HEXDUMP(CACHE_TAG, response_data, data_length,
                           ESP_LOG_ERROR);
    return ESP_ERR_INVALID_CRC;
  }

  ESP_LOGI(CACHE_TAG, "CRC验证通过: %04X", received_crc);

  // 验证响应数据长度是否与请求的寄存器数量匹配
  size_t expected_payload_length = register_count * 2; // 每个寄存器2字节
  size_t actual_payload_length =
      data_length - 5; // 减去响应头(3字节)和CRC(2字节)

  if (actual_payload_length != expected_payload_length) {
    ESP_LOGE(CACHE_TAG, "响应数据长度不匹配: 期望%zu字节, 实际%zu字节",
             expected_payload_length, actual_payload_length);
    return ESP_ERR_INVALID_SIZE;
  }

  ESP_LOGI(CACHE_TAG, "响应数据长度验证通过: %zu字节 (%d个寄存器)",
           actual_payload_length, register_count);

  if (g_cache_manager.cache_mutex == NULL) {
    ESP_LOGE(CACHE_TAG, "缓存管理器未初始化");
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(g_cache_manager.cache_mutex, portMAX_DELAY);

  // 查找对应的真实数据表项
  RealDataEntry *found_entry = NULL;
  for (int i = 0; i < g_cache_manager.real_data_count; i++) {
    RealDataEntry *entry = &g_cache_manager.real_data_table[i];
    uint32_t req_end =
        (uint32_t)start_register + (uint32_t)register_count;
    uint32_t entry_end =
        (uint32_t)entry->start_register + (uint32_t)entry->register_count;
    if (entry->slave_addr == slave_addr &&
        start_register >= entry->start_register &&
        req_end <= entry_end) {
      found_entry = entry;
      break;
    }
  }

  if (!found_entry) {
    ESP_LOGW(CACHE_TAG, "未找到匹配的真实数据表项: 从机%d, 寄存器%d",
             slave_addr, start_register);
    xSemaphoreGive(g_cache_manager.cache_mutex);
    return ESP_ERR_NOT_FOUND;
  }

  // 计算数据在缓存中的偏移位置
  uint16_t offset =
      (start_register - found_entry->start_register) * 2; // 每个寄存器2字节
  uint8_t *payload = response_data + 3;                   // 跳过Modbus响应头
  size_t payload_length = data_length - 5; // 减去响应头(3字节)和CRC(2字节)

  // 检查数据是否超出缓存范围
  if (offset + payload_length > found_entry->data_length) {
    ESP_LOGE(CACHE_TAG, "数据超出缓存范围");
    xSemaphoreGive(g_cache_manager.cache_mutex);
    return ESP_ERR_INVALID_SIZE;
  }

  // 更新数据
  memcpy(found_entry->data + offset, payload, payload_length);
  found_entry->timestamp = esp_timer_get_time();
  found_entry->data_valid = true;

  ESP_LOGI(CACHE_TAG, "真实数据已更新: 从机%d, 寄存器%d+%d, 数据长度%d字节",
           slave_addr, start_register, payload_length / 2, payload_length);

  xSemaphoreGive(g_cache_manager.cache_mutex);
  return ESP_OK;
}

// 根据映射地址获取数据
esp_err_t get_mapped_data(uint8_t mapped_slave_addr,
                          uint16_t mapped_register_addr,
                          uint16_t register_count, uint8_t *output_data,
                          size_t *output_length) {
  static const char *CACHE_TAG = "CACHE_GET";

  if (output_data == NULL || output_length == NULL) {
    ESP_LOGE(CACHE_TAG, "输出参数为空");
    return ESP_ERR_INVALID_ARG;
  }
  if (register_count > MODBUS_MAX_REGISTER_COUNT ||
      !modbus_u16_range_valid(mapped_register_addr, register_count)) {
    ESP_LOGE(CACHE_TAG, "无效映射读取范围: 从机%d, 寄存器%d+%d",
             mapped_slave_addr, mapped_register_addr, register_count);
    return ESP_ERR_INVALID_ARG;
  }

  if (g_cache_manager.cache_mutex == NULL) {
    ESP_LOGE(CACHE_TAG, "缓存管理器未初始化");
    return ESP_ERR_INVALID_STATE;
  }

  xSemaphoreTake(g_cache_manager.cache_mutex, portMAX_DELAY);

  esp_err_t result = ESP_ERR_NOT_FOUND;

  // 在映射数据表中查找匹配项
  MappedDataEntry *found_mapping = NULL;
  for (int i = 0; i < g_cache_manager.mapped_data_count; i++) {
    MappedDataEntry *mapping = &g_cache_manager.mapped_data_table[i];
    uint32_t req_end =
        (uint32_t)mapped_register_addr + (uint32_t)register_count;
    uint32_t mapping_end =
        (uint32_t)mapping->mapped_register + (uint32_t)mapping->register_count;

    if (mapping->mapped_slave_addr == mapped_slave_addr &&
        mapped_register_addr >= mapping->mapped_register &&
        req_end <= mapping_end) {
      found_mapping = mapping;
      break;
    }
  }

  if (!found_mapping) {
    ESP_LOGW(CACHE_TAG, "未找到映射: 从机%d, 寄存器%d+%d", mapped_slave_addr,
             mapped_register_addr, register_count);
    result = ESP_ERR_NOT_FOUND;
  } else {
    // 检查真实数据是否有效
    RealDataEntry *real_data = found_mapping->real_data_ref;
    if (!real_data || !real_data->data_valid) {
      ESP_LOGW(CACHE_TAG, "真实数据无效或未更新");
      result = ESP_ERR_INVALID_STATE;
    } else {
      // 计算在真实数据中的偏移
      uint16_t mapping_offset =
          mapped_register_addr - found_mapping->mapped_register;
      uint16_t real_data_offset =
          (found_mapping->real_offset + mapping_offset) * 2; // 字节偏移
      size_t data_size = register_count * 2;

      // 检查数据范围
      if (real_data_offset + data_size <= real_data->data_length) {
        memcpy(output_data, real_data->data + real_data_offset, data_size);
        *output_length = data_size;
        result = ESP_OK;

        ESP_LOGI(CACHE_TAG,
                 "映射数据获取成功: 映射[%d:%d+%d] -> 真实[%d:%d+%d]",
                 mapped_slave_addr, mapped_register_addr, register_count,
                 real_data->slave_addr,
                 real_data->start_register + found_mapping->real_offset +
                     mapping_offset,
                 register_count);
      } else {
        ESP_LOGE(CACHE_TAG, "数据范围超出真实数据边界");
        result = ESP_ERR_INVALID_SIZE;
      }
    }
  }

  if (result != ESP_OK) {
    ESP_LOGW(CACHE_TAG, "未找到匹配的缓存数据 - 映射从机: %d, 映射寄存器: %d",
             mapped_slave_addr, mapped_register_addr);
  }

  xSemaphoreGive(g_cache_manager.cache_mutex);
  return result;
}

// 打印缓存状态（调试用）
void print_cache_status(void) {
  static const char *CACHE_TAG = "CACHE_STATUS";

  if (g_cache_manager.cache_mutex == NULL) {
    ESP_LOGW(CACHE_TAG, "缓存管理器未初始化");
    return;
  }

  xSemaphoreTake(g_cache_manager.cache_mutex, portMAX_DELAY);

  ESP_LOGI(CACHE_TAG, "=== 缓存状态报告 ===");
  ESP_LOGI(CACHE_TAG, "真实数据表项数: %d", g_cache_manager.real_data_count);
  ESP_LOGI(CACHE_TAG, "映射数据表项数: %d", g_cache_manager.mapped_data_count);

  // 打印真实数据表状态
  for (int i = 0; i < g_cache_manager.real_data_count; i++) {
    RealDataEntry *real_item = &g_cache_manager.real_data_table[i];
    ESP_LOGI(CACHE_TAG,
             "真实数据 %d: 从机%d, 功能码%d, 寄存器%d+%d, 有效=%s, 数据长度=%d",
             i, real_item->slave_addr, real_item->function_code,
             real_item->start_register, real_item->register_count,
             real_item->data_valid ? "是" : "否", real_item->data_length);
  }

  // 打印映射数据表状态
  for (int i = 0; i < g_cache_manager.mapped_data_count; i++) {
    MappedDataEntry *mapped_item = &g_cache_manager.mapped_data_table[i];
    ESP_LOGI(CACHE_TAG, "映射数据 %d: 映射从机%d, 映射寄存器%d+%d, 真实偏移%d",
             i, mapped_item->mapped_slave_addr, mapped_item->mapped_register,
             mapped_item->register_count, mapped_item->real_offset);
  }

  xSemaphoreGive(g_cache_manager.cache_mutex);
}

// ================== 虚拟从机响应功能实现 ==================

// 构建Modbus响应数据包
esp_err_t build_modbus_response(uint8_t slave_addr, uint8_t function_code,
                                uint16_t register_addr, uint16_t register_count,
                                uint8_t *response_buffer,
                                size_t *response_length) {
  static const char *RESP_TAG = "VIRTUAL_RESPONSE";

  if (response_buffer == NULL || response_length == NULL) {
    ESP_LOGE(RESP_TAG, "响应缓冲区参数无效");
    return ESP_ERR_INVALID_ARG;
  }
  if ((function_code != MODBUS_READ_HOLDING &&
       function_code != MODBUS_READ_INPUT) ||
      register_count > MODBUS_MAX_REGISTER_COUNT ||
      !modbus_u16_range_valid(register_addr, register_count)) {
    ESP_LOGW(RESP_TAG, "旧虚拟响应不支持或范围无效: fc=0x%02X addr=%u qty=%u",
             function_code, register_addr, register_count);
    return ESP_ERR_INVALID_ARG;
  }

  // 从缓存中获取数据（附加新鲜度判断）
  uint8_t cached_data[1024]; // 增大到1024字节支持两个125寄存器包
  size_t cached_data_length = 0;

  esp_err_t cache_result =
      get_mapped_data(slave_addr, register_addr, register_count, cached_data,
                      &cached_data_length);

  if (cache_result != ESP_OK) {
    ESP_LOGI(RESP_TAG, "未找到映射配置，应该透传请求到真实从机");
    // 返回特殊错误码，指示需要透传
    return ESP_ERR_NOT_FOUND;
  }

  // 可选：检查数据新鲜度（例如2秒内更新视为新鲜）
  bool fresh_ok = false;
  if (g_cache_manager.cache_mutex != NULL) {
    xSemaphoreTake(g_cache_manager.cache_mutex, portMAX_DELAY);
    for (int i = 0; i < g_cache_manager.mapped_data_count; i++) {
      MappedDataEntry *m = &g_cache_manager.mapped_data_table[i];
      uint32_t req_end = (uint32_t)register_addr + (uint32_t)register_count;
      uint32_t mapping_end =
          (uint32_t)m->mapped_register + (uint32_t)m->register_count;
      if (m->mapped_slave_addr == slave_addr &&
          register_addr >= m->mapped_register &&
          req_end <= mapping_end) {
        RealDataEntry *real = m->real_data_ref;
        if (real && real->data_valid) {
          uint64_t now_us = esp_timer_get_time();
          // 使用 AUTO_CACHE_DATA_FRESH_TIME_MS 新鲜度阈值
          fresh_ok = (now_us - real->timestamp) <=
                     ((uint64_t)AUTO_CACHE_DATA_FRESH_TIME_MS * 1000ULL);
        }
        break;
      }
    }
    xSemaphoreGive(g_cache_manager.cache_mutex);
  }

  if (!fresh_ok) {
    ESP_LOGW(RESP_TAG, "缓存数据过期，建议透传真实从机");
    return ESP_ERR_NOT_FOUND;
  }

  // 构建正常响应
  response_buffer[0] = slave_addr;
  response_buffer[1] = function_code;
  response_buffer[2] = cached_data_length; // 数据字节数

  // 复制缓存数据
  memcpy(&response_buffer[3], cached_data, cached_data_length);

  // 计算并添加CRC
  size_t data_length = 3 + cached_data_length;
  uint16_t crc = calculate_crc(response_buffer, data_length);
  response_buffer[data_length] = crc & 0xFF;
  response_buffer[data_length + 1] = (crc >> 8) & 0xFF;

  *response_length = data_length + 2;

  ESP_LOGI(RESP_TAG, "构建响应成功 - 从机:%d, 功能码:%d, 响应长度:%d",
           slave_addr, function_code, *response_length);
  ESP_LOG_BUFFER_HEXDUMP(RESP_TAG, response_buffer, *response_length,
                         ESP_LOG_INFO);

  return ESP_OK;
}

// 虚拟从机响应任务
void virtual_slave_response_task(void *pvParameter) {
  static const char *TASK_TAG = "VIRTUAL_SLAVE_TASK";

  ESP_LOGI(TASK_TAG, "虚拟从机响应任务启动");

  uint8_t request_buffer[1024];
  uint64_t request_timestamp;

  while (1) {
    // 检查CH1通道的请求
    int ch1_data_len = pop_channel_data(
        1, request_buffer, sizeof(request_buffer), &request_timestamp);
    if (ch1_data_len >=
        8) { // 最小Modbus请求长度：地址(1)+功能码(1)+寄存器地址(2)+寄存器数量(2)+CRC(2)
             // = 8字节
      ESP_LOGI(TASK_TAG, "从CH1接收到请求，长度: %d 字节", ch1_data_len);
      ESP_LOG_BUFFER_HEXDUMP(TASK_TAG, request_buffer, ch1_data_len,
                             ESP_LOG_INFO);

      // 解析Modbus请求
      uint8_t slave_addr = request_buffer[0];
      uint8_t function_code = request_buffer[1];
      uint16_t register_addr = (request_buffer[2] << 8) | request_buffer[3];
      uint16_t register_count = (request_buffer[4] << 8) | request_buffer[5];

      ESP_LOGI(TASK_TAG, "CH1请求解析 - 从机:%d, 功能码:%d, 寄存器:%d, 数量:%d",
               slave_addr, function_code, register_addr, register_count);
      if (!verify_crc(request_buffer, ch1_data_len) ||
          (function_code != MODBUS_READ_HOLDING &&
           function_code != MODBUS_READ_INPUT) ||
          register_count > MODBUS_MAX_REGISTER_COUNT ||
          !modbus_u16_range_valid(register_addr, register_count)) {
        ESP_LOGW(TASK_TAG,
                 "丢弃CH1旧虚拟响应无效请求: slave=%u fc=0x%02X addr=%u qty=%u",
                 slave_addr, function_code, register_addr, register_count);
        goto check_ch2_request;
      }

      // 构建并发送响应
      uint8_t response_buffer[1024];
      size_t response_length = 0;

      esp_err_t build_result = build_modbus_response(
          slave_addr, function_code, register_addr, register_count,
          response_buffer, &response_length);

      if (build_result == ESP_OK && response_length > 0) {
        // 发送缓存响应到CH1
        tx_tasks_to_channel(response_buffer, response_length, 1);
        ESP_LOGI(TASK_TAG, "已向CH1发送缓存响应，长度: %d 字节",
                 response_length);
      } else if (build_result == ESP_ERR_NOT_FOUND) {
        // 未找到映射配置，透传请求到真实从机
        ESP_LOGI(TASK_TAG,
                 "透传CH1请求到CH3: 从机%d, 功能码%d, 寄存器%d, 数量%d",
                 slave_addr, function_code, register_addr, register_count);

        // 使用CH3通道超时时间
        int timeout = timeout3;

        // 暂停接收任务，避免干扰
        suspend_all_uart_rx_tasks();
        vTaskDelay(pdMS_TO_TICKS(20));

        // 转发原始请求到CH3（使用智能发送，跟随模式自动跟随来源参数）
        smart_send_data_to_ch3(get_uart_config_mode(), 1, request_buffer,
                               ch1_data_len);

        // 恢复接收任务
        resume_all_uart_rx_tasks();

        // 等待CH3响应并转发回CH1
        uint8_t ch3_response[1024]; // 增大到1024字节支持两个125寄存器包
        uint64_t ch3_timestamp;
        int wait_time = 0;
        const int check_interval = 10;
        bool received_response = false;

        while (wait_time < timeout && !received_response) {
          int ch3_data_len = pop_channel_data(
              3, ch3_response, sizeof(ch3_response), &ch3_timestamp);
          if (ch3_data_len > 0) {
            // 转发CH3响应到CH1
            tx_tasks_to_channel(ch3_response, ch3_data_len, 1);
            ESP_LOGI(TASK_TAG, "已透传CH3响应到CH1，长度: %d 字节",
                     ch3_data_len);
            received_response = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(check_interval));
          wait_time += check_interval;
        }

        if (!received_response) {
          ESP_LOGW(TASK_TAG, "透传CH1请求超时，未收到CH3响应");
        }
      }
    }

check_ch2_request:;
    // 检查CH2通道的请求
    int ch2_data_len = pop_channel_data(
        2, request_buffer, sizeof(request_buffer), &request_timestamp);
    if (ch2_data_len >= 8) {
      ESP_LOGI(TASK_TAG, "从CH2接收到请求，长度: %d 字节", ch2_data_len);
      ESP_LOG_BUFFER_HEXDUMP(TASK_TAG, request_buffer, ch2_data_len,
                             ESP_LOG_INFO);

      // 解析Modbus请求
      uint8_t slave_addr = request_buffer[0];
      uint8_t function_code = request_buffer[1];
      uint16_t register_addr = (request_buffer[2] << 8) | request_buffer[3];
      uint16_t register_count = (request_buffer[4] << 8) | request_buffer[5];

      ESP_LOGI(TASK_TAG, "CH2请求解析 - 从机:%d, 功能码:%d, 寄存器:%d, 数量:%d",
               slave_addr, function_code, register_addr, register_count);
      if (!verify_crc(request_buffer, ch2_data_len) ||
          (function_code != MODBUS_READ_HOLDING &&
           function_code != MODBUS_READ_INPUT) ||
          register_count > MODBUS_MAX_REGISTER_COUNT ||
          !modbus_u16_range_valid(register_addr, register_count)) {
        ESP_LOGW(TASK_TAG,
                 "丢弃CH2旧虚拟响应无效请求: slave=%u fc=0x%02X addr=%u qty=%u",
                 slave_addr, function_code, register_addr, register_count);
        vTaskDelay(pdMS_TO_TICKS(20)); // 避免任务空转
        continue;
      }

      // 构建并发送响应
      uint8_t response_buffer[1024];
      size_t response_length = 0;

      esp_err_t build_result = build_modbus_response(
          slave_addr, function_code, register_addr, register_count,
          response_buffer, &response_length);

      if (build_result == ESP_OK && response_length > 0) {
        // 发送缓存响应到CH2
        tx_tasks_to_channel(response_buffer, response_length, 2);
        ESP_LOGI(TASK_TAG, "已向CH2发送缓存响应，长度: %d 字节",
                 response_length);
      } else if (build_result == ESP_ERR_NOT_FOUND) {
        // 未找到映射配置，透传请求到真实从机
        ESP_LOGI(TASK_TAG, "透传请求到CH3: 从机%d, 功能码%d, 寄存器%d, 数量%d",
                 slave_addr, function_code, register_addr, register_count);

        // 使用CH3通道超时时间
        int timeout = timeout3;

        // 暂停接收任务，避免干扰
        suspend_all_uart_rx_tasks();
        vTaskDelay(pdMS_TO_TICKS(20));

        // 转发原始请求到CH3（使用智能发送，跟随模式自动跟随来源参数）
        smart_send_data_to_ch3(get_uart_config_mode(), 2, request_buffer,
                               ch2_data_len);

        // 恢复接收任务
        resume_all_uart_rx_tasks();

        // 等待CH3响应并转发回CH2
        uint8_t ch3_response[1024]; // 增大到1024字节支持两个125寄存器包
        uint64_t ch3_timestamp;
        int wait_time = 0;
        const int check_interval = 10;
        bool received_response = false;

        while (wait_time < timeout && !received_response) {
          int ch3_data_len = pop_channel_data(
              3, ch3_response, sizeof(ch3_response), &ch3_timestamp);
          if (ch3_data_len > 0) {
            // 转发CH3响应到CH2
            tx_tasks_to_channel(ch3_response, ch3_data_len, 2);
            ESP_LOGI(TASK_TAG, "已透传CH3响应到CH2，长度: %d 字节",
                     ch3_data_len);
            received_response = true;
            break;
          }
          vTaskDelay(pdMS_TO_TICKS(check_interval));
          wait_time += check_interval;
        }

        if (!received_response) {
          ESP_LOGW(TASK_TAG, "透传请求超时，未收到CH3响应");
        }
      }
    }

    // 延时，给其他任务更多CPU时间，特别是看门狗重置
    vTaskDelay(pdMS_TO_TICKS(30));
  }

  // 任务退出（通常不会执行到这里）
  ESP_LOGI(TASK_TAG, "虚拟从机响应任务退出");
}

// 启动虚拟从机响应任务
esp_err_t start_virtual_slave_response_task(void) {
  static const char *START_TAG = "START_VIRTUAL_SLAVE";

  if (virtual_slave_task_handle != NULL) {
    ESP_LOGW(START_TAG, "虚拟从机响应任务已在运行");
    return ESP_OK;
  }

  BaseType_t ret =
      create_app_task_psram(virtual_slave_response_task, "virtual_slave_task",
                            12288, NULL, 17, // 增加栈空间到12KB
                            &virtual_slave_task_handle, SX_WORK_CORE_ID);

  if (ret != pdPASS) {
    ESP_LOGE(START_TAG, "创建虚拟从机响应任务失败");
    return ESP_FAIL;
  }

  ESP_LOGI(START_TAG, "虚拟从机响应任务启动成功");
  return ESP_OK;
}

// 停止虚拟从机响应任务
void stop_virtual_slave_response_task(void) {
  static const char *STOP_TAG = "STOP_VIRTUAL_SLAVE";

  if (virtual_slave_task_handle != NULL) {
    delete_app_task_with_caps(virtual_slave_task_handle);
    virtual_slave_task_handle = NULL;
    ESP_LOGI(STOP_TAG, "虚拟从机响应任务已停止");
  }
}

// ================== 诊断和调试功能实现 ==================

// 打印当前工作模式状态（简化版本）
void print_current_work_mode(void) {
  static const char *DEBUG_TAG = "WORK_MODE_DEBUG";

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(DEBUG_TAG, "无法打开NVS: %s", esp_err_to_name(err));
    return;
  }

  // 检查工作模式
  char work_mode[32] = {0};
  size_t work_mode_len = sizeof(work_mode);
  err = nvs_get_str(nvs_handle, "w_mode", work_mode, &work_mode_len);
  ESP_LOGI(DEBUG_TAG, "当前工作模式: %s",
           (err == ESP_OK) ? work_mode : "未设置");

  // 检查自动透明缓存开关
  uint8_t auto_cache = 1;
  err = nvs_get_u8(nvs_handle, "auto_cache", &auto_cache);
  ESP_LOGI(DEBUG_TAG, "自动透明缓存: %s",
           (err == ESP_OK) ? (auto_cache ? "启用" : "禁用") : "未设置");

  // 检查模板数量（简化版本）
  int32_t items_count = 0;
  err = nvs_get_i32(nvs_handle, "m_count", &items_count);
  ESP_LOGI(DEBUG_TAG, "配置模板数量: %d",
           (err == ESP_OK) ? (int)items_count : 0);

  nvs_close(nvs_handle);
}

// 调试版本的模板数量获取函数
int get_modbus_items_count_debug(nvs_handle_t storage_handle) {
  static const char *DEBUG_TAG = "TEMPLATE_DEBUG";

  int32_t items_count = 0;
  esp_err_t err = nvs_get_i32(storage_handle, "m_count", &items_count);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(DEBUG_TAG, "NVS中未找到m_count键");
    return 0;
  } else if (err != ESP_OK) {
    ESP_LOGE(DEBUG_TAG, "读取m_count失败: %s", esp_err_to_name(err));
    return 0;
  }

  ESP_LOGI(DEBUG_TAG, "从NVS读取到模板数量: %d", items_count);

  // 检查每个模板的配置
  for (int i = 0; i < items_count; i++) {
    char key[32];

    // 检查启用状态
    snprintf(key, sizeof(key), "m%d_en", i);
    uint8_t enabled = 0;
    err = nvs_get_u8(storage_handle, key, &enabled);
    ESP_LOGI(DEBUG_TAG, "模板 %d 启用状态: %s", i,
             (err == ESP_OK) ? (enabled ? "启用" : "禁用") : "未设置");

    // 检查从机地址
    snprintf(key, sizeof(key), "m%ds_addr", i);
    char slave_addr[8] = {0};
    size_t len = sizeof(slave_addr);
    err = nvs_get_str(storage_handle, key, slave_addr, &len);
    ESP_LOGI(DEBUG_TAG, "模板 %d 从机地址: %s", i,
             (err == ESP_OK) ? slave_addr : "未设置");

    // 检查功能码
    snprintf(key, sizeof(key), "m%df_code", i);
    char func_code[8] = {0};
    len = sizeof(func_code);
    err = nvs_get_str(storage_handle, key, func_code, &len);
    ESP_LOGI(DEBUG_TAG, "模板 %d 功能码: %s", i,
             (err == ESP_OK) ? func_code : "未设置");

    // 检查映射地址
    snprintf(key, sizeof(key), "m%dm_s_addr", i);
    char mapped_slave[8] = {0};
    len = sizeof(mapped_slave);
    err = nvs_get_str(storage_handle, key, mapped_slave, &len);
    ESP_LOGI(DEBUG_TAG, "模板 %d 映射从机地址: %s", i,
             (err == ESP_OK) ? mapped_slave : "未设置");
  }

  return items_count;
}

// 打印完整的缓存系统状态
void print_modbus_cache_status(void) {
  static const char *STATUS_TAG = "CACHE_SYSTEM_STATUS";

  ESP_LOGI(STATUS_TAG, "=== Modbus缓存系统状态报告 ===");

  // 打印工作模式信息
  print_current_work_mode();

  // 打印任务状态
  ESP_LOGI(STATUS_TAG, "轮询任务状态: %s",
           (modbus_cache_task_handle != NULL) ? "运行中" : "未运行");
  ESP_LOGI(STATUS_TAG, "虚拟从机任务状态: %s",
           (virtual_slave_task_handle != NULL) ? "运行中" : "未运行");

  // 打印缓存状态
  if (g_cache_manager.cache_mutex != NULL) {
    ESP_LOGI(STATUS_TAG,
             "缓存管理器状态: 已初始化，真实数据表项: %d, 映射数据表项: %d",
             g_cache_manager.real_data_count,
             g_cache_manager.mapped_data_count);
    print_cache_status();
  } else {
    ESP_LOGI(STATUS_TAG, "缓存管理器状态: 未初始化");
  }

  ESP_LOGI(STATUS_TAG, "=== 状态报告结束 ===");
}

// 测试Modbus缓存配置功能
void test_modbus_cache_config(void) {
  static const char *TEST_TAG = "TEST_MODBUS_CONFIG";

  ESP_LOGI(TEST_TAG, "=== 开始测试Modbus缓存配置 ===");

  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TEST_TAG, "无法打开NVS: %s", esp_err_to_name(err));
    return;
  }

  // 测试1: 检查工作模式
  char work_mode[32] = {0};
  size_t work_mode_len = sizeof(work_mode);
  err = nvs_get_str(nvs_handle, "w_mode", work_mode, &work_mode_len);
  ESP_LOGI(TEST_TAG, "工作模式: %s (状态: %s)",
           (err == ESP_OK) ? work_mode : "未设置", esp_err_to_name(err));

  // 测试2: 检查自动透明缓存开关
  uint8_t auto_cache = 1;
  err = nvs_get_u8(nvs_handle, "auto_cache", &auto_cache);
  ESP_LOGI(TEST_TAG, "自动透明缓存: %s (状态: %s)",
           (err == ESP_OK) ? (auto_cache ? "启用" : "禁用") : "未设置",
           esp_err_to_name(err));

  // 测试3: 检查模板数量
  int32_t m_count = 0;
  err = nvs_get_i32(nvs_handle, "m_count", &m_count);
  ESP_LOGI(TEST_TAG, "m_count键: %d (状态: %s)",
           (err == ESP_OK) ? (int)m_count : -1, esp_err_to_name(err));

  // 测试4: 检查第一个模板的配置
  if (err == ESP_OK && m_count > 0) {
    // 检查启用状态
    uint8_t enabled = 0;
    esp_err_t en_err = nvs_get_u8(nvs_handle, "m0_en", &enabled);
    ESP_LOGI(TEST_TAG, "模板0启用状态: %s (状态: %s)",
             (en_err == ESP_OK) ? (enabled ? "启用" : "禁用") : "未设置",
             esp_err_to_name(en_err));

    // 检查从机地址
    char slave_addr[8] = {0};
    size_t len = sizeof(slave_addr);
    esp_err_t addr_err = nvs_get_str(nvs_handle, "m0s_addr", slave_addr, &len);
    ESP_LOGI(TEST_TAG, "模板0从机地址: %s (状态: %s)",
             (addr_err == ESP_OK) ? slave_addr : "未设置",
             esp_err_to_name(addr_err));
  }

  // 测试5: 检查任务状态
  ESP_LOGI(TEST_TAG, "轮询任务句柄: %s",
           (modbus_cache_task_handle != NULL) ? "有效" : "无效");
  ESP_LOGI(TEST_TAG, "虚拟从机任务句柄: %s",
           (virtual_slave_task_handle != NULL) ? "有效" : "无效");

  nvs_close(nvs_handle);
  ESP_LOGI(TEST_TAG, "=== 测试完成 ===");
}

//

// ==================== 缓存系统清理和重新初始化 ====================

/**
 * @brief 清理当前缓存系统并重新初始化
 * 用于工作模式切换时确保缓存系统与新配置一致
 * 由于每次切换工作模式必会重启系统，所以这个函数不被需要。
 */
// void cleanup_and_reinit_modbus_cache(void) {
//   static const char *CLEANUP_TAG = "CACHE_CLEANUP";
//   ESP_LOGI(CLEANUP_TAG, "=== 开始清理并重新初始化缓存系统 ===");
//   // 已弃用：工作模式切换会重启，不再需要在线重置
// }

static int get_ch5_timeout_override(void) __attribute__((unused));
static int get_ch5_timeout_override(void) {
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    return -1;
  }

  // 优先尝试字符串键 ch5_timeout
  size_t len = 0;
  if (nvs_get_str(nvs_handle, "ch5_timeout", NULL, &len) == ESP_OK && len > 0) {
    char *buf = (char *)malloc(len);
    if (buf) {
      if (nvs_get_str(nvs_handle, "ch5_timeout", buf, &len) == ESP_OK) {
        int v = atoi(buf);
        free(buf);
        nvs_close(nvs_handle);
        return v;
      }
      free(buf);
    }
  }

  // 尝试整数键 ch5_timeout
  int32_t v32 = 0;
  if (nvs_get_i32(nvs_handle, "ch5_timeout", &v32) == ESP_OK) {
    nvs_close(nvs_handle);
    return (int)v32;
  }

  // 兼容备用键 reply_timeout_5
  len = 0;
  if (nvs_get_str(nvs_handle, "reply_timeout_5", NULL, &len) == ESP_OK &&
      len > 0) {
    char *buf = (char *)malloc(len);
    if (buf) {
      if (nvs_get_str(nvs_handle, "reply_timeout_5", buf, &len) == ESP_OK) {
        int v = atoi(buf);
        free(buf);
        nvs_close(nvs_handle);
        return v;
      }
      free(buf);
    }
  }

  // 兜底使用统一回复超时（如存在）
  len = 0;
  if (nvs_get_str(nvs_handle, "unified_reply_timeout", NULL, &len) == ESP_OK &&
      len > 0) {
    char *buf = (char *)malloc(len);
    if (buf) {
      if (nvs_get_str(nvs_handle, "unified_reply_timeout", buf, &len) ==
          ESP_OK) {
        int v = atoi(buf);
        free(buf);
        nvs_close(nvs_handle);
        return v;
      }
      free(buf);
    }
  }

  nvs_close(nvs_handle);
  return -1;
}
