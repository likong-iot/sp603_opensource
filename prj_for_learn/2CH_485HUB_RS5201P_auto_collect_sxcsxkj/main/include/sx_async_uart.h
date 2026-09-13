/*
 * @Author: Orion
 * @Date: 2024-05-08 16:08:44
 * @LastEditors: Orion
 * @LastEditTime: 2025-02-14 15:22:44
 * @FilePath: \SERIIAL_SERVER_P\main\include\sx_async_uart.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
#include "hal/uart_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/mpu_wrappers.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
// #include <stdint.h>

// 通道收发LED引脚定义（按通道CH1/CH2/CH3绑定）
// 逻辑通道与UART端口对应关系：CH1 -> UART2，CH2 -> UART0，CH3 -> UART1
// 注：CH1 与 CH3 的 LED 引脚已互换
#define CH1_TX_LED GPIO_NUM_45  // CH1发送指示灯
#define CH1_RX_LED GPIO_NUM_38  // CH1接收指示灯
#define CH2_TX_LED GPIO_NUM_39  // CH2发送指示灯
#define CH2_RX_LED GPIO_NUM_40  // CH2接收指示灯
#define CH3_TX_LED GPIO_NUM_41  // CH3发送指示灯
#define CH3_RX_LED GPIO_NUM_42  // CH3接收指示灯

void uart_init(void);
esp_err_t tx_tasks_to_channel(uint8_t data[], size_t length, int channel);
int sendDataToUart(char *data, size_t length, uart_port_t uart_num);
void rx_task_for_uart(uart_port_t uart_num, void *arg);
void rx_task_for_uart_wrapper(void *arg);
void create_multi_uart_rx_tasks(void);
void suspend_all_uart_rx_tasks(void);
void resume_all_uart_rx_tasks(void);
void stop_all_uart_tasks(void);
esp_err_t send_data_to_channel(int channel, char *data, size_t length);
// 通道缓冲区访问函数
int get_channel_data(int channel, uint8_t* buffer, size_t buffer_size, uint64_t* timestamp);
int pop_channel_data(int channel, uint8_t* buffer, size_t buffer_size, uint64_t* timestamp);
void clear_channel_data(int channel);
extern uint8_t dataArray[];
void uart_reinit(int baudrate1, uart_word_length_t data_bits1, uart_parity_t parity1, uart_stop_bits_t stop_bits1, int frame_time1, int frame_len1,
                 int timeout1,
                 int baudrate2, uart_word_length_t data_bits2, uart_parity_t parity2, uart_stop_bits_t stop_bits2, int frame_time2, int frame_len2,
                 int timeout2,
                 int baudrate3, uart_word_length_t data_bits3, uart_parity_t parity3, uart_stop_bits_t stop_bits3, int frame_time3, int frame_len3,
                 int timeout3);
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
void select_uart_channel(int channel);

void stop_rx_task();
#define UART_TRACE_BUFFER_SIZE 2048
extern uint8_t *uart_response;       // 接收数据缓冲区 - 运行时从PSRAM分配
extern uint8_t *uart_tx_data;        // 发送数据缓冲区 - 运行时从PSRAM分配
extern size_t response_len;          // 接收数据长度
extern size_t tx_data_len;          // 发送数据长度
extern char *hex_data_str;
extern SemaphoreHandle_t xSemaphore;
extern portMUX_TYPE uart_spinlock;



// 通道LED控制函数（参数为通道号：1=CH1, 2=CH2, 3=CH3）
void uart_led_init(void);
void uart_tx_led_on(int channel);
void uart_tx_led_off(int channel);
void uart_rx_led_on(int channel);
void uart_rx_led_off(int channel);

// 添加时间戳结构定义
typedef struct {
    uint64_t tx_timestamp;
    uint64_t rx_timestamp;
} uart_timestamps_t;

// 通道参数结构体
typedef struct {
    int channel;                    // 通道号 (1, 2, 3)
    int baudrate;                   // 波特率
    uart_word_length_t data_bits;   // 数据位
    uart_parity_t parity;           // 校验位
    uart_stop_bits_t stop_bits;     // 停止位
    int frame_time;                 // 帧时间
    int frame_len;                  // 帧长度
    int timeout;                    // 超时时间
} channel_uart_config_t;

// 声明为外部变量
extern uart_timestamps_t uart_timestamps;

// 新增函数声明
// 参数对比方法
bool compare_uart_config(const channel_uart_config_t* config1, const channel_uart_config_t* config2);

// 单个通道重启方法
esp_err_t restart_single_channel(int channel, const channel_uart_config_t* config);

// 轻量级快速重新配置方法（仅配置必要参数，不重启任务）
esp_err_t quick_reconfigure_channel(int channel, const channel_uart_config_t* config);

// 运行时配置管理函数
void set_current_runtime_config(int channel, const channel_uart_config_t* config);
esp_err_t get_current_runtime_config(int channel, channel_uart_config_t* config);
void clear_runtime_config(int channel);

// 从NVS读取配置（仅在初始化或恢复时使用）
esp_err_t get_channel_uart_config_from_nvs(int channel, channel_uart_config_t* config);

// 临时参数发送核心函数：按指定参数发送数据（不持久化配置）
esp_err_t send_data_with_temp_config(int channel, 
                                    const channel_uart_config_t* temp_config,
                                    const uint8_t* data, 
                                    size_t data_len);

// UART配置模式枚举和函数声明
typedef enum {
    UART_CONFIG_MODE_NORMAL = 0,    // 正常模式：使用各自配置
    UART_CONFIG_MODE_SLAVE_FOLLOW   // 从机跟随模式：CH3跟随主机来源配置
} uart_config_mode_t;

// UART配置模式相关函数
uart_config_mode_t get_uart_config_mode(void);
void set_uart_config_mode(uart_config_mode_t mode);

// 智能发送函数：根据工作模式和来源通道智能选择发送参数
esp_err_t smart_send_data_to_ch3(uart_config_mode_t work_mode, 
                                 int source_channel, 
                                 const uint8_t* data, 
                                 size_t data_len);

// 新增：OTA前释放UART驱动占用的内存
void deinit_all_uart_drivers_for_ota(void);

#ifdef __cplusplus
}
#endif
