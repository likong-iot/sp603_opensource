#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/mpu_wrappers.h"
#include "freertos/task.h"
#include "hal/uart_types.h"
#include "string.h"
#include "stdbool.h"
#include "stdio.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_gpio.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include <stdio.h>
#include <stdlib.h>
#include "sx_async_uart.h"
#include <strings.h>  // 添加strcasecmp支持
#include "sx_time_manager.h"  // 统一时间管理

// static const int RX_BUF_SIZE = 8192;  // 扩展到8KB以支持大容量Modbus数据
uint8_t dataArray[] = {0};
// char global_hex_data_str[256];
// uint8_t *source_data = NULL;
// int lens = 0;
TaskHandle_t uartTaskHandle = NULL;

// 全局任务句柄，用于安全的任务管理
static TaskHandle_t uart0_rx_task_handle = NULL;
static TaskHandle_t uart1_rx_task_handle = NULL;
static TaskHandle_t uart2_rx_task_handle = NULL;

// 全局标志，指示是否正在进行UART重新初始化
static bool uart_reinit_in_progress = false;

// UART通道配置选项（逻辑绑定：CH1->UART2, CH2->UART0, CH3->UART1）
// 逻辑CH1使用原CH3的物理串口 - 面板口485-3
#define TXD_PIN_CH1 (GPIO_NUM_17)
#define RXD_PIN_CH1 (GPIO_NUM_16)
#define RTS_PIN_CH1 (GPIO_NUM_5)

// 逻辑CH2使用原CH1的物理串口 - 面板口485-1
#define TXD_PIN_CH2 (GPIO_NUM_43)
#define RXD_PIN_CH2 (GPIO_NUM_44)
#define RTS_PIN_CH2 (GPIO_NUM_4)

// 逻辑CH3使用原CH2的物理串口 - 面板口485-2
#define TXD_PIN_CH3 (GPIO_NUM_15)
#define RXD_PIN_CH3 (GPIO_NUM_7)
#define RTS_PIN_CH3 (GPIO_NUM_2)

// 当前使用的通道配置 (默认使用通道1)
#define TXD_PIN TXD_PIN_CH1
#define RXD_PIN RXD_PIN_CH1
#define ECHO_TEST_RTS RTS_PIN_CH1

#define TAG "ASYNCRS485"
#define BUF_SIZE (8192)  // 扩展到8KB以支持大容量Modbus数据

// 缓冲区池：运行时从PSRAM分配，避免占用内部DRAM BSS
#define BUFFER_POOL_SIZE 4  // 4个8KB缓冲区，支持多UART并发
static uint8_t *static_buffers[BUFFER_POOL_SIZE] = {0};
static bool buffer_in_use[BUFFER_POOL_SIZE] = {false};
static SemaphoreHandle_t buffer_pool_mutex = NULL;
static bool buffer_pool_initialized = false;

// 缓冲区池统计信息
static struct {
    uint32_t pool_allocations;   // 从池中分配的次数
    uint32_t malloc_allocations; // malloc分配的次数
    uint32_t pool_returns;       // 返回到池的次数
    uint32_t malloc_frees;       // free释放的次数
} buffer_stats = {0};

static bool ensure_buffer_pool_ready(void) {
    if (buffer_pool_mutex) {
        return true;
    }

    buffer_pool_mutex = xSemaphoreCreateMutex();
    if (!buffer_pool_mutex) {
        ESP_LOGE(TAG, "创建缓冲区池互斥量失败，后续将回退到动态分配");
        return false;
    }

    for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
        if (static_buffers[i] == NULL) {
            static_buffers[i] = heap_caps_calloc(1, BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (static_buffers[i] == NULL) {
                static_buffers[i] = heap_caps_calloc(1, BUF_SIZE, MALLOC_CAP_8BIT);
            }
            if (static_buffers[i] == NULL) {
                ESP_LOGE(TAG, "分配UART缓冲池 #%d 失败，后续将回退到动态分配", i);
                return false;
            }
        }
    }

    memset(buffer_in_use, 0, sizeof(buffer_in_use));
    buffer_pool_initialized = true;
    return true;
}

// 校验位解析通用函数
static uart_parity_t parse_parity_string(const char* parity_str) {
    if (parity_str == NULL) {
        ESP_LOGW(TAG, "校验位字符串为空，使用默认值(无校验)");
        return UART_PARITY_DISABLE;
    }
    
    // 支持前端发送的数字字符串格式（重要！）
    if (strcmp(parity_str, "0") == 0) {
        ESP_LOGD(TAG, "校验位配置: 无校验 (数字字符串 '0')");
        return UART_PARITY_DISABLE;
    } else if (strcmp(parity_str, "1") == 0) {
        ESP_LOGD(TAG, "校验位配置: 奇校验 (数字字符串 '1')");
        return UART_PARITY_ODD;
    } else if (strcmp(parity_str, "2") == 0) {
        ESP_LOGD(TAG, "校验位配置: 偶校验 (数字字符串 '2')");
        return UART_PARITY_EVEN;
    }
    
    // 未知格式，使用默认值
    ESP_LOGW(TAG, "未知的校验位配置 '%s'，使用默认值(无校验)", parity_str);
    return UART_PARITY_DISABLE;
}

// 校验位配置调试函数
static const char* parity_to_string(uart_parity_t parity) {
    switch (parity) {
        case UART_PARITY_DISABLE:
            return "无校验";
        case UART_PARITY_EVEN:
            return "偶校验";
        case UART_PARITY_ODD:
            return "奇校验";
        default:
            return "未知";
    }
}

