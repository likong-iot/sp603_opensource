#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "s3_gpio.h"
#include "sdkconfig.h"

/*
 * 项目现实串口映射（无 CH432）
 * CH1 -> COM2 / RS485-2；RS422 布局时为 COM1 TX + COM2 RX
 * CH2 -> RS232
 * CH3 -> COM1 / RS485-1（能力保留；UART0 调试开启时被占用）
 */
#define ASYNC_UART_MAX_CHANNELS 3

#ifndef ASYNC_UART_ENABLE_UART0
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART && \
    defined(CONFIG_ESP_CONSOLE_UART_NUM) && CONFIG_ESP_CONSOLE_UART_NUM == 0
#define ASYNC_UART_ENABLE_UART0 0
#elif defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) && CONFIG_ESP_CONSOLE_UART_DEFAULT
#define ASYNC_UART_ENABLE_UART0 0
#else
#define ASYNC_UART_ENABLE_UART0 1
#endif
#endif

#define ASYNC_UART_BUF_SIZE 5120

/*
 * 串口数据灯映射：每个接口仅有一个数据灯，TX/RX 共用。
 * CH1/UART1 -> COM2 (LED5, RS485-2)
 * CH2/UART2 -> 232  (LED7, RS232)
 * CH3/UART0 -> COM1 (LED3, RS485-1)
 */
#define CH1_TX_LED LED_COM2
#define CH1_RX_LED LED_COM2
#define CH2_TX_LED LED_232
#define CH2_RX_LED LED_232
#define CH3_TX_LED LED_COM1
#define CH3_RX_LED LED_COM1

typedef struct {
    uint64_t tx_timestamp;
    uint64_t rx_timestamp;
} uart_timestamps_t;

typedef struct {
    int channel;
    int baudrate;
    uart_word_length_t data_bits;
    uart_parity_t parity;
    uart_stop_bits_t stop_bits;
    int frame_time; /* ms, 用于收包帧边界 */
    int frame_len;  /* 用于 RX 阈值/缓存规划 */
    int timeout;    /* 协议层超时参数，保留 */
} channel_uart_config_t;

typedef enum {
    UART_CONFIG_MODE_NORMAL = 0,
    UART_CONFIG_MODE_SLAVE_FOLLOW,
} uart_config_mode_t;

extern uint8_t dataArray[];
extern uint8_t uart_response[ASYNC_UART_BUF_SIZE];
extern uint8_t uart_tx_data[ASYNC_UART_BUF_SIZE];
extern size_t response_len;
extern size_t tx_data_len;
extern uart_timestamps_t uart_timestamps;
extern portMUX_TYPE uart_spinlock;

void uart_init(void);
void stop_rx_task(void);

void create_multi_uart_rx_tasks(void);
void suspend_all_uart_rx_tasks(void);
void resume_all_uart_rx_tasks(void);
void stop_all_uart_tasks(void);

int sendDataToUart(char *data, size_t length, uart_port_t uart_num);
void tx_tasks_to_channel(uint8_t data[], size_t length, int channel);
void send_data_to_channel(int channel, char *data, size_t length);

void rx_task_for_uart(uart_port_t uart_num, void *arg);
void rx_task_for_uart_wrapper(void *arg);

int get_channel_data(int channel, uint8_t *buffer, size_t buffer_size, uint64_t *timestamp);
int take_channel_data(int channel, uint8_t *buffer, size_t buffer_size, uint64_t *timestamp);
void clear_channel_data(int channel);

void select_uart_channel(int channel);
void uart_configure(int baudrate, uart_word_length_t data_bits,
                    uart_parity_t parity, uart_stop_bits_t stop_bits,
                    int frame_time, int frame_len, int uart_channel);
void configure_uart0(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len);
void configure_uart1(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len);
void configure_uart2(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len);

/*
 * 兼容旧接口：项目无 CH4/CH5，传入参数会被忽略并记录告警日志。
 */
void uart_reinit(int baudrate1, uart_word_length_t data_bits1, uart_parity_t parity1, uart_stop_bits_t stop_bits1,
                 int frame_time1, int frame_len1,
                 int baudrate2, uart_word_length_t data_bits2, uart_parity_t parity2, uart_stop_bits_t stop_bits2,
                 int frame_time2, int frame_len2,
                 int baudrate3, uart_word_length_t data_bits3, uart_parity_t parity3, uart_stop_bits_t stop_bits3,
                 int frame_time3, int frame_len3,
                 int baudrate4, uart_word_length_t data_bits4, uart_parity_t parity4, uart_stop_bits_t stop_bits4,
                 int frame_time4, int frame_len4,
                 int baudrate5, uart_word_length_t data_bits5, uart_parity_t parity5, uart_stop_bits_t stop_bits5,
                 int frame_time5, int frame_len5);

bool compare_uart_config(const channel_uart_config_t *config1, const channel_uart_config_t *config2);
esp_err_t restart_single_channel(int channel, const channel_uart_config_t *config);
esp_err_t quick_reconfigure_channel(int channel, const channel_uart_config_t *config);

void set_current_runtime_config(int channel, const channel_uart_config_t *config);
esp_err_t get_current_runtime_config(int channel, channel_uart_config_t *config);
void clear_runtime_config(int channel);
esp_err_t get_channel_uart_config_from_nvs(int channel, channel_uart_config_t *config);

esp_err_t send_data_with_temp_config(int channel,
                                     const channel_uart_config_t *temp_config,
                                     const uint8_t *data,
                                     size_t data_len);

void uart_led_init(void);
void uart_tx_led_on(int uart_num);
void uart_tx_led_off(int uart_num);
void uart_rx_led_on(int uart_num);
void uart_rx_led_off(int uart_num);
void uart_tx_led_on_by_channel(int channel);
void uart_tx_led_off_by_channel(int channel);
void uart_rx_led_on_by_channel(int channel);
void uart_rx_led_off_by_channel(int channel);
void uart_led_boot_animation(void);
void uart_led_reset_animation(void);

uart_config_mode_t get_uart_config_mode(void);
void set_uart_config_mode(uart_config_mode_t mode);

/* 兼容旧业务接口：本项目将“总线发送口”映射为 CH2(UART2) */
esp_err_t smart_send_data_to_ch5(uart_config_mode_t work_mode,
                                 int source_channel,
                                 const uint8_t *data,
                                 size_t data_len);

void deinit_all_uart_drivers_for_ota(void);

#ifdef __cplusplus
}
#endif
