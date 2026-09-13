#include "sx_modbus.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"
#include "sx_utils.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>


static const char *TAG = "MODBUS";
static TaskHandle_t modbus_task_handle = NULL;
static volatile bool modbus_stop_requested = false;

static inline void modbus_delay_with_wdt(int delay_ms) {
  while (delay_ms > 0) {
    if (modbus_stop_requested) {
      return;
    }
    int chunk = (delay_ms > 500) ? 500 : delay_ms;
    vTaskDelay(pdMS_TO_TICKS(chunk));
    esp_task_wdt_reset();
    delay_ms -= chunk;
  }
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

void modbus_poll_task(void *arg) {
  // 检查参数有效性
  if (arg == NULL) {
    ESP_LOGE(TAG, "无效的任务参数");
    vTaskDelete(NULL);
    return;
  }

  ModbusTaskConfig *config = (ModbusTaskConfig *)arg;
  if (config->items == NULL || config->items_count <= 0) {
    ESP_LOGE(TAG, "无效的 Modbus 配置");
    // 清理传入的配置内存
    if (config->items) free(config->items);
    free(config);
    vTaskDelete(NULL);
    return;
  }
  
  ESP_LOGI(TAG, "Modbus轮询任务开始，配置项数量: %d", config->items_count);
  TickType_t next_poll_time;
  static const char *TAG = "MODBUS_POLL";

  // 从 NVS 中读取 poll_time
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("nvs_namespace", NVS_READONLY, &nvs_handle);
  int poll_time = 30000; // 默认值 30秒（毫秒）

  if (err == ESP_OK) {
    char poll_time_str[32];
    size_t len = sizeof(poll_time_str);
    if (nvs_get_str(nvs_handle, "poll_time", poll_time_str, &len) == ESP_OK) {
      poll_time = atoi(poll_time_str);
    }
    nvs_close(nvs_handle);
  }

    // 计算所有启用模板的总时间（接收超时时间 + 间隔时间）
  int total_template_time = 0;
  int enabled_count = 0;
  for (int i = 0; i < config->items_count; i++) {
    ModbusItemConfig *item = &config->items[i];
    if (item->enabled) {
      int timeout = atoi(item->timeout);
      int interval_time = atoi(item->interval_time);
      int template_total_time = timeout + interval_time;
      total_template_time += template_total_time;
      enabled_count++;
      ESP_LOGI(TAG, "模板 %d: 超时=%dms, 间隔=%dms, 总计=%dms",
               i+1, timeout, interval_time, template_total_time);
    }
  }

  // 验证轮询间隔时间是否足够
  if (total_template_time > 0 && enabled_count > 0) {
    int min_poll_time = total_template_time + 1000; // 加1秒缓冲
    ESP_LOGI(TAG, "总共%d个启用模板，总执行时间=%dms，最小轮询时间=%dms",
             enabled_count, total_template_time, min_poll_time);

    if (poll_time < min_poll_time) {
      ESP_LOGW(TAG, "轮询间隔时间 %d ms 小于所需的最小时间 %d ms，自动调整", poll_time, min_poll_time);
      poll_time = min_poll_time;

      // 将调整后的值保存回NVS
      nvs_handle_t nvs_write_handle;
      if (nvs_open("nvs_namespace", NVS_READWRITE, &nvs_write_handle) == ESP_OK) {
        char adjusted_poll_time_str[32];
        snprintf(adjusted_poll_time_str, sizeof(adjusted_poll_time_str), "%d", poll_time);
        nvs_set_str(nvs_write_handle, "poll_time", adjusted_poll_time_str);
        nvs_commit(nvs_write_handle);
        nvs_close(nvs_write_handle);
        ESP_LOGI(TAG, "轮询间隔时间已自动调整为 %d ms (%.1f 秒)", poll_time, poll_time / 1000.0);
      }
    }
  }

  ESP_LOGI(TAG, "轮询间隔时间设置为 %d ms (%.1f 秒)", poll_time, poll_time / 1000.0);

  modbus_stop_requested = false;

  // 注册当前任务到看门狗
  esp_err_t wdt_err = esp_task_wdt_add(NULL);
  if (wdt_err == ESP_OK) {
    ESP_LOGI(TAG, "Modbus任务已注册到看门狗");
  } else {
    ESP_LOGW(TAG, "Modbus任务看门狗注册失败: %s", esp_err_to_name(wdt_err));
  }

  next_poll_time = xTaskGetTickCount();

  while (!modbus_stop_requested) {
    // 喂狗，防止看门狗重启
    esp_task_wdt_reset();
    
    // 检查内存状况，防止内存不足导致死机
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < 8192) { // 小于8KB时警告
      ESP_LOGW(TAG, "内存不足警告: 剩余堆内存 %zu bytes", free_heap);
      if (free_heap < 2048) { // 小于2KB时停止任务
        ESP_LOGE(TAG, "内存严重不足，停止Modbus任务以防死机");
        break;
      }
    }
    
    TickType_t cycle_start_time = xTaskGetTickCount();
    ESP_LOGI(TAG, "开始轮询周期，共有 %d 个命令，剩余内存: %zu bytes", 
             config->items_count, free_heap);

    for (int i = 0; i < config->items_count && !modbus_stop_requested; i++) {
      ModbusItemConfig *item = &config->items[i];

      // 验证配置项有效性
      if (item == NULL) {
        ESP_LOGE(TAG, "配置项 %d 为空，跳过", i + 1);
        continue;
      }

      if (!item->enabled) {
        ESP_LOGI(TAG, "命令 %d 被禁用，跳过", i + 1);
        continue;
      }
      
      // 在每个命令处理前重置看门狗（防止单个命令执行时间过长）
      esp_task_wdt_reset();

      // 打印数据格式
      ESP_LOGI(TAG, "命令 %d 数据格式: %s", i + 1, item->data_format);

      // 设置当前处理的模板索引，并打印日志
      current_modbus_template_target = i;
      ESP_LOGI(TAG, "处理命令 %d (索引: %d)", i + 1,
               current_modbus_template_target);

      // 在重新配置串口前停止接收任务
      stop_rx_task();
      vTaskDelay(pdMS_TO_TICKS(100));
      esp_task_wdt_reset();

      // 验证串口参数有效性
      int baud_rate = atoi(item->baud_rate);
      int data_bit = atoi(item->data_bit);
      float stop_bit = atof(item->stop_bit);
      
      if (baud_rate <= 0 || data_bit < 5 || data_bit > 8 || stop_bit < 1.0) {
        ESP_LOGE(TAG, "无效的串口参数 %d: baud=%d, data=%d, stop=%.1f", 
                 i + 1, baud_rate, data_bit, stop_bit);
        continue;
      }

      // 更新串口参数
      uart_parity_t parity = UART_PARITY_DISABLE;
      if (strcmp(item->check_bit, "Odd") == 0) {
        parity = UART_PARITY_ODD;
      } else if (strcmp(item->check_bit, "Even") == 0) {
        parity = UART_PARITY_EVEN;
      }

      // 重新配置串口（添加错误处理）
      int interval_time_raw = atoi(item->interval_time);
      if (interval_time_raw <= 0) {
        interval_time_raw = 100;
      }
      int frame_time = interval_time_raw;
      if (frame_time < 10) {
        frame_time = 10;
      } else if (frame_time > 600000) {
        frame_time = 600000;
      }

      ESP_LOGI(TAG,
               "配置串口: baud=%d, data=%d, parity=%d, stop=%.1f, frame_time=%d",
               baud_rate, data_bit, parity, stop_bit, frame_time);

      uart_reinit(baud_rate, data_bit, parity, stop_bit, frame_time, 512, false);

      vTaskDelay(pdMS_TO_TICKS(50));
      esp_task_wdt_reset();
      rx_wait();  // 重新启动接收任务，否则无法接收响应
      vTaskDelay(pdMS_TO_TICKS(50));
      esp_task_wdt_reset();

      // 构建Modbus请求，验证参数范围
      int slave_addr = atoi(item->slave_addr);
      int function_code = atoi(item->function_code);
      int reg_addr = atoi(item->register_addr);
      int reg_num = atoi(item->register_num);
      
      // 验证Modbus参数
      if (slave_addr < 1 || slave_addr > 247) {
        ESP_LOGE(TAG, "无效的从站地址 %d，跳过命令 %d", slave_addr, i + 1);
        continue;
      }
      if (function_code < 1 || function_code > 6) {
        ESP_LOGE(TAG, "无效的功能码 %d，跳过命令 %d", function_code, i + 1);
        continue;
      }
      if (reg_addr < 0 || reg_addr > 65535 || reg_num < 1 || reg_num > 125) {
        ESP_LOGE(TAG, "无效的寄存器参数 addr=%d num=%d，跳过命令 %d", 
                 reg_addr, reg_num, i + 1);
        continue;
      }
      
      uint8_t request[8];
      request[0] = (uint8_t)slave_addr;
      request[1] = (uint8_t)function_code;
      request[2] = (reg_addr >> 8) & 0xFF;
      request[3] = reg_addr & 0xFF;
      request[4] = (reg_num >> 8) & 0xFF;
      request[5] = reg_num & 0xFF;

      uint16_t crc = calculate_crc(request, 6);
      request[6] = crc & 0xFF;
      request[7] = (crc >> 8) & 0xFF;

      ESP_LOGI(TAG, "发送Modbus请求 %d: slave=%d, func=%d, addr=%d, num=%d", 
               i + 1, slave_addr, function_code, reg_addr, reg_num);
      tx_tasks(request, sizeof(request));

      // 等待响应超时时间（验证范围）
      int timeout = atoi(item->timeout);
      if (timeout < 100 || timeout > 10000) {
        ESP_LOGW(TAG, "超时时间 %d ms 超出范围，调整为 1000 ms", timeout);
        timeout = 1000;
      }

      // 在等待期间分批延迟，并定期喂狗
      int delay_chunks = (timeout + 999) / 1000;  // 向上取整到秒数
      int delay_per_chunk = timeout / delay_chunks;
      for (int d = 0; d < delay_chunks && !modbus_stop_requested; d++) {
        modbus_delay_with_wdt(delay_per_chunk);
      }

      // 在进入下一个命令之前，确保当前响应已经被处理
      modbus_delay_with_wdt(100); // 添加额外延时确保响应处理完成

      // 计算实际间隔时间 = 设置的间隔时间 - (已经使用的时间)
      int interval_time = interval_time_raw;
      int used_time = timeout + 100; // 已经等待的时间(超时+额外延时)
      int actual_interval = interval_time > used_time ? interval_time - used_time : 0;

      // 等待实际间隔时间，并在长延迟期间喂狗
      if (actual_interval > 0) {
        int remaining = actual_interval;
        modbus_delay_with_wdt(remaining);
      }
    }

    // 计算本次循环实际耗时
    TickType_t current_time = xTaskGetTickCount();
    int elapsed_ms = (current_time - cycle_start_time) * portTICK_PERIOD_MS;

    ESP_LOGI(TAG, "循环完成，耗时 %d 毫秒", elapsed_ms);

    // 计算剩余时间，并在等待期间定期喂狗
    int remaining_time = poll_time - elapsed_ms;
    if (remaining_time > 0) {
      // 每秒喂一次狗
      modbus_delay_with_wdt(remaining_time);
    }

    next_poll_time += pdMS_TO_TICKS(poll_time);
  }
  
  // 正常情况下不会到达这里，但为了安全起见添加清理代码
  ESP_LOGW(TAG, "Modbus轮询任务退出，清理资源");
  
  // 取消看门狗注册
  esp_task_wdt_delete(NULL);
  
  if (config) {
    if (config->items) free(config->items);
    free(config);
  }

  modbus_stop_requested = false;
  modbus_task_handle = NULL;
  vTaskDelete(NULL);
}