// 缓冲区池管理函数
static uint8_t* allocate_buffer(void) {
    if (!ensure_buffer_pool_ready()) {
        buffer_stats.malloc_allocations++;
        ESP_LOGW(TAG, "缓冲区池不可用，使用动态分配 (malloc:%u)", buffer_stats.malloc_allocations);
        return malloc(BUF_SIZE);
    }

    if (xSemaphoreTake(buffer_pool_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
            if (!buffer_in_use[i]) {
                buffer_in_use[i] = true;
                buffer_stats.pool_allocations++;
                xSemaphoreGive(buffer_pool_mutex);
                ESP_LOGD(TAG, "从缓冲区池分配缓冲区 #%d", i);
                return static_buffers[i];
            }
        }
        xSemaphoreGive(buffer_pool_mutex);
    } else {
        ESP_LOGW(TAG, "获取缓冲区池互斥量超时，回退到动态分配");
    }

    buffer_stats.malloc_allocations++;
    uint8_t *fallback = heap_caps_malloc(BUF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (fallback == NULL) {
        fallback = malloc(BUF_SIZE);
    }
    if (!fallback) {
        ESP_LOGE(TAG, "动态分配缓冲区失败（池:%u, malloc:%u）", buffer_stats.pool_allocations, buffer_stats.malloc_allocations);
    } else {
        ESP_LOGW(TAG, "缓冲区池已满，使用动态分配（池:%u, malloc:%u）", buffer_stats.pool_allocations, buffer_stats.malloc_allocations);
    }
    return fallback;
}

static void free_buffer(uint8_t* buffer) {
    if (!buffer) {
        return;
    }

    if (!buffer_pool_mutex || !buffer_pool_initialized) {
        free(buffer);
        return;
    }

    bool returned_to_pool = false;
    for (int attempt = 0; attempt < 3 && !returned_to_pool; attempt++) {
        if (xSemaphoreTake(buffer_pool_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            for (int i = 0; i < BUFFER_POOL_SIZE; i++) {
                if (static_buffers[i] == buffer) {
                    buffer_in_use[i] = false;
                    buffer_stats.pool_returns++;
                    returned_to_pool = true;
                    ESP_LOGD(TAG, "缓冲区 #%d 返回到池中", i);
                    break;
                }
            }
            xSemaphoreGive(buffer_pool_mutex);
        } else {
            ESP_LOGW(TAG, "尝试获取缓冲区池互斥量失败 (attempt %d)", attempt + 1);
        }

        if (!returned_to_pool && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
            vTaskDelay(sx_ms_to_ticks(1));
        }
    }

    if (!returned_to_pool) {
        buffer_stats.malloc_frees++;
        free(buffer);
    }
}

// RTS for RS485 Half-Duplex Mode manages DE/~RE
// #define ECHO_TEST_RTS 2

// CTS is not used in RS485 Half-Duplex Mode
#define ECHO_TEST_CTS (UART_PIN_NO_CHANGE)

#define ECHO_READ_TOUT (3) // 3.5T * 8 = 28 ticks, TOUT=3 -> ~24..33 ticks
#define PACKET_READ_TICS (100 / portTICK_PERIOD_MS)

// char device_response[512];
uint8_t *uart_response = NULL;  // 扩展到2KB以支持大容量Modbus数据
uint8_t *uart_tx_data = NULL;   // 扩展到2KB以支持大容量Modbus数据
size_t tx_data_len = 0;
uart_timestamps_t uart_timestamps = {0};

// 每个通道的独立缓冲区
#define CHANNEL_FRAME_MAX_LEN 2048
#define CHANNEL_QUEUE_DEPTH 8

typedef struct {
    uint8_t data[CHANNEL_FRAME_MAX_LEN];
    size_t length;
    uint64_t timestamp;
} channel_frame_t;

typedef struct {
    channel_frame_t frames[CHANNEL_QUEUE_DEPTH];
    uint8_t head;
    uint8_t count;
    uint32_t dropped;
} channel_buffer_t;

static channel_buffer_t *channel_buffers = NULL; // CH1, CH2, CH3对应索引0,1,2
static portMUX_TYPE channel_spinlock = portMUX_INITIALIZER_UNLOCKED;

static void *psram_calloc_or_heap(size_t count, size_t size) {
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static bool ensure_uart_runtime_buffers_ready(void) {
    if (uart_response == NULL) {
        uart_response = psram_calloc_or_heap(1, UART_TRACE_BUFFER_SIZE);
        if (uart_response == NULL) {
            ESP_LOGE(TAG, "分配UART接收显示缓冲失败");
            return false;
        }
    }
    if (uart_tx_data == NULL) {
        uart_tx_data = psram_calloc_or_heap(1, UART_TRACE_BUFFER_SIZE);
        if (uart_tx_data == NULL) {
            ESP_LOGE(TAG, "分配UART发送显示缓冲失败");
            return false;
        }
    }
    if (channel_buffers == NULL) {
        channel_buffers = psram_calloc_or_heap(3, sizeof(channel_buffer_t));
        if (channel_buffers == NULL) {
            ESP_LOGE(TAG, "分配UART通道队列缓冲失败");
            return false;
        }
    }
    return true;
}

static inline uint8_t channel_queue_tail(const channel_buffer_t *queue) {
    return (uint8_t)((queue->head + queue->count) % CHANNEL_QUEUE_DEPTH);
}

static void channel_queue_push_locked(channel_buffer_t *queue,
                                      const uint8_t *data,
                                      size_t length,
                                      uint64_t timestamp) {
    if (queue->count >= CHANNEL_QUEUE_DEPTH) {
        queue->frames[queue->head].length = 0;
        queue->frames[queue->head].timestamp = 0;
        queue->head = (uint8_t)((queue->head + 1U) % CHANNEL_QUEUE_DEPTH);
        queue->count = CHANNEL_QUEUE_DEPTH - 1U;
        queue->dropped++;
    }

    uint8_t tail = channel_queue_tail(queue);
    memcpy(queue->frames[tail].data, data, length);
    queue->frames[tail].length = length;
    queue->frames[tail].timestamp = timestamp;
    queue->count++;
}

int current_modbus_template_target = 0;
#define RING_BUFFER_SIZE 20480  // 扩展环形缓冲区到20KB
void stop_rx_task();

// 通过通道获取缓冲区数据的函数
int get_channel_data(int channel, uint8_t* buffer, size_t buffer_size, uint64_t* timestamp);
int pop_channel_data(int channel, uint8_t* buffer, size_t buffer_size, uint64_t* timestamp);
void clear_channel_data(int channel);


// uint8_t uart_response[BUF_SIZE] = {0};
size_t response_len = 0;
portMUX_TYPE uart_spinlock = portMUX_INITIALIZER_UNLOCKED;

// UART通道选择函数
void select_uart_channel(int channel) {
    switch(channel) {
        case 1:
            #undef TXD_PIN
            #undef RXD_PIN
            #undef ECHO_TEST_RTS
            #define TXD_PIN TXD_PIN_CH1
            #define RXD_PIN RXD_PIN_CH1
            #define ECHO_TEST_RTS RTS_PIN_CH1
            break;
        case 2:
            #undef TXD_PIN
            #undef RXD_PIN
            #undef ECHO_TEST_RTS
            #define TXD_PIN TXD_PIN_CH2
            #define RXD_PIN RXD_PIN_CH2
            #define ECHO_TEST_RTS RTS_PIN_CH2
            break;
        case 3:
            #undef TXD_PIN
            #undef RXD_PIN
            #undef ECHO_TEST_RTS
            #define TXD_PIN TXD_PIN_CH3
            #define RXD_PIN RXD_PIN_CH3
            #define ECHO_TEST_RTS RTS_PIN_CH3
            break;
        default:
            ESP_LOGE(TAG, "Invalid UART channel: %d", channel);
            break;
    }
}

// UART LED初始化函数
void uart_led_init(void) {
    
    // 配置所有通道LED为输出模式
    gpio_config_t led_config = {
        .pin_bit_mask = (1ULL << CH1_TX_LED) | (1ULL << CH1_RX_LED) |
                        (1ULL << CH2_TX_LED) | (1ULL << CH2_RX_LED) |
                        (1ULL << CH3_TX_LED) | (1ULL << CH3_RX_LED),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_config);

    // 初始化时所有LED熄灭
    gpio_set_level(CH1_TX_LED, 0);
    gpio_set_level(CH1_RX_LED, 0);
    gpio_set_level(CH2_TX_LED, 0);
    gpio_set_level(CH2_RX_LED, 0);
    gpio_set_level(CH3_TX_LED, 0);
    gpio_set_level(CH3_RX_LED, 0);
}

// 通道发送LED控制函数（channel: 1=CH1, 2=CH2, 3=CH3）
void uart_tx_led_on(int channel) {
    switch(channel) {
        case 1:
            gpio_set_level(CH1_TX_LED, 1);
            break;
        case 2:
            gpio_set_level(CH2_TX_LED, 1);
            break;
        case 3:
            gpio_set_level(CH3_TX_LED, 1);
            break;
        default:
            break;
    }
}

void uart_tx_led_off(int channel) {
    switch(channel) {
        case 1:
            gpio_set_level(CH1_TX_LED, 0);
            break;
        case 2:
            gpio_set_level(CH2_TX_LED, 0);
            break;
        case 3:
            gpio_set_level(CH3_TX_LED, 0);
            break;
        default:
            break;
    }
}

// 通道接收LED控制函数（channel: 1=CH1, 2=CH2, 3=CH3）
void uart_rx_led_on(int channel) {
    switch(channel) {
        case 1:
            gpio_set_level(CH1_RX_LED, 1);
            break;
        case 2:
            gpio_set_level(CH2_RX_LED, 1);
            break;
        case 3:
            gpio_set_level(CH3_RX_LED, 1);
            break;
        default:
            break;
    }
}

void uart_rx_led_off(int channel) {
    switch(channel) {
        case 1:
            gpio_set_level(CH1_RX_LED, 0);
            break;
        case 2:
            gpio_set_level(CH2_RX_LED, 0);
            break;
        case 3:
            gpio_set_level(CH3_RX_LED, 0);
            break;
        default:
            break;
    }
}

// 独立的UART配置函数
void configure_uart0(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len) {
  ESP_LOGI(TAG, "Configuring UART0 (Channel 2 -> UART0)");
  ESP_LOGI(TAG, "Input params - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d",
           baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
  ESP_LOGI(TAG, "TX: GPIO%d, RX: GPIO%d, RTS: GPIO%d", TXD_PIN_CH2, RXD_PIN_CH2, RTS_PIN_CH2);

  // 参数验证和转换（支持整数输入）
  uart_parity_t uart_parity;
  uart_word_length_t uart_data_bits = data_bits;
  uart_stop_bits_t uart_stop_bits = stop_bits;
  
  // 校验位处理：整数1需要转换为UART_PARITY_ODD(3)
  if ((int)parity == 1) {
    ESP_LOGI(TAG, "UART0校验位转换(整数1): 奇校验");
    uart_parity = UART_PARITY_ODD;
  } else {
    // 0,2,3都是有效的枚举值，直接使用
    ESP_LOGI(TAG, "UART0校验位(枚举值%d): %s", (int)parity, parity_to_string(parity));
    uart_parity = parity;
  }
  
  if ((int)data_bits >= 5 && (int)data_bits <= 8) {
    if ((int)data_bits == 5) uart_data_bits = UART_DATA_5_BITS;
    else if ((int)data_bits == 6) uart_data_bits = UART_DATA_6_BITS;
    else if ((int)data_bits == 7) uart_data_bits = UART_DATA_7_BITS;
    else if ((int)data_bits == 8) uart_data_bits = UART_DATA_8_BITS;
  }
  
  // 停止位已在上面定义，直接使用
  
  uart_config_t uart_config = {
      .baud_rate = baudrate,
      .data_bits = uart_data_bits,
      .parity = uart_parity,
      .stop_bits = uart_stop_bits,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };

  // 使用固定的UART硬件超时时间（3.5个字符时间）
  // frame_time用于应用层包分界判断，不用于UART硬件超时
  int timeout_ticks = 3; // 固定3 ticks，约3.5个字符时间

  int rx_threshold = frame_len;
  if (rx_threshold < 1) rx_threshold = 1;
  if (rx_threshold > 63) rx_threshold = 63;

  int rx_buffer_size = 1024;  // 默认1KB，扩展基础大小
  if (frame_len > 256) rx_buffer_size = 2048;
  if (frame_len > 512) rx_buffer_size = 4096;
  if (frame_len > 1024) rx_buffer_size = 8192;
  if (frame_len > 2048) rx_buffer_size = 8192;

  ESP_LOGI(TAG, "UART0 配置 - frame_time: %d ms (包间隔判断), timeout_ticks: %d (UART硬件超时), RX buffer: %d, RX threshold: %d", 
           frame_time, timeout_ticks, rx_buffer_size, rx_threshold);

  // 安全地暂停所有UART接收任务（检查是否已在重新初始化中）
  bool should_manage_tasks = !uart_reinit_in_progress;
  if (should_manage_tasks) {
    suspend_all_uart_rx_tasks();
  }

  // 删除现有驱动
  uart_driver_delete(UART_NUM_0);
  
  // 等待一小段时间确保驱动完全释放
  vTaskDelay(pdMS_TO_TICKS(20));
  
  // 配置UART0
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_0, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_0, TXD_PIN_CH2, RXD_PIN_CH2, RTS_PIN_CH2, UART_PIN_NO_CHANGE));
  
  // 安装驱动，带错误处理和重试机制
  esp_err_t err = uart_driver_install(UART_NUM_0, rx_buffer_size, 0, 0, NULL, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UART0 driver install failed with error: %d", err);
    // 如果安装失败，尝试使用最小的缓冲区大小
    rx_buffer_size = 256;
    err = uart_driver_install(UART_NUM_0, rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "UART0 driver install retry failed");
      // 恢复任务后再返回（仅在需要时）
      if (should_manage_tasks) {
        resume_all_uart_rx_tasks();
      }
      return;
    }
    ESP_LOGW(TAG, "UART0 driver installed with reduced buffer size: %d", rx_buffer_size);
  }
  
  ESP_ERROR_CHECK(uart_set_mode(UART_NUM_0, UART_MODE_RS485_HALF_DUPLEX));
  ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_0, timeout_ticks));
  ESP_ERROR_CHECK(uart_set_rx_full_threshold(UART_NUM_0, rx_threshold));
  
  // 改参后清空硬件与高层缓冲，避免残留包被上层转发
  uart_flush(UART_NUM_0);
  clear_channel_data(2);
  
  // 安全地恢复所有UART接收任务（仅在需要时）
  if (should_manage_tasks) {
    resume_all_uart_rx_tasks();
  }
  
  ESP_LOGI(TAG, "UART0 configured successfully");
  
  // 更新运行时配置缓存
  channel_uart_config_t runtime_config = {
    .channel = 2,  // CH2 -> UART0
    .baudrate = baudrate,
    .data_bits = uart_data_bits,
    .parity = uart_parity,
    .stop_bits = uart_stop_bits,
    .frame_time = frame_time,  // 帧间隔时间(ms) - 用于确定Modbus帧包分界
    .frame_len = frame_len,
    .timeout = 1000  // Modbus协议超时时间(ms) - 不是UART硬件超时
  };
  set_current_runtime_config(2, &runtime_config);
}



