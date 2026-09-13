#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include <string.h>

#include "include/sx_async_uart.h"
#include "include/sx_master_slave.h"
#include "sx_utils.h"
#include <stddef.h>
#include <stdint.h>

static const char *TAG = "MASTER_SLAVE";
#define MASTER_SLAVE_TASK_PRIORITY 12
#define MASTER_SLAVE_TX_GAP_MS 1
#define MASTER_SLAVE_LOOP_DELAY_MS 5

static uint8_t *ch1buffer = NULL;
static uint8_t *ch2buffer = NULL;
static uint8_t *ch3buffer = NULL;
size_t ch1data_len;
size_t ch2data_len;
size_t ch3data_len;

static void *master_slave_psram_calloc(size_t count, size_t size) {
  void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (ptr == NULL) {
    ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
  }
  return ptr;
}

static bool ensure_master_slave_buffers(void) {
  if (ch1buffer == NULL) {
    ch1buffer = master_slave_psram_calloc(1, 2048);
  }
  if (ch2buffer == NULL) {
    ch2buffer = master_slave_psram_calloc(1, 2048);
  }
  if (ch3buffer == NULL) {
    ch3buffer = master_slave_psram_calloc(1, 2048);
  }
  return ch1buffer != NULL && ch2buffer != NULL && ch3buffer != NULL;
}

void sx_master_slave_init(void) {
  if (!ensure_master_slave_buffers()) {
    ESP_LOGE(TAG, "无法分配主从转发缓冲区");
    return;
  }
  create_app_task_psram(sx_task_master_slave, "sx_task_master_slave", 8192,
                        NULL, MASTER_SLAVE_TASK_PRIORITY, NULL,
                        SX_WORK_CORE_ID); // 栈放入PSRAM
}

void sx_task_master_slave(void *pvParameter) {
  ESP_LOGI(TAG, "一主多从任务启动");
  
  // 关键修复：检查当前工作模式，防止多任务冲突
  char work_mode[32] = {0};
  nvs_handle_t nvs_check;
  if (nvs_open("storage", NVS_READONLY, &nvs_check) == ESP_OK) {
    size_t len = sizeof(work_mode);
    nvs_get_str(nvs_check, "w_mode", work_mode, &len);
    nvs_close(nvs_check);
    
    if (strcmp(work_mode, "master_slave") != 0) {
      ESP_LOGW(TAG, "⚠️  当前工作模式不是master_slave（当前：%s），任务退出防止冲突", work_mode);
      delete_self_app_task_with_caps();
      return;
    }
  }
  
  ESP_LOGI(TAG, "转发规则: CH1(主站) <-> CH2,CH3(从站)");

  while (1) {
    // 检查CH1(主站总线)是否有数据
    ch1data_len = get_channel_data(1, ch1buffer, sizeof(ch1buffer), NULL);
    if (ch1data_len > 0 && ch1data_len <= sizeof(ch1buffer)) {
      ESP_LOGD(TAG, "CH1(主站)收到数据，长度: %zu，转发到CH2,CH3", ch1data_len);
      // 转发到CH2和CH3(从站总线)
      tx_tasks_to_channel(ch1buffer, ch1data_len, 2);
      vTaskDelay(sx_ms_to_ticks(MASTER_SLAVE_TX_GAP_MS));
      tx_tasks_to_channel(ch1buffer, ch1data_len, 3);
      clear_channel_data(1);
    }

    // 检查CH2(从站总线)是否有数据
    ch2data_len = get_channel_data(2, ch2buffer, sizeof(ch2buffer), NULL);
    if (ch2data_len > 0 && ch2data_len <= sizeof(ch2buffer)) {
      ESP_LOGD(TAG, "CH2(从站)收到数据，长度: %zu，转发到CH1", ch2data_len);
      // 转发到CH1(主站总线)
      tx_tasks_to_channel(ch2buffer, ch2data_len, 1);
      clear_channel_data(2);
    }

    // 检查CH3(从站总线)是否有数据
    ch3data_len = get_channel_data(3, ch3buffer, sizeof(ch3buffer), NULL);
    if (ch3data_len > 0 && ch3data_len <= sizeof(ch3buffer)) {
      ESP_LOGD(TAG, "CH3(从站)收到数据，长度: %zu，转发到CH1", ch3data_len);
      // 转发到CH1(主站总线)
      tx_tasks_to_channel(ch3buffer, ch3data_len, 1);
      clear_channel_data(3);
    }

    // 任务延时，5ms平衡响应性和CPU占用
    vTaskDelay(sx_ms_to_ticks(MASTER_SLAVE_LOOP_DELAY_MS));
  }
  
  // 任务退出（通常不会执行到这里）
  ESP_LOGI(TAG, "主从任务退出");
}