// 停止Modbus任务（安全方式）
void stop_modbus_tasks(void) {
  if (modbus_task_handle == NULL) {
    return;
  }

  ESP_LOGI(TAG, "正在安全停止Modbus任务...");
  modbus_stop_requested = true;

  // 等待任务自行退出
  const int wait_slice_ms = 50;
  for (int i = 0; i < 100; i++) { // 最多等待5秒
    if (modbus_task_handle == NULL) {
      ESP_LOGI(TAG, "Modbus任务已安全停止");
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(wait_slice_ms));
  }

  ESP_LOGW(TAG, "Modbus任务未在超时时间内退出，强制删除");
  vTaskDelete(modbus_task_handle);
  modbus_task_handle = NULL;
  modbus_stop_requested = false;
}

// 启动Modbus任务
esp_err_t start_modbus_task(ModbusTaskConfig *config) {
  if (config == NULL || config->items == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  // 确保之前的任务已经完全停止
  if (modbus_task_handle != NULL) {
    ESP_LOGW(TAG, "Modbus任务正在运行，先停止现有任务");
    stop_modbus_tasks();
  }

  modbus_stop_requested = false;

  // 创建任务（增加栈大小以防止栈溢出）
  BaseType_t ret =
      xTaskCreate(modbus_poll_task, "modbus_task", 8192, (void *)config,
                  configMAX_PRIORITIES - 3, &modbus_task_handle);

  if (ret != pdPASS) {
    ESP_LOGE(TAG, "Failed to create Modbus task, free heap: %" PRIu32 " bytes", 
             esp_get_free_heap_size());
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Modbus任务启动成功，句柄: %p", modbus_task_handle);
  return ESP_OK;
}

esp_err_t init_modbus_auto_polling(void) {
  static const char *TAG = "MODBUS_AUTO_INIT";
  ESP_LOGI(TAG, "初始化Modbus自动轮询");

  // 初始化NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 打开NVS存储
  nvs_handle_t storage_handle;
  ret = nvs_open("nvs_namespace", NVS_READONLY, &storage_handle);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "无法打开NVS存储: %s", esp_err_to_name(ret));
    return ret;
  }

  // 获取工作模式
  char work_mode[32] = {0};
  size_t len = sizeof(work_mode);
  ret = nvs_get_str(storage_handle, "w_mode", work_mode, &len);
  if (ret != ESP_OK) {
    ESP_LOGW(TAG, "获取工作模式失败: %s", esp_err_to_name(ret));
    nvs_close(storage_handle);
    return ret;
  }

  // 检查是否为Modbus-RTU模式
  if (strcmp(work_mode, "modbus_rtu") != 0) {
    ESP_LOGI(TAG, "当前工作模式不是Modbus-RTU，无需启动轮询");
    nvs_close(storage_handle);
    return ESP_OK;
  }

  // 获取模板数量
  int items_count = get_modbus_items_count(storage_handle);
  if (items_count <= 0) {
    ESP_LOGW(TAG, "未找到有效的Modbus模板配置");
    nvs_close(storage_handle);
    return ESP_ERR_NOT_FOUND;
  }

  ESP_LOGI(TAG, "找到 %d 个Modbus模板配置", items_count);

  // 创建模板配置结构
  ModbusTaskConfig *config =
      (ModbusTaskConfig *)malloc(sizeof(ModbusTaskConfig));
  if (config == NULL) {
    ESP_LOGE(TAG, "无法为模板配置分配内存");
    nvs_close(storage_handle);
    return ESP_ERR_NO_MEM;
  }

  config->items_count = items_count;
  config->items =
      (ModbusItemConfig *)malloc(items_count * sizeof(ModbusItemConfig));
  if (config->items == NULL) {
    ESP_LOGE(TAG, "无法为模板项分配内存");
    free(config);
    nvs_close(storage_handle);
    return ESP_ERR_NO_MEM;
  }

  // 读取所有模板配置
  bool config_valid = true;
  for (int i = 0; i < items_count; i++) {
    char key[32];
    size_t size;

    // 读取启用状态
    snprintf(key, sizeof(key), "m%d_en", i);
    uint8_t enabled = 0;
    if (nvs_get_u8(storage_handle, key, &enabled) != ESP_OK) {
      ESP_LOGW(TAG, "无法读取模板 %d 的启用状态，默认为启用", i);
      config->items[i].enabled = true; // 默认启用
    } else {
      config->items[i].enabled = (enabled != 0);
    }

    // 保存配置到NVS和任务配置
    const struct {
      const char *nvs_key;
      char *config_field;
      size_t field_size;
    } fields[] = {
        {"s_addr", config->items[i].slave_addr,
         sizeof(config->items[i].slave_addr)},
        {"f_code", config->items[i].function_code,
         sizeof(config->items[i].function_code)},
        {"r_addr", config->items[i].register_addr,
         sizeof(config->items[i].register_addr)},
        {"r_num", config->items[i].register_num,
         sizeof(config->items[i].register_num)},
        {"timeout", config->items[i].timeout, sizeof(config->items[i].timeout)},
        {"d_fmt", config->items[i].data_format,
         sizeof(config->items[i].data_format)},
        {"i_time", config->items[i].interval_time,
         sizeof(config->items[i].interval_time)},
        {"r_fmt", config->items[i].report_format,
         sizeof(config->items[i].report_format)},
        {"baud_rate", config->items[i].baud_rate,
         sizeof(config->items[i].baud_rate)},
        {"data_bit", config->items[i].data_bit,
         sizeof(config->items[i].data_bit)},
        {"stop_bit", config->items[i].stop_bit,
         sizeof(config->items[i].stop_bit)},
        {"check_bit", config->items[i].check_bit,
         sizeof(config->items[i].check_bit)},
    };

    for (int j = 0; j < sizeof(fields) / sizeof(fields[0]); j++) {
      snprintf(key, sizeof(key), "m%d%s", i, fields[j].nvs_key);
      size = fields[j].field_size;

      esp_err_t field_ret =
          nvs_get_str(storage_handle, key, fields[j].config_field, &size);
      if (field_ret != ESP_OK) {
        ESP_LOGW(TAG, "无法读取模板 %d 的 %s: %s", i, fields[j].nvs_key,
                 esp_err_to_name(field_ret));
        config_valid = false;
        break;
      }
    }

    if (!config_valid) {
      break;
    }
  }

  nvs_close(storage_handle);

  if (!config_valid) {
    ESP_LOGE(TAG, "模板配置无效，无法启动轮询");
    free(config->items);
    free(config);
    return ESP_ERR_INVALID_STATE;
  }

  // 启动Modbus轮询任务
  ESP_LOGI(TAG, "启动Modbus轮询任务");
  esp_err_t start_result = start_modbus_task(config);
  if (start_result != ESP_OK) {
    ESP_LOGE(TAG, "启动Modbus轮询任务失败: %s", esp_err_to_name(start_result));
    free(config->items);
    free(config);
    return start_result;
  }

  return ESP_OK;
}