void configure_uart1(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len) {
  ESP_LOGI(TAG, "Configuring UART1 (Channel 3 -> UART1)");
  ESP_LOGI(TAG, "Input params - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d",
           baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
  ESP_LOGI(TAG, "TX: GPIO%d, RX: GPIO%d, RTS: GPIO%d", TXD_PIN_CH3, RXD_PIN_CH3, RTS_PIN_CH3);

  // 参数验证和转换（支持整数输入）
  uart_parity_t uart_parity;
  uart_word_length_t uart_data_bits = data_bits;
  uart_stop_bits_t uart_stop_bits = stop_bits;
  
  // 校验位处理：整数1需要转换为UART_PARITY_ODD(3)
  if ((int)parity == 1) {
    ESP_LOGI(TAG, "UART1校验位转换(整数1): 奇校验");
    uart_parity = UART_PARITY_ODD;
  } else {
    // 0,2,3都是有效的枚举值，直接使用
    ESP_LOGI(TAG, "UART1校验位(枚举值%d): %s", (int)parity, parity_to_string(parity));
    uart_parity = parity;
  }
  
  if ((int)data_bits >= 5 && (int)data_bits <= 8) {
    if ((int)data_bits == 5) uart_data_bits = UART_DATA_5_BITS;
    else if ((int)data_bits == 6) uart_data_bits = UART_DATA_6_BITS;
    else if ((int)data_bits == 7) uart_data_bits = UART_DATA_7_BITS;
    else if ((int)data_bits == 8) uart_data_bits = UART_DATA_8_BITS;
  }
  
  // 停止位已在上面定义，直接使用
  
  uart_config_t uart_config = {
      .baud_rate = baudrate,
      .data_bits = uart_data_bits,
      .parity = uart_parity,
      .stop_bits = uart_stop_bits,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };

  // 使用固定的UART硬件超时时间（3.5个字符时间）
  // frame_time用于应用层包分界判断，不用于UART硬件超时
  int timeout_ticks = 3; // 固定3 ticks，约3.5个字符时间

  int rx_threshold = frame_len;
  if (rx_threshold < 1) rx_threshold = 1;
  if (rx_threshold > 63) rx_threshold = 63;

  int rx_buffer_size = 1024;  // 默认1KB，扩展基础大小
  if (frame_len > 256) rx_buffer_size = 2048;
  if (frame_len > 512) rx_buffer_size = 4096;
  if (frame_len > 1024) rx_buffer_size = 8192;
  if (frame_len > 2048) rx_buffer_size = 8192;



  // 安全地暂停所有UART接收任务（检查是否已在重新初始化中）
  bool should_manage_tasks = !uart_reinit_in_progress;
  if (should_manage_tasks) {
    suspend_all_uart_rx_tasks();
  }

  // 删除现有驱动
  uart_driver_delete(UART_NUM_1);
  
  // 等待一小段时间确保驱动完全释放
  vTaskDelay(pdMS_TO_TICKS(20));
  
  // 配置UART1
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, TXD_PIN_CH3, RXD_PIN_CH3, RTS_PIN_CH3, UART_PIN_NO_CHANGE));
  
  // 安装驱动，带错误处理和重试机制
  esp_err_t err = uart_driver_install(UART_NUM_1, rx_buffer_size, 0, 0, NULL, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UART1 driver install failed with error: %d", err);
    // 如果安装失败，尝试使用最小的缓冲区大小
    rx_buffer_size = 256;
    err = uart_driver_install(UART_NUM_1, rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "UART1 driver install retry failed");
      // 恢复任务后再返回（仅在需要时）
      if (should_manage_tasks) {
        resume_all_uart_rx_tasks();
      }
      return;
    }
    ESP_LOGW(TAG, "UART1 driver installed with reduced buffer size: %d", rx_buffer_size);
  }
  
  ESP_ERROR_CHECK(uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX));
  ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_1, timeout_ticks));
  ESP_ERROR_CHECK(uart_set_rx_full_threshold(UART_NUM_1, rx_threshold));
  
  // 改参后清空硬件与高层缓冲，避免残留包被上层转发
  uart_flush(UART_NUM_1);
  clear_channel_data(3);
  
  // 安全地恢复所有UART接收任务（仅在需要时）
  if (should_manage_tasks) {
    resume_all_uart_rx_tasks();
  }
  
  ESP_LOGI(TAG, "UART1 configured successfully");
  
  // 更新运行时配置缓存
  channel_uart_config_t runtime_config = {
    .channel = 3,  // CH3 -> UART1
    .baudrate = baudrate,
    .data_bits = uart_data_bits,
    .parity = uart_parity,
    .stop_bits = uart_stop_bits,
    .frame_time = frame_time,  // 帧间隔时间(ms) - 用于确定Modbus帧包分界
    .frame_len = frame_len,
    .timeout = 1000  // Modbus协议超时时间(ms) - 不是UART硬件超时
  };
  set_current_runtime_config(3, &runtime_config);
}

void configure_uart2(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len) {
  ESP_LOGI(TAG, "Configuring UART2 (Channel 1 -> UART2)");
  ESP_LOGI(TAG, "Input params - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d", 
           baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
  ESP_LOGI(TAG, "TX: GPIO%d, RX: GPIO%d, RTS: GPIO%d", TXD_PIN_CH1, RXD_PIN_CH1, RTS_PIN_CH1);

  // 参数验证和转换（支持整数输入）
  uart_parity_t uart_parity;
  uart_word_length_t uart_data_bits = data_bits;
  uart_stop_bits_t uart_stop_bits = stop_bits;
  
  // 校验位处理：整数1需要转换为UART_PARITY_ODD(3)
  if ((int)parity == 1) {
    ESP_LOGI(TAG, "UART2校验位转换(整数1): 奇校验");
    uart_parity = UART_PARITY_ODD;
  } else {
    // 0,2,3都是有效的枚举值，直接使用
    ESP_LOGI(TAG, "UART2校验位(枚举值%d): %s", (int)parity, parity_to_string(parity));
    uart_parity = parity;
  }
  
  if ((int)data_bits >= 5 && (int)data_bits <= 8) {
    if ((int)data_bits == 5) uart_data_bits = UART_DATA_5_BITS;
    else if ((int)data_bits == 6) uart_data_bits = UART_DATA_6_BITS;
    else if ((int)data_bits == 7) uart_data_bits = UART_DATA_7_BITS;
    else if ((int)data_bits == 8) uart_data_bits = UART_DATA_8_BITS;
  }
  
  // 停止位已在上面定义，直接使用
  
  uart_config_t uart_config = {
      .baud_rate = baudrate,
      .data_bits = uart_data_bits,
      .parity = uart_parity,
      .stop_bits = uart_stop_bits,
      .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
      .rx_flow_ctrl_thresh = 122,
      .source_clk = UART_SCLK_DEFAULT,
  };

  // 使用固定的UART硬件超时时间（3.5个字符时间）
  // frame_time用于应用层包分界判断，不用于UART硬件超时
  int timeout_ticks = 3; // 固定3 ticks，约3.5个字符时间

  int rx_threshold = frame_len;
  if (rx_threshold < 1) rx_threshold = 1;
  if (rx_threshold > 63) rx_threshold = 63;

  int rx_buffer_size = 1024;  // 默认1KB，扩展基础大小
  if (frame_len > 256) rx_buffer_size = 2048;
  if (frame_len > 512) rx_buffer_size = 4096;
  if (frame_len > 1024) rx_buffer_size = 8192;
  if (frame_len > 2048) rx_buffer_size = 8192;



  // 安全地暂停所有UART接收任务（检查是否已在重新初始化中）
  bool should_manage_tasks = !uart_reinit_in_progress;
  if (should_manage_tasks) {
    suspend_all_uart_rx_tasks();
  }

  // 删除现有驱动
  uart_driver_delete(UART_NUM_2);
  
  // 等待一小段时间确保驱动完全释放
  vTaskDelay(pdMS_TO_TICKS(20));
  
  // 配置UART2
  ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
  ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, TXD_PIN_CH1, RXD_PIN_CH1, RTS_PIN_CH1, UART_PIN_NO_CHANGE));
  
  // 安装驱动，带错误处理和重试机制
  esp_err_t err = uart_driver_install(UART_NUM_2, rx_buffer_size, 0, 0, NULL, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "UART2 driver install failed with error: %d", err);
    // 如果安装失败，尝试使用最小的缓冲区大小
    rx_buffer_size = 256;
    err = uart_driver_install(UART_NUM_2, rx_buffer_size, 0, 0, NULL, 0);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "UART2 driver install retry failed");
      // 恢复任务后再返回（仅在需要时）
      if (should_manage_tasks) {
        resume_all_uart_rx_tasks();
      }
      return;
    }
    ESP_LOGW(TAG, "UART2 driver installed with reduced buffer size: %d", rx_buffer_size);
  }
  
  ESP_ERROR_CHECK(uart_set_mode(UART_NUM_2, UART_MODE_RS485_HALF_DUPLEX));
  ESP_ERROR_CHECK(uart_set_rx_timeout(UART_NUM_2, timeout_ticks));
  ESP_ERROR_CHECK(uart_set_rx_full_threshold(UART_NUM_2, rx_threshold));
  
  // 改参后清空硬件与高层缓冲，避免残留包被上层转发
  uart_flush(UART_NUM_2);
  clear_channel_data(1);
  
  // 安全地恢复所有UART接收任务（仅在需要时）
  if (should_manage_tasks) {
    resume_all_uart_rx_tasks();
  }
  
  ESP_LOGI(TAG, "UART2 configured successfully");
  
  // 更新运行时配置缓存
  channel_uart_config_t runtime_config = {
    .channel = 1,  // CH1 -> UART2
    .baudrate = baudrate,
    .data_bits = uart_data_bits,
    .parity = uart_parity,
    .stop_bits = uart_stop_bits,
    .frame_time = frame_time,  // 帧间隔时间(ms) - 用于确定Modbus帧包分界
    .frame_len = frame_len,
    .timeout = 1000  // Modbus协议超时时间(ms) - 不是UART硬件超时
  };
  set_current_runtime_config(1, &runtime_config);
}

void uart_init(void) {
  if (!ensure_uart_runtime_buffers_ready()) {
    ESP_LOGE(TAG, "UART运行时缓冲区初始化失败");
    return;
  }

  esp_err_t rets = nvs_init();
  nvs_handle_t nvs_handle;
  if (rets != ESP_OK) {
    ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(rets));
    return;
  }

  if (nvs_open("storage", NVS_READWRITE, &nvs_handle) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to open NVS namespace");
    return;
  }

  // size_t baud_rate, data_bit, check_bit, stop_bit, frame_time, frame_len = 0;
  //ch1
  char *nvs_values1[7] = {NULL}; // 用数组统一管理内存，增加reply_timeout
  size_t sizes[7] = {0};
  const char *keys[7] = {"ch1_baud_rate", "ch1_data_bit", "ch1_check_bit",
                         "ch1_stop_bit", "ch1_frame_time", "ch1_frame_len", "ch1_timeout"};
  //ch2
  char *nvs_values2[7] = {NULL}; // 用数组统一管理内存，增加reply_timeout
  size_t sizes2[7] = {0};
  const char *keys2[7] = {"ch2_baud_rate", "ch2_data_bit", "ch2_check_bit",
                         "ch2_stop_bit", "ch2_frame_time", "ch2_frame_len", "ch2_timeout"};
  //ch3
  char *nvs_values3[7] = {NULL}; // 用数组统一管理内存，增加reply_timeout
  size_t sizes3[7] = {0};
  const char *keys3[7] = {"ch3_baud_rate", "ch3_data_bit", "ch3_check_bit",
                         "ch3_stop_bit", "ch3_frame_time", "ch3_frame_len", "ch3_timeout"};
                    
  while (true) {
    bool need_defaults = false;
    memset(sizes, 0, sizeof(sizes));
    memset(sizes2, 0, sizeof(sizes2));
    memset(sizes3, 0, sizeof(sizes3));

    // 获取通道1字段大小
    for (int i = 0; i < 7; i++) {
      esp_err_t err = nvs_get_str(nvs_handle, keys[i], NULL, &sizes[i]);
      if (err == ESP_ERR_NVS_NOT_FOUND) {
        need_defaults = true;
      } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get size for %s", keys[i]);
        nvs_close(nvs_handle);
        return;
      }
    }

    // 获取通道2字段大小
    for (int i = 0; i < 7; i++) {
      esp_err_t err = nvs_get_str(nvs_handle, keys2[i], NULL, &sizes2[i]);
      if (err == ESP_ERR_NVS_NOT_FOUND) {
        need_defaults = true;
      } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get size for %s", keys2[i]);
        nvs_close(nvs_handle);
        return;
      }
    }

    // 获取通道3字段大小
    for (int i = 0; i < 7; i++) {
      esp_err_t err = nvs_get_str(nvs_handle, keys3[i], NULL, &sizes3[i]);
      if (err == ESP_ERR_NVS_NOT_FOUND) {
        need_defaults = true;
      } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get size for %s", keys3[i]);
        nvs_close(nvs_handle);
        return;
      }
    }

    if (!need_defaults) {
      break;
    }

    ESP_LOGW(TAG, "First run, setting default values");
    // ch1
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_frame_len", "512"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch1_timeout", "1000"));
    // ch2
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_frame_len", "512"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch2_timeout", "1000"));
    // ch3
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_baud_rate", "9600"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_data_bit", "8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_check_bit", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_stop_bit", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_frame_time", "50"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_frame_len", "512"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ch3_timeout", "1000"));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    // 再次循环，重新读取所有字段长度
  }

  // 分配内存并读取值
  //ch1
  bool allocation_failed = false;
  for (int i = 0; i < 7; i++) {
    nvs_values1[i] = malloc(sizes[i]);
    if (nvs_values1[i] == NULL) {
      ESP_LOGE(TAG, "Memory allocation failed for %s", keys[i]);
      allocation_failed = true;
      break;
    }

    if (nvs_get_str(nvs_handle, keys[i], nvs_values1[i], &sizes[i]) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to read %s", keys[i]);
      allocation_failed = true;
      break;
    }
  }
  //ch2
  for (int i = 0; i < 7; i++) {
    nvs_values2[i] = malloc(sizes2[i]);
    if (nvs_values2[i] == NULL) {
      ESP_LOGE(TAG, "Memory allocation failed for %s", keys2[i]);
      allocation_failed = true;
      break;
    }

    if (nvs_get_str(nvs_handle, keys2[i], nvs_values2[i], &sizes2[i]) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to read %s", keys2[i]);
      allocation_failed = true;
      break;
    }
  }
  //ch3
  for (int i = 0; i < 7; i++) {
    nvs_values3[i] = malloc(sizes3[i]);
    if (nvs_values3[i] == NULL) {
      ESP_LOGE(TAG, "Memory allocation failed for %s", keys3[i]);
      allocation_failed = true;
      break;
    }

    if (nvs_get_str(nvs_handle, keys3[i], nvs_values3[i], &sizes3[i]) != ESP_OK) {
      ESP_LOGE(TAG, "Failed to read %s", keys3[i]);
      allocation_failed = true;
      break;
    }
  }

  // 如果所有值都成功读取，则配置所有UART实例
  if (!allocation_failed) {
    //ch1
    int baudrate = atoi(nvs_values1[0]);
    int data_bits1 = atoi(nvs_values1[1]);
    int frame_time = atoi(nvs_values1[4]);
    int frame_len = atoi(nvs_values1[5]);
    //ch2
    int baudrate2 = atoi(nvs_values2[0]);
    uart_word_length_t data_bits2 = atoi(nvs_values2[1]);
    int frame_time2 = atoi(nvs_values2[4]);
    int frame_len2 = atoi(nvs_values2[5]);
    //ch3
    int baudrate3 = atoi(nvs_values3[0]);
    uart_word_length_t data_bits3 = atoi(nvs_values3[1]);
    int frame_time3 = atoi(nvs_values3[4]);
    int frame_len3 = atoi(nvs_values3[5]);
    // 转换参数格式 - 使用改进的校验位解析函数
    uart_parity_t uart_parity = parse_parity_string(nvs_values1[2]);
    ESP_LOGI(TAG, "CH1校验位配置: %s -> %s", 
             nvs_values1[2] ? nvs_values1[2] : "NULL", parity_to_string(uart_parity));
    
    //ch2
    uart_parity_t uart_parity2 = parse_parity_string(nvs_values2[2]);
    ESP_LOGI(TAG, "CH2校验位配置: %s -> %s", 
             nvs_values2[2] ? nvs_values2[2] : "NULL", parity_to_string(uart_parity2));
    
    //ch3
    uart_parity_t uart_parity3 = parse_parity_string(nvs_values3[2]);
    ESP_LOGI(TAG, "CH3校验位配置: %s -> %s", 
             nvs_values3[2] ? nvs_values3[2] : "NULL", parity_to_string(uart_parity3));
    //ch1
    uart_word_length_t uart_data_bits1 = UART_DATA_8_BITS;
    if (data_bits1 == 5) uart_data_bits1 = UART_DATA_5_BITS;
    else if (data_bits1 == 6) uart_data_bits1 = UART_DATA_6_BITS;
    else if (data_bits1 == 7) uart_data_bits1 = UART_DATA_7_BITS;    
    uart_stop_bits_t uart_stop_bits1 = UART_STOP_BITS_1;
    if (strcmp(nvs_values1[3], "1.5") == 0) uart_stop_bits1 = UART_STOP_BITS_1_5;
    else if (strcmp(nvs_values1[3], "2") == 0) uart_stop_bits1 = UART_STOP_BITS_2;
    //ch2
    uart_word_length_t uart_data_bits2 = UART_DATA_8_BITS;
    if (data_bits2 == 5) uart_data_bits2 = UART_DATA_5_BITS;
    else if (data_bits2 == 6) uart_data_bits2 = UART_DATA_6_BITS;
    else if (data_bits2 == 7) uart_data_bits2 = UART_DATA_7_BITS;
    uart_stop_bits_t uart_stop_bits2 = UART_STOP_BITS_1;
    if (strcmp(nvs_values2[3], "1.5") == 0) uart_stop_bits2 = UART_STOP_BITS_1_5;
    else if (strcmp(nvs_values2[3], "2") == 0) uart_stop_bits2 = UART_STOP_BITS_2;
    //ch3
    uart_word_length_t uart_data_bits3 = UART_DATA_8_BITS;
    if (data_bits3 == 5) uart_data_bits3 = UART_DATA_5_BITS;
    else if (data_bits3 == 6) uart_data_bits3 = UART_DATA_6_BITS;
    else if (data_bits3 == 7) uart_data_bits3 = UART_DATA_7_BITS;
    uart_stop_bits_t uart_stop_bits3 = UART_STOP_BITS_1;
    if (strcmp(nvs_values3[3], "1.5") == 0) uart_stop_bits3 = UART_STOP_BITS_1_5;
    else if (strcmp(nvs_values3[3], "2") == 0) uart_stop_bits3 = UART_STOP_BITS_2;
    // 分别初始化三个UART实例
    ESP_LOGI(TAG, "Initializing all three UART instances...");
    
    // 打印配置信息用于调试
    ESP_LOGI(TAG, "Channel 1 (UART2) - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d", 
             baudrate, uart_data_bits1, uart_parity, uart_stop_bits1, frame_time, frame_len);
    ESP_LOGI(TAG, "Channel 2 (UART0) - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d", 
             baudrate2, uart_data_bits2, uart_parity2, uart_stop_bits2, frame_time2, frame_len2);
    ESP_LOGI(TAG, "Channel 3 (UART1) - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d", 
             baudrate3, uart_data_bits3, uart_parity3, uart_stop_bits3, frame_time3, frame_len3);
    
    // 按当前绑定：CH1->UART2, CH2->UART0, CH3->UART1

    // 初始化UART0 (使用通道2配置)
    configure_uart0(baudrate2, uart_data_bits2, uart_parity2, uart_stop_bits2, frame_time2, frame_len2);

    // 初始化UART1 (使用通道3配置)
    configure_uart1(baudrate3, uart_data_bits3, uart_parity3, uart_stop_bits3, frame_time3, frame_len3);

    // 初始化UART2 (使用通道1配置)
    configure_uart2(baudrate, uart_data_bits1, uart_parity, uart_stop_bits1, frame_time, frame_len);

    // configure_uart*只接收物理串口参数；用NVS中的完整配置覆盖运行时缓存，
    // 确保reply_timeout与网页/NVS保持一致。
    channel_uart_config_t runtime1 = {
        1, baudrate, uart_data_bits1, uart_parity, uart_stop_bits1,
        frame_time, frame_len, atoi(nvs_values1[6])};
    channel_uart_config_t runtime2 = {
        2, baudrate2, uart_data_bits2, uart_parity2, uart_stop_bits2,
        frame_time2, frame_len2, atoi(nvs_values2[6])};
    channel_uart_config_t runtime3 = {
        3, baudrate3, uart_data_bits3, uart_parity3, uart_stop_bits3,
        frame_time3, frame_len3, atoi(nvs_values3[6])};
    set_current_runtime_config(1, &runtime1);
    set_current_runtime_config(2, &runtime2);
    set_current_runtime_config(3, &runtime3);
    
    ESP_LOGI(TAG, "All three UART instances initialized successfully");
  }

  // 清理资源
  //ch1
  for (int i = 0; i < 7; i++) {
    if (nvs_values1[i] != NULL) {
      free(nvs_values1[i]);
    }
  }
  //ch2
  for (int i = 0; i < 7; i++) {
    if (nvs_values2[i] != NULL) {
      free(nvs_values2[i]);
    }
  }
  //ch3
  for (int i = 0; i < 7; i++) {
    if (nvs_values3[i] != NULL) {
      free(nvs_values3[i]);
    }
  }
    nvs_close(nvs_handle);
}
void uart_reinit(int baudrate1, uart_word_length_t data_bits1, uart_parity_t parity1, uart_stop_bits_t stop_bits1, int frame_time1, int frame_len1,
                 int timeout1,
                 int baudrate2, uart_word_length_t data_bits2, uart_parity_t parity2, uart_stop_bits_t stop_bits2, int frame_time2, int frame_len2,
                 int timeout2,
                 int baudrate3, uart_word_length_t data_bits3, uart_parity_t parity3, uart_stop_bits_t stop_bits3, int frame_time3, int frame_len3,
                 int timeout3) {
  ESP_LOGI(TAG, "Starting UART reinitialize with individual channel configurations");
  
  // 设置重新初始化标志，防止configure_uart函数重复管理任务
  uart_reinit_in_progress = true;
  
  // 安全地暂停所有UART接收任务（只在Web重新配置时需要）
  suspend_all_uart_rx_tasks();
  
  // 也停止旧的单任务处理方式（为了兼容性）
  stop_rx_task();
  
  printf("Reinit UART with individual channel configurations\n");
  
  // 打印通道1配置
  printf("Channel 1 (UART2) - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d\n", 
         baudrate1, data_bits1, parity1, stop_bits1, frame_time1, frame_len1);
  
  // 打印通道2配置
  printf("Channel 2 (UART0) - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d\n", 
         baudrate2, data_bits2, parity2, stop_bits2, frame_time2, frame_len2);
  
  // 打印通道3配置
  printf("Channel 3 (UART1) - Baudrate: %d, Data bits: %d, Parity: %d, Stop bits: %d, Frame time: %d, Frame len: %d\n", 
         baudrate3, data_bits3, parity3, stop_bits3, frame_time3, frame_len3);
  
  // 等待任务完全停止
  vTaskDelay(pdMS_TO_TICKS(10));

  // 转换通道1参数格式 - 修复校验位处理
  // 注意：传入的parity参数可能是uart_parity_t枚举值(0,2,3)或整数值(0,1,2)
  // 简化判断：如果值为1，肯定是整数格式（因为枚举中不存在1）
  uart_parity_t uart_parity1;
  if ((int)parity1 == 1) {
    // 整数1代表奇校验，转换为UART_PARITY_ODD(3)
    ESP_LOGI(TAG, "CH1校验位转换(整数1): 奇校验");
    uart_parity1 = UART_PARITY_ODD;
  } else {
    // 0,2,3都是有效的枚举值，直接使用
    ESP_LOGI(TAG, "CH1校验位(枚举值%d): %s", (int)parity1, parity_to_string(parity1));
    uart_parity1 = parity1;
  }
  
  // 调用方传入的已经是ESP-IDF uart_word_length_t枚举，不再按5/6/7重复转换。
  uart_word_length_t uart_data_bits1 = data_bits1;
  
  uart_stop_bits_t uart_stop_bits1 = stop_bits1;

  // 转换通道2参数格式 - 修复校验位处理
  uart_parity_t uart_parity2;
  if ((int)parity2 == 1) {
    ESP_LOGI(TAG, "CH2校验位转换(整数1): 奇校验");
    uart_parity2 = UART_PARITY_ODD;
  } else {
    ESP_LOGI(TAG, "CH2校验位(枚举值%d): %s", (int)parity2, parity_to_string(parity2));
    uart_parity2 = parity2;
  }
  
  uart_word_length_t uart_data_bits2 = data_bits2;
  
  uart_stop_bits_t uart_stop_bits2 = stop_bits2;

  // 转换通道3参数格式 - 修复校验位处理
  uart_parity_t uart_parity3;
  if ((int)parity3 == 1) {
    ESP_LOGI(TAG, "CH3校验位转换(整数1): 奇校验");
    uart_parity3 = UART_PARITY_ODD;
  } else {
    ESP_LOGI(TAG, "CH3校验位(枚举值%d): %s", (int)parity3, parity_to_string(parity3));
    uart_parity3 = parity3;
  }
  
  uart_word_length_t uart_data_bits3 = data_bits3;
  
  uart_stop_bits_t uart_stop_bits3 = stop_bits3;

  // 重新配置所有UART实例，每个通道使用独立的参数
  // 使用新的单个通道重启方法
  channel_uart_config_t config1 = {1, baudrate1, uart_data_bits1, uart_parity1, uart_stop_bits1, frame_time1, frame_len1, timeout1};
  channel_uart_config_t config2 = {2, baudrate2, uart_data_bits2, uart_parity2, uart_stop_bits2, frame_time2, frame_len2, timeout2};
  channel_uart_config_t config3 = {3, baudrate3, uart_data_bits3, uart_parity3, uart_stop_bits3, frame_time3, frame_len3, timeout3};
  
  restart_single_channel(1, &config1);  // CH1
  restart_single_channel(2, &config2);  // CH2  
  restart_single_channel(3, &config3);  // CH3
  
  // 改参后清空三路通道缓冲，避免残留包被上层转发
  clear_channel_data(1);
  clear_channel_data(2);
  clear_channel_data(3);
  
  // 清除重新初始化标志
  uart_reinit_in_progress = false;
  
  // 安全地恢复所有UART接收任务
  resume_all_uart_rx_tasks();
  
  ESP_LOGI(TAG, "UART reinitialize completed successfully");
}


int sendDataToUart(char *data, size_t length, uart_port_t uart_num) {
    if (data == NULL || length == 0) {
        ESP_LOGE("UART_TX", "UART%d TX参数无效: data=%p length=%zu", uart_num,
                 data, length);
        return -1;
    }

    if (!ensure_uart_runtime_buffers_ready()) {
        return -1;
    }

    if (length > UART_TRACE_BUFFER_SIZE) {
        ESP_LOGE("UART_TX", "UART%d TX长度超限: %zu > %zu", uart_num, length,
                 (size_t)UART_TRACE_BUFFER_SIZE);
        return -1;
    }

    gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLUP_ONLY);
    uart_timestamps.tx_timestamp = time_manager_get_current_us();

    int txBytes;
    // 直接将数据作为二进制数据发送，不进行字符串检查
    portENTER_CRITICAL(&uart_spinlock);
    memset(uart_tx_data, 0, UART_TRACE_BUFFER_SIZE);
    memcpy(uart_tx_data, data, length);
    tx_data_len = length;
    portEXIT_CRITICAL(&uart_spinlock);

    // 直接发送二进制数据
    txBytes = uart_write_bytes(uart_num, data, length);
    if (txBytes < 0 || (size_t)txBytes != length) {
        ESP_LOGE("UART_TX", "UART%d TX写入失败: txBytes=%d expected=%zu",
                 uart_num, txBytes, length);
        gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLDOWN_ONLY);
        return -1;
    }

    ESP_LOGD("UART_TX", "Sending data to UART%d:", uart_num);
    ESP_LOG_BUFFER_HEXDUMP("UART_TX", data, length, ESP_LOG_DEBUG);

    // 增加更可靠的等待机制，等待数据发送完成
    esp_err_t err = uart_wait_tx_done(uart_num, 100); // 增加等待时间到100毫秒
    if (err == ESP_OK) {
        gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLDOWN_ONLY);
        ESP_LOGD("UART_TX", "UART%d TX数据发送完成", uart_num);
    } else {
        gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLUP_ONLY);
        ESP_LOGW("UART_TX", "UART%d TX等待超时，错误: %s", uart_num, esp_err_to_name(err));

        // 尝试再次确认发送完成
        vTaskDelay(pdMS_TO_TICKS(10));
        err = uart_wait_tx_done(uart_num, 50);
        if (err == ESP_OK) {
            ESP_LOGD("UART_TX", "UART%d TX数据在延迟后成功发送", uart_num);
            gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLDOWN_ONLY);
        } else {
            ESP_LOGE("UART_TX", "UART%d TX数据发送失败: %s", uart_num, esp_err_to_name(err));
            gpio_set_pull_mode(GPIO_NUM_4, GPIO_PULLDOWN_ONLY);
            return -1;
        }
    }

    return txBytes;
}



esp_err_t tx_tasks_to_channel(uint8_t data[], size_t length, int channel) {
  static const char *TX_TASK_TAG = "TX_TASK";
  uart_port_t uart_num;
  
  switch(channel) {
    case 1:
      uart_num = UART_NUM_2;  // CH1 -> UART2
      break;
    case 2:
      uart_num = UART_NUM_0;  // CH2 -> UART0
      break;
    case 3:
      uart_num = UART_NUM_1;  // CH3 -> UART1
      break;
    default:
      ESP_LOGE(TX_TASK_TAG, "Invalid channel: %d", channel);
      return ESP_ERR_INVALID_ARG;
  }
  
  printf("TX_TASK for UART%d (Channel %d)\n", uart_num, channel);
  uart_tx_led_on(channel); // 对应通道发送LED点亮
  esp_log_level_set(TX_TASK_TAG, ESP_LOG_INFO);

  int tx_ret = sendDataToUart((char *)data, length, uart_num);

  uart_tx_led_off(channel); // 对应通道发送LED熄灭

  if (tx_ret < 0) {
    ESP_LOGE(TX_TASK_TAG, "UART%d发送失败，跳过WebSocket发送记录", uart_num);
    return ESP_FAIL;
  }

  // 通过WebSocket实时推送发送的数据
  send_uart_to_websocket_from_port(data, length, true, uart_num); // true表示发送数据

  // 注意：不要在这里释放data，因为调用者负责内存管理
  // 对于十六进制数据，调用者会释放malloc的内存
  // 对于ASCII字符串，data指向cJSON对象的内存，不应释放
  return ESP_OK;
}



void rx_task_for_uart(uart_port_t uart_num, void *arg) {
  ESP_LOGI(TAG, "启动RX任务 for UART%d", uart_num);
  static const char *RX_TASK_TAG = "RX_TASK";
  esp_log_level_set(RX_TASK_TAG, ESP_LOG_INFO);

  // 由UART端口号推导通道号（CH1<-UART2, CH2<-UART0, CH3<-UART1），供LED与缓冲区使用
  int channel = 0;
  switch (uart_num) {
    case UART_NUM_0: channel = 2; break;  // CH2
    case UART_NUM_1: channel = 3; break;  // CH3
    case UART_NUM_2: channel = 1; break;  // CH1
    default: channel = 0; break;
  }

  while (1) {
    if (uart_reinit_in_progress) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }

    uint8_t *data = NULL;
    if (uartTaskHandle != NULL && eTaskGetState(uartTaskHandle) == eDeleted) {
      break;
    }

    size_t buffered_size;
    if (uart_get_buffered_data_len(uart_num, &buffered_size) != ESP_OK) {
      vTaskDelay(sx_ms_to_ticks(5));
      continue;
    }

    if (buffered_size > 0) {
      ESP_LOGD(RX_TASK_TAG, "UART%d 收到数据，长度: %zu 字节", uart_num, buffered_size);

      // 点亮接收LED
      uart_rx_led_on(channel); // 对应通道接收LED点亮

      data = allocate_buffer();
      if (data == NULL) {
        ESP_LOGE(RX_TASK_TAG, "内存分配失败");
        uart_rx_led_off(channel); // 内存分配失败时熄灭LED
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }

      // 获取frame_time配置，用作包间隔时间
      channel_uart_config_t current_config;
      int frame_time_ms = 50; // 默认50ms

      if (channel > 0 && get_current_runtime_config(channel, &current_config) == ESP_OK) {
        frame_time_ms = current_config.frame_time;
        // 参数验证：frame_time必须在合理范围内
        if (frame_time_ms < 1) {
          frame_time_ms = 1; // 最小1ms
          ESP_LOGW(RX_TASK_TAG, "UART%d frame_time过小，调整为1ms", uart_num);
        } else if (frame_time_ms > 1000) {
          frame_time_ms = 1000; // 最大1秒
          ESP_LOGW(RX_TASK_TAG, "UART%d frame_time过大，调整为1000ms", uart_num);
        }
        ESP_LOGD(RX_TASK_TAG, "UART%d 使用配置的包间隔时间: %d ms", uart_num, frame_time_ms);
      } else {
        ESP_LOGD(RX_TASK_TAG, "UART%d 使用默认包间隔时间: %d ms", uart_num, frame_time_ms);
      }

      // 实现包分界逻辑：持续读取直到frame_time ms内没有新数据
      size_t total_rxBytes = 0;
      uint64_t last_receive_time = esp_timer_get_time();
      uint64_t packet_start_time = last_receive_time;
      const uint64_t max_packet_timeout = 5000000; // 5秒总超时保护(微秒)
      
      while (total_rxBytes < BUF_SIZE) {
        // 总超时保护：防止无限等待
        uint64_t current_time = esp_timer_get_time();
        if ((current_time - packet_start_time) >= max_packet_timeout) {
          ESP_LOGW(RX_TASK_TAG, "UART%d 包接收总超时(5s)，当前长度: %zu 字节", uart_num, total_rxBytes);
          break;
        }
        
        size_t current_buffered;
        if (uart_get_buffered_data_len(uart_num, &current_buffered) != ESP_OK) {
          ESP_LOGW(RX_TASK_TAG, "UART%d 获取缓冲区长度失败", uart_num);
          break;
        }
        
        if (current_buffered > 0) {
          // 还有数据，继续读取
          size_t read_size = (current_buffered > BUF_SIZE - total_rxBytes) ? 
                            (BUF_SIZE - total_rxBytes) : current_buffered;
          
          if (read_size == 0) {
            ESP_LOGW(RX_TASK_TAG, "UART%d 缓冲区已满，停止接收", uart_num);
            break;
          }
          
          int bytes_read = uart_read_bytes(uart_num, data + total_rxBytes, read_size, 
                                         pdMS_TO_TICKS(10));
          if (bytes_read > 0) {
            total_rxBytes += bytes_read;
            last_receive_time = esp_timer_get_time();
            ESP_LOGD(RX_TASK_TAG, "UART%d 累计接收: %zu 字节", uart_num, total_rxBytes);
          } else if (bytes_read < 0) {
            ESP_LOGW(RX_TASK_TAG, "UART%d 读取错误，停止接收", uart_num);
            break;
          }
        } else {
          // 没有新数据，检查是否超过包间隔时间
          current_time = esp_timer_get_time();
          if ((current_time - last_receive_time) >= (frame_time_ms * 1000ULL)) {
            // 超过包间隔时间，包接收完成
            ESP_LOGD(RX_TASK_TAG, "UART%d 包接收完成，总长度: %zu 字节，间隔时间: %d ms",
                     uart_num, total_rxBytes, frame_time_ms);
            break;
          }
          // 等待1ms后再检查
          vTaskDelay(sx_ms_to_ticks(1));
        }
      }
      
      // 检查是否因缓冲区满而退出
      if (total_rxBytes >= BUF_SIZE) {
        ESP_LOGW(RX_TASK_TAG, "UART%d 缓冲区已满，可能有数据丢失", uart_num);
      }
      
      int rxBytes = total_rxBytes;

      if (rxBytes > 0) {
        ESP_LOG_BUFFER_HEXDUMP(RX_TASK_TAG, data, rxBytes, ESP_LOG_DEBUG);
        uart_rx_led_off(channel); // 接收完成后熄灭LED
        uart_timestamps.rx_timestamp = time_manager_get_current_us();
        bool runtime_buffers_ready = ensure_uart_runtime_buffers_ready();
        
        // 更新全局接收数据缓冲区，供web界面显示（保持兼容性）
        portENTER_CRITICAL(&uart_spinlock);
        if (runtime_buffers_ready && rxBytes <= UART_TRACE_BUFFER_SIZE) {
          memcpy(uart_response, data, rxBytes);
          response_len = rxBytes;
        } else {
          response_len = 0; // 数据太长，不保存
        }
        portEXIT_CRITICAL(&uart_spinlock);
        
        // 存储到对应的通道缓冲区（channel已在前面确定）
        if (runtime_buffers_ready && channel > 0) {
          int index = channel - 1;
          portENTER_CRITICAL(&channel_spinlock);
          if (rxBytes <= CHANNEL_FRAME_MAX_LEN) {
            uint32_t before_dropped = channel_buffers[index].dropped;
            channel_queue_push_locked(&channel_buffers[index], data, rxBytes,
                                      uart_timestamps.rx_timestamp);
            uint8_t queued = channel_buffers[index].count;
            bool dropped = channel_buffers[index].dropped != before_dropped;
            portEXIT_CRITICAL(&channel_spinlock);
            if (dropped) {
              ESP_LOGW(RX_TASK_TAG,
                       "UART%d 通道%d队列已满，丢弃最旧帧后保存新帧，长度: %zu，队列: %u/%u",
                       uart_num, channel, rxBytes, queued, CHANNEL_QUEUE_DEPTH);
            } else {
              ESP_LOGI(RX_TASK_TAG,
                       "UART%d 数据已保存到通道%d队列，长度: %zu，队列: %u/%u",
                       uart_num, channel, rxBytes, queued, CHANNEL_QUEUE_DEPTH);
            }
          } else {
            portEXIT_CRITICAL(&channel_spinlock);
            ESP_LOGW(RX_TASK_TAG, "UART%d 数据长度超出通道%d缓冲区大小", uart_num, channel);
          }
        }
        
        // 通过WebSocket实时推送接收到的数据
        send_uart_to_websocket_from_port(data, rxBytes, false, uart_num); // false表示接收数据
      } else {
        // 没有接收到数据，只熄灭LED
        uart_rx_led_off(channel);
      }

      // 释放主数据缓冲区
      if (data != NULL) {
        free_buffer(data);
        data = NULL;
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }

  ESP_LOGI(RX_TASK_TAG, "接收任务退出");
  delete_self_app_task_with_caps();
}


void stop_rx_task() {
  if (uartTaskHandle != NULL) {
    delete_app_task_with_caps(uartTaskHandle);
    uartTaskHandle = NULL;
  }
}

// 创建多UART接收任务
void create_multi_uart_rx_tasks(void) {
  ESP_LOGI(TAG, "Creating multi-UART receive tasks...");
  
  // 为每个UART实例创建接收任务并保存任务句柄 - 进一步增大栈大小到12KB以彻底解决栈溢出
  BaseType_t ret0 = create_app_task_psram(rx_task_for_uart_wrapper, "rx_task_uart0", 16384, (void*)UART_NUM_0, 22, &uart0_rx_task_handle, SX_WORK_CORE_ID);
  BaseType_t ret1 = create_app_task_psram(rx_task_for_uart_wrapper, "rx_task_uart1", 16384, (void*)UART_NUM_1, 22, &uart1_rx_task_handle, SX_WORK_CORE_ID);
  BaseType_t ret2 = create_app_task_psram(rx_task_for_uart_wrapper, "rx_task_uart2", 16384, (void*)UART_NUM_2, 22, &uart2_rx_task_handle, SX_WORK_CORE_ID);
  
  if (ret0 == pdPASS && ret1 == pdPASS && ret2 == pdPASS) {
    ESP_LOGI(TAG, "Multi-UART receive tasks created successfully");
  } else {
    ESP_LOGE(TAG, "Failed to create some UART receive tasks (ret0=%d, ret1=%d, ret2=%d)", ret0, ret1, ret2);
  }
}

// FreeRTOS任务包装函数
void rx_task_for_uart_wrapper(void *arg) {
  uart_port_t uart_num = (uart_port_t)(intptr_t)arg;
  rx_task_for_uart(uart_num, NULL);
}

// 安全地暂停所有UART接收任务
void suspend_all_uart_rx_tasks(void) {
  ESP_LOGI(TAG, "Attempting to suspend all UART RX tasks...");
  
  // 检查调度器状态
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    ESP_LOGW(TAG, "Scheduler not running, skipping task suspension");
    return;
  }
  
  // 暂停UART0接收任务
  if (uart0_rx_task_handle != NULL && eTaskGetState(uart0_rx_task_handle) != eDeleted) {
    vTaskSuspend(uart0_rx_task_handle);
    ESP_LOGI(TAG, "UART0 RX task suspended");
  } else {
    ESP_LOGI(TAG, "UART0 RX task not found or already deleted");
  }
  
  // 暂停UART1接收任务
  if (uart1_rx_task_handle != NULL && eTaskGetState(uart1_rx_task_handle) != eDeleted) {
    vTaskSuspend(uart1_rx_task_handle);
    ESP_LOGI(TAG, "UART1 RX task suspended");
  } else {
    ESP_LOGI(TAG, "UART1 RX task not found or already deleted");
  }
  
  // 暂停UART2接收任务
  if (uart2_rx_task_handle != NULL && eTaskGetState(uart2_rx_task_handle) != eDeleted) {
    vTaskSuspend(uart2_rx_task_handle);
    ESP_LOGI(TAG, "UART2 RX task suspended");
  } else {
    ESP_LOGI(TAG, "UART2 RX task not found or already deleted");
  }
}

// 安全地恢复所有UART接收任务
void resume_all_uart_rx_tasks(void) {
  ESP_LOGI(TAG, "Attempting to resume all UART RX tasks...");
  
  // 检查调度器状态
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    ESP_LOGW(TAG, "Scheduler not running, skipping task resumption");
    return;
  }
  
  // 恢复UART0接收任务
  if (uart0_rx_task_handle != NULL && eTaskGetState(uart0_rx_task_handle) == eSuspended) {
    vTaskResume(uart0_rx_task_handle);
    ESP_LOGI(TAG, "UART0 RX task resumed");
  } else {
    ESP_LOGI(TAG, "UART0 RX task not suspended or not found");
  }
  
  // 恢复UART1接收任务
  if (uart1_rx_task_handle != NULL && eTaskGetState(uart1_rx_task_handle) == eSuspended) {
    vTaskResume(uart1_rx_task_handle);
    ESP_LOGI(TAG, "UART1 RX task resumed");
  } else {
    ESP_LOGI(TAG, "UART1 RX task not suspended or not found");
  }
  
  // 恢复UART2接收任务
  if (uart2_rx_task_handle != NULL && eTaskGetState(uart2_rx_task_handle) == eSuspended) {
    vTaskResume(uart2_rx_task_handle);
    ESP_LOGI(TAG, "UART2 RX task resumed");
  } else {
    ESP_LOGI(TAG, "UART2 RX task not suspended or not found");
  }
}

void stop_all_uart_tasks(void) {
  ESP_LOGI(TAG, "Stopping all UART RX tasks and turning off UART LEDs");

  suspend_all_uart_rx_tasks();

  if (uart0_rx_task_handle != NULL &&
      eTaskGetState(uart0_rx_task_handle) != eDeleted) {
    ESP_LOGI(TAG, "Deleting UART0 RX task");
    delete_app_task_with_caps(uart0_rx_task_handle);
  }
  if (uart1_rx_task_handle != NULL &&
      eTaskGetState(uart1_rx_task_handle) != eDeleted) {
    ESP_LOGI(TAG, "Deleting UART1 RX task");
    delete_app_task_with_caps(uart1_rx_task_handle);
  }
  if (uart2_rx_task_handle != NULL &&
      eTaskGetState(uart2_rx_task_handle) != eDeleted) {
    ESP_LOGI(TAG, "Deleting UART2 RX task");
    delete_app_task_with_caps(uart2_rx_task_handle);
  }

  uart0_rx_task_handle = NULL;
  uart1_rx_task_handle = NULL;
  uart2_rx_task_handle = NULL;

  // 确保所有通道LED熄灭
  uart_tx_led_off(1);
  uart_tx_led_off(2);
  uart_tx_led_off(3);
  uart_rx_led_off(1);
  uart_rx_led_off(2);
  uart_rx_led_off(3);
}

// 发送数据到指定UART通道
esp_err_t send_data_to_channel(int channel, char *data, size_t length) {
  uart_port_t uart_num;

  switch(channel) {
    case 1:
      uart_num = UART_NUM_2;  // CH1 -> UART2
      break;
    case 2:
      uart_num = UART_NUM_0;  // CH2 -> UART0
      break;
    case 3:
      uart_num = UART_NUM_1;  // CH3 -> UART1
      break;
    default:
      ESP_LOGE(TAG, "Invalid channel: %d", channel);
      return ESP_ERR_INVALID_ARG;
  }
  
  return sendDataToUart(data, length, uart_num) < 0 ? ESP_FAIL : ESP_OK;
}

// 通过通道获取缓冲区数据的函数实现
int get_channel_data(int channel, uint8_t* buffer, size_t buffer_size, uint64_t* timestamp) {
    if (channel < 1 || channel > 3) {
        ESP_LOGE(TAG, "Invalid channel: %d", channel);
        return -1;
    }
    
    if (buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return -1;
    }

    if (!ensure_uart_runtime_buffers_ready()) {
        return -1;
    }
    
    int index = channel - 1; // 转换为数组索引 (CH1->0, CH2->1, CH3->2)
    
    portENTER_CRITICAL(&channel_spinlock);
    
    if (channel_buffers[index].count == 0) {
        portEXIT_CRITICAL(&channel_spinlock);
        return 0; // 没有数据
    }

    channel_frame_t *frame = &channel_buffers[index].frames[channel_buffers[index].head];
    
    size_t copy_size = (frame->length < buffer_size) ?
                       frame->length : buffer_size;
    
    memcpy(buffer, frame->data, copy_size);
    
    if (timestamp != NULL) {
        *timestamp = frame->timestamp;
    }

    uint8_t queued = channel_buffers[index].count;
    
    portEXIT_CRITICAL(&channel_spinlock);
    
    ESP_LOGI(TAG, "获取通道%d队首数据，长度: %zu 字节，队列: %u/%u", channel,
             copy_size, queued, CHANNEL_QUEUE_DEPTH);
    return copy_size;
}

// 原子读取并清除通道缓冲区，避免 get 后 clear 之间误清新帧。
int pop_channel_data(int channel, uint8_t* buffer, size_t buffer_size, uint64_t* timestamp) {
    if (channel < 1 || channel > 3) {
        ESP_LOGE(TAG, "Invalid channel: %d", channel);
        return -1;
    }

    if (buffer == NULL || buffer_size == 0) {
        ESP_LOGE(TAG, "Invalid buffer parameters");
        return -1;
    }

    if (!ensure_uart_runtime_buffers_ready()) {
        return -1;
    }

    int index = channel - 1;

    portENTER_CRITICAL(&channel_spinlock);

    if (channel_buffers[index].count == 0) {
        portEXIT_CRITICAL(&channel_spinlock);
        return 0;
    }

    channel_frame_t *frame = &channel_buffers[index].frames[channel_buffers[index].head];
    size_t copy_size = (frame->length < buffer_size) ?
                       frame->length : buffer_size;

    memcpy(buffer, frame->data, copy_size);

    if (timestamp != NULL) {
        *timestamp = frame->timestamp;
    }

    frame->length = 0;
    frame->timestamp = 0;
    channel_buffers[index].head =
        (uint8_t)((channel_buffers[index].head + 1U) % CHANNEL_QUEUE_DEPTH);
    channel_buffers[index].count--;
    uint8_t queued = channel_buffers[index].count;

    portEXIT_CRITICAL(&channel_spinlock);

    ESP_LOGI(TAG, "弹出通道%d队首数据，长度: %zu 字节，剩余: %u/%u", channel,
             copy_size, queued, CHANNEL_QUEUE_DEPTH);
    return copy_size;
}

// 清除指定通道的缓冲区数据
void clear_channel_data(int channel) {
    if (channel < 1 || channel > 3) {
        ESP_LOGE(TAG, "Invalid channel: %d", channel);
        return;
    }
    
    int index = channel - 1;

    if (!ensure_uart_runtime_buffers_ready()) {
        return;
    }
    
    portENTER_CRITICAL(&channel_spinlock);
    uint8_t cleared = channel_buffers[index].count;
    channel_buffers[index].head = 0;
    channel_buffers[index].count = 0;
    portEXIT_CRITICAL(&channel_spinlock);
    
    ESP_LOGI(TAG, "清除通道%d队列数据，清除帧数: %u", channel, cleared);
}

// ==================== 新增功能实现 ====================

// 当前UART运行时配置缓存 - 记录当前实际运行的配置（不读取NVS）
static channel_uart_config_t *current_runtime_config = NULL;
static bool *runtime_config_valid = NULL;

static void *uart_psram_calloc(size_t count, size_t size) {
    void *ptr = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ptr == NULL) {
        ptr = heap_caps_calloc(count, size, MALLOC_CAP_8BIT);
    }
    return ptr;
}

static bool ensure_runtime_config_storage(void) {
    if (current_runtime_config == NULL) {
        current_runtime_config = uart_psram_calloc(3, sizeof(channel_uart_config_t));
    }
    if (runtime_config_valid == NULL) {
        runtime_config_valid = uart_psram_calloc(3, sizeof(bool));
    }
    return current_runtime_config != NULL && runtime_config_valid != NULL;
}

// 设置通道当前运行时配置
void set_current_runtime_config(int channel, const channel_uart_config_t* config) {
    if (channel < 1 || channel > 3 || config == NULL) return;
    if (!ensure_runtime_config_storage()) return;
    
    int index = channel - 1;
    current_runtime_config[index] = *config;
    runtime_config_valid[index] = true;
    
    ESP_LOGI(TAG, "更新通道%d运行时配置: 波特率=%d，帧间隔=%dms，超时=%dms", 
             channel, config->baudrate, config->frame_time, config->timeout);
}

// 获取通道当前运行时配置
esp_err_t get_current_runtime_config(int channel, channel_uart_config_t* config) {
    if (channel < 1 || channel > 3 || config == NULL) {
        ESP_LOGE(TAG, "Invalid channel (%d) or config pointer", channel);
        return ESP_ERR_INVALID_ARG;
    }
    if (!ensure_runtime_config_storage()) {
        return ESP_ERR_NO_MEM;
    }
    
    int index = channel - 1;
    if (!runtime_config_valid[index]) {
        ESP_LOGW(TAG, "通道%d运行时配置无效，需要先初始化", channel);
        return ESP_ERR_NOT_FOUND;
    }
    
    *config = current_runtime_config[index];
    return ESP_OK;
}

// 清除运行时配置缓存
void clear_runtime_config(int channel) {
    if (channel < 1 || channel > 3) return;
    if (!ensure_runtime_config_storage()) return;
    runtime_config_valid[channel - 1] = false;
    ESP_LOGI(TAG, "清除通道%d运行时配置", channel);
}

// 从NVS读取通道配置（仅在需要时使用，临时参数发送不需要此函数）
esp_err_t get_channel_uart_config_from_nvs(int channel, channel_uart_config_t* config) {
    if (channel < 1 || channel > 3 || config == NULL) {
        ESP_LOGE(TAG, "Invalid channel (%d) or config pointer", channel);
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = nvs_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    
    nvs_handle_t nvs_handle;
    ret = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 构建NVS键名
    char key_prefix[8];
    snprintf(key_prefix, sizeof(key_prefix), "ch%d", channel);
    
    char key[32];
    char *nvs_values[7] = {NULL};
    size_t sizes[7] = {0};
    const char *suffixes[7] = {"_baud_rate", "_data_bit", "_check_bit", 
                               "_stop_bit", "_frame_time", "_frame_len", "_timeout"};
    
    // 读取所有配置值
    bool read_success = true;
    for (int i = 0; i < 7; i++) {
        snprintf(key, sizeof(key), "%s%s", key_prefix, suffixes[i]);
        
        // 获取大小
        ret = nvs_get_str(nvs_handle, key, NULL, &sizes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to get size for %s: %s", key, esp_err_to_name(ret));
            read_success = false;
            break;
        }
        
        // 分配内存并读取值
        nvs_values[i] = malloc(sizes[i]);
        if (nvs_values[i] == NULL) {
            ESP_LOGE(TAG, "Memory allocation failed for %s", key);
            read_success = false;
            break;
        }
        
        ret = nvs_get_str(nvs_handle, key, nvs_values[i], &sizes[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read %s: %s", key, esp_err_to_name(ret));
            read_success = false;
            break;
        }
    }
    
    nvs_close(nvs_handle);
    
    if (read_success) {
        // 解析配置参数
        config->channel = channel;
        config->baudrate = atoi(nvs_values[0]);
        config->frame_time = atoi(nvs_values[4]);
        config->frame_len = atoi(nvs_values[5]);
        config->timeout = atoi(nvs_values[6]);
        
        // 解析数据位
        int data_bits = atoi(nvs_values[1]);
        if (data_bits == 5) config->data_bits = UART_DATA_5_BITS;
        else if (data_bits == 6) config->data_bits = UART_DATA_6_BITS;
        else if (data_bits == 7) config->data_bits = UART_DATA_7_BITS;
        else config->data_bits = UART_DATA_8_BITS;
        
        // 解析校验位 - 统一使用数字格式
        config->parity = parse_parity_string(nvs_values[2]);
        
        // 解析停止位
        if (strcmp(nvs_values[3], "1.5") == 0) config->stop_bits = UART_STOP_BITS_1_5;
        else if (strcmp(nvs_values[3], "2") == 0) config->stop_bits = UART_STOP_BITS_2;
        else config->stop_bits = UART_STOP_BITS_1;
        
        ESP_LOGI(TAG, "从NVS获取通道%d配置成功: 波特率=%d, 数据位=%d, 校验=%d, 停止位=%d", 
                 channel, config->baudrate, config->data_bits, config->parity, config->stop_bits);
    }
    
    // 清理内存
    for (int i = 0; i < 7; i++) {
        if (nvs_values[i] != NULL) {
            free(nvs_values[i]);
        }
    }
    
    return read_success ? ESP_OK : ESP_FAIL;
}

// 参数对比方法
bool compare_uart_config(const channel_uart_config_t* config1, const channel_uart_config_t* config2) {
    if (config1 == NULL || config2 == NULL) {
        ESP_LOGE(TAG, "配置指针为空");
        return false;
    }
    
    bool is_same = (config1->baudrate == config2->baudrate) &&
                   (config1->data_bits == config2->data_bits) &&
                   (config1->parity == config2->parity) &&
                   (config1->stop_bits == config2->stop_bits) &&
                   (config1->frame_time == config2->frame_time) &&
                   (config1->frame_len == config2->frame_len) &&
                   (config1->timeout == config2->timeout);
    
    if (!is_same) {
        ESP_LOGI(TAG, "配置参数不同:");
        ESP_LOGI(TAG, "  波特率: %d vs %d", config1->baudrate, config2->baudrate);
        ESP_LOGI(TAG, "  数据位: %d vs %d", config1->data_bits, config2->data_bits);
        ESP_LOGI(TAG, "  校验位: %d vs %d", config1->parity, config2->parity);
        ESP_LOGI(TAG, "  停止位: %d vs %d", config1->stop_bits, config2->stop_bits);
        ESP_LOGI(TAG, "  帧时间: %d vs %d", config1->frame_time, config2->frame_time);
        ESP_LOGI(TAG, "  帧长度: %d vs %d", config1->frame_len, config2->frame_len);
        ESP_LOGI(TAG, "  超时: %d vs %d", config1->timeout, config2->timeout);
    } else {
        ESP_LOGI(TAG, "通道%d配置参数相同，无需重新配置", config1->channel);
    }
    
    return is_same;
}

// 单个通道重启方法
esp_err_t restart_single_channel(int channel, const channel_uart_config_t* config) {
    if (channel < 1 || channel > 3 || config == NULL) {
        ESP_LOGE(TAG, "Invalid channel (%d) or config pointer", channel);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "重启通道%d，新配置: 波特率=%d, 数据位=%d, 校验=%d, 停止位=%d", 
             channel, config->baudrate, config->data_bits, config->parity, config->stop_bits);
    
    esp_err_t ret = ESP_OK;
    
    // 根据通道号调用对应的配置函数
    switch(channel) {
        case 1:
            // CH1 -> UART2
            configure_uart2(config->baudrate, config->data_bits, config->parity,
                           config->stop_bits, config->frame_time, config->frame_len);
            break;
        case 2:
            // CH2 -> UART0
            configure_uart0(config->baudrate, config->data_bits, config->parity,
                           config->stop_bits, config->frame_time, config->frame_len);
            break;
        case 3:
            // CH3 -> UART1
            configure_uart1(config->baudrate, config->data_bits, config->parity,
                           config->stop_bits, config->frame_time, config->frame_len);
            break;
        default:
            ESP_LOGE(TAG, "Invalid channel: %d", channel);
            ret = ESP_ERR_INVALID_ARG;
            break;
    }
    
    if (ret == ESP_OK) {
        // configure_uart*完成物理重配后，保存调用方提供的完整配置（含timeout）。
        set_current_runtime_config(channel, config);
        ESP_LOGI(TAG, "通道%d重启完成", channel);
    } else {
        ESP_LOGE(TAG, "通道%d重启失败", channel);
    }
    
    return ret;
}

// 轻量级快速重新配置方法（仅配置必要参数，不重启任务）
esp_err_t quick_reconfigure_channel(int channel, const channel_uart_config_t* config) {
    if (channel < 1 || channel > 3 || config == NULL) {
        ESP_LOGE(TAG, "Invalid channel (%d) or config pointer", channel);
        return ESP_ERR_INVALID_ARG;
    }
    
    uart_port_t uart_num;
    
    // 映射通道到UART号
    switch(channel) {
        case 1: uart_num = UART_NUM_2; break;  // CH1 -> UART2
        case 2: uart_num = UART_NUM_0; break;  // CH2 -> UART0
        case 3: uart_num = UART_NUM_1; break;  // CH3 -> UART1
        default:
            ESP_LOGE(TAG, "Invalid channel: %d", channel);
            return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "快速重新配置通道%d (UART%d)", channel, uart_num);
    
    // 只配置关键的UART参数，避免重启整个驱动
    uart_config_t uart_config = {
        .baud_rate = config->baudrate,
        .data_bits = config->data_bits,
        .parity = config->parity,
        .stop_bits = config->stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    // 使用固定的UART硬件超时时间（3.5个字符时间）
    // frame_time用于应用层包分界判断，不用于UART硬件超时
    int timeout_ticks = 3; // 固定3 ticks，约3.5个字符时间
    
    int rx_threshold = config->frame_len;
    if (rx_threshold < 1) rx_threshold = 1;
    if (rx_threshold > 63) rx_threshold = 63;
    
    // 尝试在线更新配置（不删除驱动）
    esp_err_t ret = uart_param_config(uart_num, &uart_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "在线参数配置失败，回退到完整重启: %s", esp_err_to_name(ret));
        return restart_single_channel(channel, config);
    }
    
    // 更新超时和阈值设置
    ret = uart_set_rx_timeout(uart_num, timeout_ticks);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "设置RX超时失败: %s", esp_err_to_name(ret));
    }
    
    ret = uart_set_rx_full_threshold(uart_num, rx_threshold);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "设置RX阈值失败: %s", esp_err_to_name(ret));
    }
    
    // 清空UART缓冲区并清空高层通道缓冲
    uart_flush(uart_num);
    clear_channel_data(channel);
    
    ESP_LOGI(TAG, "通道%d快速重新配置完成，耗时极短", channel);
    return ESP_OK;
}

// 临时参数发送核心函数：按指定参数发送数据（不持久化配置）
esp_err_t send_data_with_temp_config(int channel, 
                                    const channel_uart_config_t* temp_config,
                                    const uint8_t* data, 
                                    size_t data_len) {
    if (channel < 1 || channel > 3 || temp_config == NULL || data == NULL || data_len == 0) {
        ESP_LOGE(TAG, "Invalid parameters: channel=%d, config=%p, data=%p, data_len=%zu", 
                 channel, temp_config, data, data_len);
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "使用临时配置向通道%d发送%zu字节数据 (波特率:%d，帧间隔:%dms)", 
             channel, data_len, temp_config->baudrate, temp_config->frame_time);
    
    // 1. 获取当前运行时配置
    channel_uart_config_t current_config;
    esp_err_t ret = get_current_runtime_config(channel, &current_config);
    
    bool need_reconfigure = true;
    if (ret == ESP_OK) {
        // 对比配置参数，如果相同则不需要重新配置
        need_reconfigure = !compare_uart_config(&current_config, temp_config);
        if (!need_reconfigure) {
            ESP_LOGI(TAG, "通道%d当前配置与目标配置相同，跳过重新配置", channel);
        }
    } else {
        ESP_LOGI(TAG, "通道%d运行时配置无效，需要重新配置", channel);
    }
    
    // 2. 如果需要重新配置，使用快速重配置
    if (need_reconfigure) {
        ESP_LOGI(TAG, "通道%d配置不同，开始快速重新配置", channel);
        ret = quick_reconfigure_channel(channel, temp_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "快速重新配置通道%d失败: %s", channel, esp_err_to_name(ret));
            return ret;
        }
        
        // 更新运行时配置缓存
        set_current_runtime_config(channel, temp_config);
        
        // 快速配置后只需很短等待
        vTaskDelay(sx_ms_to_ticks(5));
    }
    
    // 3. 发送数据
    ESP_LOGI(TAG, "开始向通道%d发送数据", channel);
    ret = tx_tasks_to_channel((uint8_t*)data, data_len, channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "通道%d临时配置发送失败: %s", channel,
                 esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "通道%d临时配置发送完成", channel);
    return ESP_OK;
}

// 快速版本：跳过参数获取，直接使用提供的当前配置进行对比
esp_err_t fast_check_and_reconfigure_channel_then_send(int channel,
                                                      const channel_uart_config_t* current_config,
                                                      const channel_uart_config_t* target_config,
                                                      const uint8_t* data,
                                                      size_t data_len) {
    if (channel < 1 || channel > 3 || current_config == NULL || target_config == NULL || 
        data == NULL || data_len == 0) {
        ESP_LOGE(TAG, "Invalid parameters in fast version");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "快速检查和配置通道%d，然后发送%zu字节数据", channel, data_len);
    
    // 1. 直接对比配置参数（跳过NVS读取）
    bool configs_match = compare_uart_config(current_config, target_config);
    
    // 2. 如果参数不同，快速重新配置通道  
    if (!configs_match) {
        ESP_LOGI(TAG, "通道%d配置不同，开始快速重新配置", channel);
        esp_err_t ret = quick_reconfigure_channel(channel, target_config);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "快速重新配置通道%d失败: %s", channel, esp_err_to_name(ret));
            return ret;
        }
        
        // 快速配置后只需很短等待
        vTaskDelay(sx_ms_to_ticks(5));  // 更短的等待时间
    }
    
    // 3. 立即发送数据
    esp_err_t ret = tx_tasks_to_channel((uint8_t*)data, data_len, channel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "通道%d快速配置发送失败: %s", channel,
                 esp_err_to_name(ret));
        return ret;
    }
    
    ESP_LOGI(TAG, "通道%d快速检查配置和发送数据完成", channel);
    return ESP_OK;
}

// ==================== 智能发送函数实现 ====================

// 智能发送函数：根据工作模式和来源通道智能选择发送参数
esp_err_t smart_send_data_to_ch3(uart_config_mode_t work_mode, 
                                 int source_channel, 
                                 const uint8_t* data, 
                                 size_t data_len) {
    if (data == NULL || data_len == 0) {
        ESP_LOGE(TAG, "智能发送参数错误: data=%p, data_len=%zu", data, data_len);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "智能发送到CH3，工作模式=%s，来源通道=%d，数据长度=%zu",
             (work_mode == UART_CONFIG_MODE_SLAVE_FOLLOW) ? "从机跟随" : "正常模式",
             source_channel, data_len);

    if (work_mode == UART_CONFIG_MODE_SLAVE_FOLLOW) {
        // 从机跟随模式：使用来源通道的相同参数
        if (source_channel == 1 || source_channel == 2) {
            // 获取来源通道的当前运行时配置
            channel_uart_config_t source_config;
            esp_err_t ret = get_current_runtime_config(source_channel, &source_config);
            
            if (ret == ESP_OK) {
                ESP_LOGI(TAG, "使用CH%d的UART参数发送到CH3，波特率=%d，帧间隔=%dms", 
                         source_channel, source_config.baudrate, source_config.frame_time);
                ESP_LOGI(TAG, "CH3临时配置：来源CH%d的帧间隔%dms -> CH3接收任务", 
                         source_channel, source_config.frame_time);
                
                // 使用来源通道的参数作为临时配置发送到CH3
                return send_data_with_temp_config(3, &source_config, data, data_len);
            } else {
                ESP_LOGE(TAG, "无法获取CH%d的运行时配置: %s", 
                         source_channel, esp_err_to_name(ret));
                return ret;
            }
        } else {
            ESP_LOGE(TAG, "从机跟随模式下来源通道参数出错，通道=%d (应为1或2)", source_channel);
            return ESP_ERR_INVALID_ARG;
        }
    } else {
        // 正常模式：使用CH3自己的配置
        ESP_LOGI(TAG, "正常模式：使用CH3自身的配置发送数据");
        return tx_tasks_to_channel((uint8_t*)data, data_len, 3);
    }
}

// ==================== 使用示例和扩展功能 ====================

/* 临时参数发送使用示例：
void example_temp_config_send() {
    // 1. 准备临时配置（不会保存到NVS）
    channel_uart_config_t temp_config = {
        .channel = 1,
        .baudrate = 115200,  // 临时使用115200波特率
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .frame_time = 50,
        .frame_len = 256,
        .timeout = 1000
    };
    
    // 2. 准备要发送的数据
    uint8_t modbus_data[] = {0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0xC5, 0xCD};
    size_t data_len = sizeof(modbus_data);
    
    // 3. 调用临时参数发送函数
    esp_err_t ret = send_data_with_temp_config(1, &temp_config, modbus_data, data_len);
    if (ret == ESP_OK) {
        ESP_LOGI("EXAMPLE", "临时配置发送成功");
    } else {
        ESP_LOGE("EXAMPLE", "临时配置发送失败: %s", esp_err_to_name(ret));
    }
    
    // 4. 如果需要多次发送相同配置的数据，后续调用会自动跳过重新配置
    uint8_t more_data[] = {0x01, 0x04, 0x00, 0x00, 0x00, 0x02, 0x71, 0xCB};
    ret = send_data_with_temp_config(1, &temp_config, more_data, sizeof(more_data));
    // 这次调用会检测到配置相同，直接发送数据，性能更好
}

// 或者分步使用：
void example_step_by_step() {
    // 1. 获取当前运行时配置
    channel_uart_config_t current_config;
    esp_err_t ret = get_current_runtime_config(2, &current_config);
    if (ret == ESP_OK) {
        ESP_LOGI("EXAMPLE", "当前通道2运行时配置: 波特率=%d", current_config.baudrate);
    } else {
        // 如果运行时配置无效，可以从NVS读取
        ret = get_channel_uart_config_from_nvs(2, &current_config);
        if (ret == ESP_OK) {
            ESP_LOGI("EXAMPLE", "从NVS读取通道2配置: 波特率=%d", current_config.baudrate);
        }
    }
    
    // 2. 准备新配置
    channel_uart_config_t new_config = current_config;
    new_config.baudrate = 57600;  // 只修改波特率
    
    // 3. 对比配置
    bool same = compare_uart_config(&current_config, &new_config);
    if (!same) {
        ESP_LOGI("EXAMPLE", "配置不同，需要重新配置");
        
        // 4. 重新配置单个通道
        ret = restart_single_channel(2, &new_config);
        if (ret == ESP_OK) {
            ESP_LOGI("EXAMPLE", "通道2重新配置成功");
        }
    }
    
    // 5. 发送数据
    uint8_t data[] = "Hello UART2";
    tx_tasks_to_channel(data, strlen((char*)data), 2);
}
*/

// 新增：OTA前释放UART驱动占用的内存
void deinit_all_uart_drivers_for_ota(void) {
  ESP_LOGI(TAG, "Deinitializing UART drivers for OTA or Reset");
  // 确保RX任务已暂停，避免并发访问驱动
  // 删除UART驱动将释放环形缓冲区与相关资源
  uart_driver_delete(UART_NUM_0);
  uart_driver_delete(UART_NUM_1);
  uart_driver_delete(UART_NUM_2);
}
