#include "async_uart.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_check.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_web_server.h"
#include "sx_work_mode.h"
#include "sx_serial_port_manager.h"
#include "sx_serial_resource.h"
#include "app_task_utils.h"

#define TAG "async_uart"

#define DEFAULT_BAUDRATE     9600
#define DEFAULT_DATA_BITS    UART_DATA_8_BITS
#define DEFAULT_PARITY       UART_PARITY_DISABLE
#define DEFAULT_STOP_BITS    UART_STOP_BITS_1
#define DEFAULT_FRAME_TIME   50
#define DEFAULT_FRAME_LEN    512
#define DEFAULT_TIMEOUT      1000

#define TX_WAIT_PRIMARY_MS   1000
#define TX_WAIT_RETRY_MS     500

typedef struct {
    int channel;
    bool enabled;
    uart_port_t port;
    gpio_num_t tx_pin;
    gpio_num_t rx_pin;
    gpio_num_t tx_led;
    gpio_num_t rx_led;
    const char *name;
} uart_channel_hw_t;

typedef struct {
    uint8_t data[ASYNC_UART_BUF_SIZE];
    size_t length;
    uint64_t timestamp;
    bool has_data;
} channel_buffer_t;

static uart_channel_hw_t s_channel_hw[ASYNC_UART_MAX_CHANNELS] = {
    {
        .channel = 1,
        .enabled = true,
        .port = UART_NUM_1,
        .tx_pin = U1TXD,
        .rx_pin = U1RXD,
        .tx_led = CH1_TX_LED,
        .rx_led = CH1_RX_LED,
        .name = "COM2 / RS485-2 (CH1/UART1)",
    },
    {
        .channel = 2,
        .enabled = true,
        .port = UART_NUM_2,
        .tx_pin = U2TXD,
        .rx_pin = U2RXD,
        .tx_led = CH2_TX_LED,
        .rx_led = CH2_RX_LED,
        .name = "RS232 (CH2/UART2)",
    },
    {
        .channel = 3,
#if ASYNC_UART_ENABLE_UART0
        .enabled = true,
#else
        .enabled = false,
#endif
        .port = UART_NUM_0,
        .tx_pin = U0TXD,
        .rx_pin = U0RXD,
        .tx_led = CH3_TX_LED,
        .rx_led = CH3_RX_LED,
        .name = "COM1 / RS485-1 (CH3/UART0)",
    },
};
static bool s_tx_led_active[ASYNC_UART_MAX_CHANNELS] = {false};
static bool s_rx_led_active[ASYNC_UART_MAX_CHANNELS] = {false};
static portMUX_TYPE s_led_spinlock = portMUX_INITIALIZER_UNLOCKED;

uint8_t dataArray[] = {0};
EXT_RAM_BSS_ATTR uint8_t uart_response[ASYNC_UART_BUF_SIZE] = {0};
EXT_RAM_BSS_ATTR uint8_t uart_tx_data[ASYNC_UART_BUF_SIZE] = {0};
size_t response_len = 0;
size_t tx_data_len = 0;
uart_timestamps_t uart_timestamps = {0};
portMUX_TYPE uart_spinlock = portMUX_INITIALIZER_UNLOCKED;

static EXT_RAM_BSS_ATTR channel_buffer_t s_channel_buffers[ASYNC_UART_MAX_CHANNELS] = {0};
static portMUX_TYPE s_channel_spinlock = portMUX_INITIALIZER_UNLOCKED;

static channel_uart_config_t s_runtime_config[ASYNC_UART_MAX_CHANNELS] = {0};
static bool s_runtime_config_valid[ASYNC_UART_MAX_CHANNELS] = {false};
static portMUX_TYPE s_runtime_spinlock = portMUX_INITIALIZER_UNLOCKED;

static TaskHandle_t s_rx_task_handles[ASYNC_UART_MAX_CHANNELS] = {NULL};

static volatile bool s_reinit_in_progress = false;
static portMUX_TYPE s_reinit_spinlock = portMUX_INITIALIZER_UNLOCKED;

static volatile bool s_led_animation_active = false;
static uart_config_mode_t s_uart_config_mode = UART_CONFIG_MODE_NORMAL;

static int channel_to_index(int channel)
{
    if (channel < 1 || channel > ASYNC_UART_MAX_CHANNELS) {
        return -1;
    }
    return channel - 1;
}

static int channel_from_uart_num(uart_port_t uart_num)
{
    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        if (s_channel_hw[i].port == uart_num) {
            return s_channel_hw[i].channel;
        }
    }
    return -1;
}

static const uart_channel_hw_t *get_channel_hw(int channel)
{
    int idx = channel_to_index(channel);
    if (idx < 0 || !s_channel_hw[idx].enabled) {
        return NULL;
    }
    return &s_channel_hw[idx];
}

static inline void set_reinit_in_progress(bool in_progress)
{
    portENTER_CRITICAL(&s_reinit_spinlock);
    s_reinit_in_progress = in_progress;
    portEXIT_CRITICAL(&s_reinit_spinlock);
}

static inline bool is_reinit_in_progress(void)
{
    bool in_progress;
    portENTER_CRITICAL(&s_reinit_spinlock);
    in_progress = s_reinit_in_progress;
    portEXIT_CRITICAL(&s_reinit_spinlock);
    return in_progress;
}

static uart_word_length_t normalize_data_bits(uart_word_length_t data_bits)
{
    switch (data_bits) {
    case UART_DATA_5_BITS:
    case UART_DATA_6_BITS:
    case UART_DATA_7_BITS:
    case UART_DATA_8_BITS:
        return data_bits;
    default:
        break;
    }

    switch ((int)data_bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_parity_t normalize_parity(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_DISABLE:
    case UART_PARITY_ODD:
    case UART_PARITY_EVEN:
        return parity;
    default:
        return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t normalize_stop_bits(uart_stop_bits_t stop_bits)
{
    switch (stop_bits) {
    case UART_STOP_BITS_1:
    case UART_STOP_BITS_1_5:
    case UART_STOP_BITS_2:
        return stop_bits;
    default:
        return UART_STOP_BITS_1;
    }
}

static int normalize_frame_time(int frame_time)
{
    if (frame_time < 1) {
        return 1;
    }
    if (frame_time > 1000) {
        return 1000;
    }
    return frame_time;
}

static int normalize_frame_len(int frame_len)
{
    if (frame_len < 1) {
        return 1;
    }
    if (frame_len > ASYNC_UART_BUF_SIZE) {
        return ASYNC_UART_BUF_SIZE;
    }
    return frame_len;
}

static int calc_rx_buffer_size(int frame_len)
{
    int n = normalize_frame_len(frame_len);
    if (n <= 256) {
        return 1024;
    }
    if (n <= 512) {
        return 2048;
    }
    if (n <= 1024) {
        return 4096;
    }
    return ASYNC_UART_BUF_SIZE;
}

static gpio_num_t tx_led_pin_from_channel(int channel)
{
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (!hw) {
        return GPIO_NUM_NC;
    }
    return hw->tx_led;
}

static gpio_num_t rx_led_pin_from_channel(int channel)
{
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (!hw) {
        return GPIO_NUM_NC;
    }
    return hw->rx_led;
}

static void uart_led_set_level(gpio_num_t pin, uint32_t level, bool force)
{
    if (pin == GPIO_NUM_NC) {
        return;
    }
    if (!force && s_led_animation_active) {
        return;
    }
    gpio_set_level(pin, level);
}

void uart_tx_led_on_by_channel(int channel)
{
    int idx = channel_to_index(channel);
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (idx < 0 || hw == NULL) return;
    portENTER_CRITICAL(&s_led_spinlock);
    s_tx_led_active[idx] = true;
    bool level = (s_tx_led_active[idx] ||
                  (hw->tx_led == hw->rx_led && s_rx_led_active[idx])) ? 0 : 1;
    portEXIT_CRITICAL(&s_led_spinlock);
    uart_led_set_level(hw->tx_led, level, false);
}

void uart_tx_led_off_by_channel(int channel)
{
    int idx = channel_to_index(channel);
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (idx < 0 || hw == NULL) return;
    portENTER_CRITICAL(&s_led_spinlock);
    s_tx_led_active[idx] = false;
    bool level = (s_tx_led_active[idx] ||
                  (hw->tx_led == hw->rx_led && s_rx_led_active[idx])) ? 0 : 1;
    portEXIT_CRITICAL(&s_led_spinlock);
    uart_led_set_level(hw->tx_led, level, false);
}

void uart_rx_led_on_by_channel(int channel)
{
    int idx = channel_to_index(channel);
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (idx < 0 || hw == NULL) return;
    portENTER_CRITICAL(&s_led_spinlock);
    s_rx_led_active[idx] = true;
    bool level = (s_rx_led_active[idx] ||
                  (hw->tx_led == hw->rx_led && s_tx_led_active[idx])) ? 0 : 1;
    portEXIT_CRITICAL(&s_led_spinlock);
    uart_led_set_level(hw->rx_led, level, false);
}

void uart_rx_led_off_by_channel(int channel)
{
    int idx = channel_to_index(channel);
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (idx < 0 || hw == NULL) return;
    portENTER_CRITICAL(&s_led_spinlock);
    s_rx_led_active[idx] = false;
    bool level = (s_rx_led_active[idx] ||
                  (hw->tx_led == hw->rx_led && s_tx_led_active[idx])) ? 0 : 1;
    portEXIT_CRITICAL(&s_led_spinlock);
    uart_led_set_level(hw->rx_led, level, false);
}

void uart_tx_led_on(int uart_num)
{
    int channel = channel_from_uart_num((uart_port_t)uart_num);
    if (channel > 0) {
        uart_tx_led_on_by_channel(channel);
    }
}

void uart_tx_led_off(int uart_num)
{
    int channel = channel_from_uart_num((uart_port_t)uart_num);
    if (channel > 0) {
        uart_tx_led_off_by_channel(channel);
    }
}

void uart_rx_led_on(int uart_num)
{
    int channel = channel_from_uart_num((uart_port_t)uart_num);
    if (channel > 0) {
        uart_rx_led_on_by_channel(channel);
    }
}

void uart_rx_led_off(int uart_num)
{
    int channel = channel_from_uart_num((uart_port_t)uart_num);
    if (channel > 0) {
        uart_rx_led_off_by_channel(channel);
    }
}

void uart_led_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        if (!s_channel_hw[i].enabled) {
            continue;
        }
        if (s_channel_hw[i].tx_led != GPIO_NUM_NC) {
            mask |= (1ULL << s_channel_hw[i].tx_led);
        }
        if (s_channel_hw[i].rx_led != GPIO_NUM_NC) {
            mask |= (1ULL << s_channel_hw[i].rx_led);
        }
    }

    if (mask == 0) {
        return;
    }

    gpio_config_t led_cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&led_cfg);

    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        if (!s_channel_hw[i].enabled) {
            continue;
        }
        uart_tx_led_off_by_channel(s_channel_hw[i].channel);
        uart_rx_led_off_by_channel(s_channel_hw[i].channel);
    }
}

void uart_led_boot_animation(void)
{
    s_led_animation_active = true;
    for (int ch = 1; ch <= ASYNC_UART_MAX_CHANNELS; ch++) {
        uart_led_set_level(rx_led_pin_from_channel(ch), 0, true);
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    for (int ch = 1; ch <= ASYNC_UART_MAX_CHANNELS; ch++) {
        uart_led_set_level(rx_led_pin_from_channel(ch), 1, true);
    }
    s_led_animation_active = false;
}

void uart_led_reset_animation(void)
{
    s_led_animation_active = true;
    for (int ch = 1; ch <= ASYNC_UART_MAX_CHANNELS; ch++) {
        uart_led_set_level(tx_led_pin_from_channel(ch), 0, true);
        uart_led_set_level(rx_led_pin_from_channel(ch), 0, true);
    }
    vTaskDelay(pdMS_TO_TICKS(120));
    for (int ch = 1; ch <= ASYNC_UART_MAX_CHANNELS; ch++) {
        uart_led_set_level(tx_led_pin_from_channel(ch), 1, true);
        uart_led_set_level(rx_led_pin_from_channel(ch), 1, true);
    }
    s_led_animation_active = false;
}

void set_current_runtime_config(int channel, const channel_uart_config_t *config)
{
    int idx = channel_to_index(channel);
    if (idx < 0 || config == NULL) {
        return;
    }

    portENTER_CRITICAL(&s_runtime_spinlock);
    s_runtime_config[idx] = *config;
    s_runtime_config_valid[idx] = true;
    portEXIT_CRITICAL(&s_runtime_spinlock);
}

esp_err_t get_current_runtime_config(int channel, channel_uart_config_t *config)
{
    int idx = channel_to_index(channel);
    if (idx < 0 || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    bool valid;
    portENTER_CRITICAL(&s_runtime_spinlock);
    valid = s_runtime_config_valid[idx];
    if (valid) {
        *config = s_runtime_config[idx];
    }
    portEXIT_CRITICAL(&s_runtime_spinlock);

    return valid ? ESP_OK : ESP_ERR_NOT_FOUND;
}

void clear_runtime_config(int channel)
{
    int idx = channel_to_index(channel);
    if (idx < 0) {
        return;
    }

    portENTER_CRITICAL(&s_runtime_spinlock);
    s_runtime_config_valid[idx] = false;
    memset(&s_runtime_config[idx], 0, sizeof(s_runtime_config[idx]));
    portEXIT_CRITICAL(&s_runtime_spinlock);
}

static void clear_all_channel_buffers(void)
{
    portENTER_CRITICAL(&s_channel_spinlock);
    memset(s_channel_buffers, 0, sizeof(s_channel_buffers));
    portEXIT_CRITICAL(&s_channel_spinlock);
}

void clear_channel_data(int channel)
{
    int idx = channel_to_index(channel);
    if (idx < 0) {
        return;
    }

    portENTER_CRITICAL(&s_channel_spinlock);
    memset(s_channel_buffers[idx].data, 0, sizeof(s_channel_buffers[idx].data));
    s_channel_buffers[idx].length = 0;
    s_channel_buffers[idx].timestamp = 0;
    s_channel_buffers[idx].has_data = false;
    portEXIT_CRITICAL(&s_channel_spinlock);
}

int get_channel_data(int channel, uint8_t *buffer, size_t buffer_size, uint64_t *timestamp)
{
    int idx = channel_to_index(channel);
    if (idx < 0 || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    size_t copy_size = 0;
    uint64_t ts = 0;

    portENTER_CRITICAL(&s_channel_spinlock);
    if (s_channel_buffers[idx].has_data) {
        copy_size = s_channel_buffers[idx].length;
        if (copy_size > buffer_size) {
            copy_size = buffer_size;
        }
        memcpy(buffer, s_channel_buffers[idx].data, copy_size);
        ts = s_channel_buffers[idx].timestamp;
    }
    portEXIT_CRITICAL(&s_channel_spinlock);

    if (copy_size == 0) {
        return 0;
    }

    if (timestamp != NULL) {
        *timestamp = ts;
    }

    return (int)copy_size;
}

int take_channel_data(int channel, uint8_t *buffer, size_t buffer_size, uint64_t *timestamp)
{
    int idx = channel_to_index(channel);
    if (idx < 0 || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    size_t copy_size = 0;
    uint64_t ts = 0;

    portENTER_CRITICAL(&s_channel_spinlock);
    if (s_channel_buffers[idx].has_data) {
        copy_size = s_channel_buffers[idx].length;
        if (copy_size > buffer_size) {
            copy_size = buffer_size;
        }
        memcpy(buffer, s_channel_buffers[idx].data, copy_size);
        ts = s_channel_buffers[idx].timestamp;

        s_channel_buffers[idx].length = 0;
        s_channel_buffers[idx].timestamp = 0;
        s_channel_buffers[idx].has_data = false;
    }
    portEXIT_CRITICAL(&s_channel_spinlock);

    if (copy_size == 0) {
        return 0;
    }

    if (timestamp != NULL) {
        *timestamp = ts;
    }

    return (int)copy_size;
}

static const char *parity_to_nvs_string(uart_parity_t parity)
{
    switch (normalize_parity(parity)) {
    case UART_PARITY_ODD:
        return "1";
    case UART_PARITY_EVEN:
        return "2";
    case UART_PARITY_DISABLE:
    default:
        return "0";
    }
}

static uart_parity_t parse_parity_from_string(const char *value)
{
    if (value == NULL) {
        return UART_PARITY_DISABLE;
    }

    if (strcmp(value, "0") == 0 || strcasecmp(value, "none") == 0) {
        return UART_PARITY_DISABLE;
    }
    if (strcmp(value, "1") == 0 || strcasecmp(value, "odd") == 0) {
        return UART_PARITY_ODD;
    }
    if (strcmp(value, "2") == 0 || strcasecmp(value, "even") == 0) {
        return UART_PARITY_EVEN;
    }

    return UART_PARITY_DISABLE;
}

static const char *stop_bits_to_nvs_string(uart_stop_bits_t stop_bits)
{
    switch (normalize_stop_bits(stop_bits)) {
    case UART_STOP_BITS_1_5:
        return "1.5";
    case UART_STOP_BITS_2:
        return "2";
    case UART_STOP_BITS_1:
    default:
        return "1";
    }
}

static void fill_default_config(int channel, channel_uart_config_t *config)
{
    if (!config) {
        return;
    }

    config->channel = channel;
    config->baudrate = DEFAULT_BAUDRATE;
    config->data_bits = DEFAULT_DATA_BITS;
    config->parity = DEFAULT_PARITY;
    config->stop_bits = DEFAULT_STOP_BITS;
    config->frame_time = DEFAULT_FRAME_TIME;
    config->frame_len = DEFAULT_FRAME_LEN;
    config->timeout = DEFAULT_TIMEOUT;
}

static esp_err_t write_channel_config_to_nvs(const channel_uart_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open("storage", NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[32];
    char value[24];

    snprintf(key, sizeof(key), "ch%d_baud_rate", config->channel);
    snprintf(value, sizeof(value), "%d", config->baudrate);
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, value), out, TAG, "set %s failed", key);

    snprintf(key, sizeof(key), "ch%d_data_bit", config->channel);
    snprintf(value, sizeof(value), "%d", (int)normalize_data_bits(config->data_bits) + 5);
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, value), out, TAG, "set %s failed", key);

    snprintf(key, sizeof(key), "ch%d_check_bit", config->channel);
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, parity_to_nvs_string(config->parity)), out, TAG, "set %s failed", key);

    snprintf(key, sizeof(key), "ch%d_stop_bit", config->channel);
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, stop_bits_to_nvs_string(config->stop_bits)), out, TAG, "set %s failed", key);

    snprintf(key, sizeof(key), "ch%d_frame_time", config->channel);
    snprintf(value, sizeof(value), "%d", normalize_frame_time(config->frame_time));
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, value), out, TAG, "set %s failed", key);

    snprintf(key, sizeof(key), "ch%d_frame_len", config->channel);
    snprintf(value, sizeof(value), "%d", normalize_frame_len(config->frame_len));
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, value), out, TAG, "set %s failed", key);

    snprintf(key, sizeof(key), "ch%d_timeout", config->channel);
    snprintf(value, sizeof(value), "%d", config->timeout > 0 ? config->timeout : DEFAULT_TIMEOUT);
    ESP_GOTO_ON_ERROR(nvs_set_str(handle, key, value), out, TAG, "set %s failed", key);

    ret = nvs_commit(handle);

out:
    nvs_close(handle);
    return ret;
}

static esp_err_t nvs_get_str_fixed(nvs_handle_t handle, const char *key, char *out, size_t out_size)
{
    size_t required = 0;
    esp_err_t err = nvs_get_str(handle, key, NULL, &required);
    if (err != ESP_OK) {
        return err;
    }

    if (required == 0 || required > out_size) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    return nvs_get_str(handle, key, out, &required);
}

esp_err_t get_channel_uart_config_from_nvs(int channel, channel_uart_config_t *config)
{
    int idx = channel_to_index(channel);
    if (idx < 0 || config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uart_channel_hw_t *hw = &s_channel_hw[idx];
    if (!hw->enabled) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    nvs_handle_t handle;
    esp_err_t ret = nvs_open("storage", NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        return ret;
    }

    char key[32];
    char value[24];
    channel_uart_config_t cfg;
    fill_default_config(channel, &cfg);

    snprintf(key, sizeof(key), "ch%d_baud_rate", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    cfg.baudrate = atoi(value);

    snprintf(key, sizeof(key), "ch%d_data_bit", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    cfg.data_bits = normalize_data_bits((uart_word_length_t)atoi(value));

    snprintf(key, sizeof(key), "ch%d_check_bit", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    cfg.parity = parse_parity_from_string(value);

    snprintf(key, sizeof(key), "ch%d_stop_bit", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    if (strcmp(value, "1.5") == 0) {
        cfg.stop_bits = UART_STOP_BITS_1_5;
    } else if (strcmp(value, "2") == 0) {
        cfg.stop_bits = UART_STOP_BITS_2;
    } else {
        cfg.stop_bits = UART_STOP_BITS_1;
    }

    snprintf(key, sizeof(key), "ch%d_frame_time", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    cfg.frame_time = normalize_frame_time(atoi(value));

    snprintf(key, sizeof(key), "ch%d_frame_len", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    cfg.frame_len = normalize_frame_len(atoi(value));

    snprintf(key, sizeof(key), "ch%d_timeout", channel);
    ESP_GOTO_ON_ERROR(nvs_get_str_fixed(handle, key, value, sizeof(value)), out, TAG, "%s not found", key);
    cfg.timeout = atoi(value);
    if (cfg.timeout <= 0) {
        cfg.timeout = DEFAULT_TIMEOUT;
    }

    *config = cfg;

out:
    nvs_close(handle);
    return ret;
}

bool compare_uart_config(const channel_uart_config_t *config1, const channel_uart_config_t *config2)
{
    if (config1 == NULL || config2 == NULL) {
        return false;
    }

    return (config1->channel == config2->channel) &&
           (config1->baudrate == config2->baudrate) &&
           (normalize_data_bits(config1->data_bits) == normalize_data_bits(config2->data_bits)) &&
           (normalize_parity(config1->parity) == normalize_parity(config2->parity)) &&
           (normalize_stop_bits(config1->stop_bits) == normalize_stop_bits(config2->stop_bits)) &&
           (normalize_frame_time(config1->frame_time) == normalize_frame_time(config2->frame_time)) &&
           (normalize_frame_len(config1->frame_len) == normalize_frame_len(config2->frame_len)) &&
           (config1->timeout == config2->timeout);
}

static esp_err_t apply_channel_config_internal(const channel_uart_config_t *config, bool reinstall_driver)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uart_channel_hw_t *hw = get_channel_hw(config->channel);
    if (hw == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    channel_uart_config_t cfg = *config;
    cfg.data_bits = normalize_data_bits(cfg.data_bits);
    cfg.parity = normalize_parity(cfg.parity);
    cfg.stop_bits = normalize_stop_bits(cfg.stop_bits);
    cfg.frame_time = normalize_frame_time(cfg.frame_time);
    cfg.frame_len = normalize_frame_len(cfg.frame_len);
    if (cfg.timeout <= 0) {
        cfg.timeout = DEFAULT_TIMEOUT;
    }

    uart_config_t uart_cfg = {
        .baud_rate = cfg.baudrate,
        .data_bits = cfg.data_bits,
        .parity = cfg.parity,
        .stop_bits = cfg.stop_bits,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    int rx_threshold = cfg.frame_len;
    if (rx_threshold < 1) {
        rx_threshold = 1;
    }
    if (rx_threshold > 120) {
        rx_threshold = 120;
    }

    int rx_buffer_size = calc_rx_buffer_size(cfg.frame_len);

    if (reinstall_driver) {
        (void)uart_driver_delete(hw->port);
    }

    ESP_RETURN_ON_ERROR(uart_param_config(hw->port, &uart_cfg), TAG, "param config failed for %s", hw->name);
    ESP_RETURN_ON_ERROR(uart_set_pin(hw->port, hw->tx_pin, hw->rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "set pin failed for %s", hw->name);

    if (reinstall_driver) {
        esp_err_t err = uart_driver_install(hw->port, rx_buffer_size, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s install rx=%d failed(%s), retry with 512", hw->name, rx_buffer_size, esp_err_to_name(err));
            err = uart_driver_install(hw->port, 512, 0, 0, NULL, 0);
            ESP_RETURN_ON_ERROR(err, TAG, "driver install failed for %s", hw->name);
        }
    }

    ESP_RETURN_ON_ERROR(uart_set_mode(hw->port, UART_MODE_UART), TAG, "set mode failed for %s", hw->name);
    ESP_RETURN_ON_ERROR(uart_set_rx_timeout(hw->port, 3), TAG, "set timeout failed for %s", hw->name);
    ESP_RETURN_ON_ERROR(uart_set_rx_full_threshold(hw->port, rx_threshold), TAG, "set threshold failed for %s", hw->name);

    uart_flush(hw->port);
    clear_channel_data(cfg.channel);
    set_current_runtime_config(cfg.channel, &cfg);

    ESP_LOGI(TAG, "%s configured: baud=%d data=%d parity=%d stop=%d frame_time=%d frame_len=%d",
             hw->name,
             cfg.baudrate,
             (int)cfg.data_bits,
             (int)cfg.parity,
             (int)cfg.stop_bits,
             cfg.frame_time,
             cfg.frame_len);

    return ESP_OK;
}

esp_err_t restart_single_channel(int channel, const channel_uart_config_t *config)
{
    if (config == NULL || config->channel != channel) {
        return ESP_ERR_INVALID_ARG;
    }

    bool manage_tasks = !is_reinit_in_progress();
    if (manage_tasks) {
        suspend_all_uart_rx_tasks();
    }

    esp_err_t ret = apply_channel_config_internal(config, true);
    if (manage_tasks) {
        resume_all_uart_rx_tasks();
    }

    return ret;
}

esp_err_t quick_reconfigure_channel(int channel, const channel_uart_config_t *config)
{
    if (config == NULL || config->channel != channel) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = apply_channel_config_internal(config, false);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "quick reconfigure CH%d failed(%s), fallback restart", channel, esp_err_to_name(ret));
        return restart_single_channel(channel, config);
    }

    return ESP_OK;
}

void configure_uart0(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len)
{
    channel_uart_config_t cfg = {
        .channel = 3,
        .baudrate = baudrate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .frame_time = frame_time,
        .frame_len = frame_len,
        .timeout = DEFAULT_TIMEOUT,
    };

    esp_err_t ret = restart_single_channel(3, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "configure_uart0 skipped: %s", esp_err_to_name(ret));
    }
}

void configure_uart1(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len)
{
    channel_uart_config_t cfg = {
        .channel = 1,
        .baudrate = baudrate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .frame_time = frame_time,
        .frame_len = frame_len,
        .timeout = DEFAULT_TIMEOUT,
    };

    esp_err_t ret = restart_single_channel(1, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "configure_uart1 failed: %s", esp_err_to_name(ret));
    }
}

void configure_uart2(int baudrate, uart_word_length_t data_bits,
                     uart_parity_t parity, uart_stop_bits_t stop_bits,
                     int frame_time, int frame_len)
{
    channel_uart_config_t cfg = {
        .channel = 2,
        .baudrate = baudrate,
        .data_bits = data_bits,
        .parity = parity,
        .stop_bits = stop_bits,
        .frame_time = frame_time,
        .frame_len = frame_len,
        .timeout = DEFAULT_TIMEOUT,
    };

    esp_err_t ret = restart_single_channel(2, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "configure_uart2 failed: %s", esp_err_to_name(ret));
    }
}

void uart_configure(int baudrate, uart_word_length_t data_bits,
                    uart_parity_t parity, uart_stop_bits_t stop_bits,
                    int frame_time, int frame_len, int uart_channel)
{
    switch (uart_channel) {
    case 1:
        configure_uart1(baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
        break;
    case 2:
        configure_uart2(baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
        break;
    case 3:
        configure_uart0(baudrate, data_bits, parity, stop_bits, frame_time, frame_len);
        break;
    default:
        ESP_LOGW(TAG, "uart_configure invalid channel=%d", uart_channel);
        break;
    }
}

void select_uart_channel(int channel)
{
    ESP_LOGI(TAG, "select_uart_channel(%d): this project uses explicit channel routing", channel);
}

static void update_tx_mirror(const uint8_t *data, size_t length)
{
    size_t mirror_len = length;
    if (mirror_len > sizeof(uart_tx_data)) {
        mirror_len = sizeof(uart_tx_data);
    }

    uart_timestamps.tx_timestamp = esp_timer_get_time();

    portENTER_CRITICAL(&uart_spinlock);
    memset(uart_tx_data, 0, sizeof(uart_tx_data));
    if (data != NULL && mirror_len > 0) {
        memcpy(uart_tx_data, data, mirror_len);
    }
    tx_data_len = mirror_len;
    portEXIT_CRITICAL(&uart_spinlock);
}

int sendDataToUart(char *data, size_t length, uart_port_t uart_num)
{
    if (data == NULL || length == 0) {
        return 0;
    }

    update_tx_mirror((const uint8_t *)data, length);

    int tx_bytes = uart_write_bytes(uart_num, data, length);
    if (tx_bytes <= 0) {
        return tx_bytes;
    }

    esp_err_t err = uart_wait_tx_done(uart_num, pdMS_TO_TICKS(TX_WAIT_PRIMARY_MS));
    if (err != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(10));
        (void)uart_wait_tx_done(uart_num, pdMS_TO_TICKS(TX_WAIT_RETRY_MS));
    }

    send_uart_to_websocket_from_port((const uint8_t *)data, (size_t)tx_bytes, true, uart_num);

    return tx_bytes;
}

void tx_tasks_to_channel(uint8_t data[], size_t length, int channel)
{
    const int physical_tx_channel = sx_serial_resource_tx_channel(channel);
    const uart_channel_hw_t *hw = get_channel_hw(physical_tx_channel);
    if (hw == NULL || data == NULL || length == 0) {
        ESP_LOGW(TAG, "logical CH%d has no TX resource (physical=%d)", channel, physical_tx_channel);
        return;
    }

    uart_tx_led_on_by_channel(physical_tx_channel);
    (void)sendDataToUart((char *)data, length, hw->port);
    uart_tx_led_off_by_channel(physical_tx_channel);
}

void send_data_to_channel(int channel, char *data, size_t length)
{
    tx_tasks_to_channel((uint8_t *)data, length, channel);
}

static void rx_task_for_channel_loop(int channel)
{
    const uart_channel_hw_t *hw = get_channel_hw(channel);
    if (!hw) {
        return;
    }

    uint8_t *frame_buf = (uint8_t *)heap_caps_malloc(ASYNC_UART_BUF_SIZE,
                                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buf) {
        frame_buf = (uint8_t *)malloc(ASYNC_UART_BUF_SIZE);
    }
    if (!frame_buf) {
        ESP_LOGE(TAG, "malloc frame buffer failed for CH%d", channel);
        return;
    }

    while (true) {
        if (is_reinit_in_progress()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        channel_uart_config_t cfg;
        if (get_current_runtime_config(channel, &cfg) != ESP_OK) {
            fill_default_config(channel, &cfg);
        }

        int frame_time_ms = normalize_frame_time(cfg.frame_time);

        int first = uart_read_bytes(hw->port, frame_buf, ASYNC_UART_BUF_SIZE, pdMS_TO_TICKS(20));
        if (first <= 0) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        size_t total = (size_t)first;
        uint64_t last_rx_us = esp_timer_get_time();

        while (total < ASYNC_UART_BUF_SIZE) {
            int chunk = uart_read_bytes(hw->port,
                                        frame_buf + total,
                                        ASYNC_UART_BUF_SIZE - total,
                                        pdMS_TO_TICKS(1));
            if (chunk > 0) {
                total += (size_t)chunk;
                last_rx_us = esp_timer_get_time();
                continue;
            }

            if ((esp_timer_get_time() - last_rx_us) >= ((uint64_t)frame_time_ms * 1000ULL)) {
                break;
            }
        }

        /* Indicate a completed frame, not every byte/read wake-up.  This
         * prevents COM2 from appearing continuously active on line noise. */
        uart_rx_led_on_by_channel(channel);

        uart_timestamps.rx_timestamp = esp_timer_get_time();

        portENTER_CRITICAL(&uart_spinlock);
        memset(uart_response, 0, sizeof(uart_response));
        memcpy(uart_response, frame_buf, total);
        response_len = total;
        portEXIT_CRITICAL(&uart_spinlock);

        int idx = channel_to_index(channel);
        if (idx >= 0) {
            portENTER_CRITICAL(&s_channel_spinlock);
            memcpy(s_channel_buffers[idx].data, frame_buf, total);
            s_channel_buffers[idx].length = total;
            s_channel_buffers[idx].timestamp = uart_timestamps.rx_timestamp;
            s_channel_buffers[idx].has_data = true;
            portEXIT_CRITICAL(&s_channel_spinlock);
        }

        const int logical_channel = sx_serial_resource_logical_from_physical_rx(channel);
        send_uart_to_websocket_async(frame_buf, total, false, logical_channel);
        sx_work_mode_handle_uart_data(logical_channel, frame_buf, total);

        vTaskDelay(pdMS_TO_TICKS(20));
        uart_rx_led_off_by_channel(channel);
    }

    free(frame_buf);
}

void rx_task_for_uart(uart_port_t uart_num, void *arg)
{
    (void)arg;
    int channel = channel_from_uart_num(uart_num);
    if (channel < 1) {
        ESP_LOGE(TAG, "rx_task_for_uart invalid uart_num=%d", uart_num);
        delete_self_app_task_with_caps();
        return;
    }

    rx_task_for_channel_loop(channel);
}

void rx_task_for_uart_wrapper(void *arg)
{
    uart_port_t uart_num = (uart_port_t)(intptr_t)arg;
    rx_task_for_uart(uart_num, NULL);
}

void create_multi_uart_rx_tasks(void)
{
    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        const uart_channel_hw_t *hw = &s_channel_hw[i];
        if (!hw->enabled) {
            continue;
        }
        if (s_rx_task_handles[i] != NULL) {
            continue;
        }
        /* RS422 consumes COM1 TX + COM2 RX; UART0 is TX-only in this layout. */
        if (sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422 &&
            hw->channel == 3) {
            continue;
        }

        char name[20];
        snprintf(name, sizeof(name), "rx_uart_ch%d", hw->channel);
        BaseType_t ok = create_app_task_psram(rx_task_for_uart_wrapper,
                                              name,
                                              4096,
                                              (void *)(intptr_t)hw->port,
                                              10,
                                              &s_rx_task_handles[i],
                                              tskNO_AFFINITY);
        if (ok != pdPASS) {
            s_rx_task_handles[i] = NULL;
            ESP_LOGE(TAG, "create RX task failed for %s", hw->name);
        }
    }
}

void suspend_all_uart_rx_tasks(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;
    }

    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        TaskHandle_t h = s_rx_task_handles[i];
        if (h && eTaskGetState(h) != eDeleted) {
            vTaskSuspend(h);
        }
    }
}

void resume_all_uart_rx_tasks(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;
    }

    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        TaskHandle_t h = s_rx_task_handles[i];
        if (h && eTaskGetState(h) == eSuspended) {
            vTaskResume(h);
        }
    }
}

void stop_all_uart_tasks(void)
{
    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        TaskHandle_t h = s_rx_task_handles[i];
        if (h && eTaskGetState(h) != eDeleted) {
            delete_app_task_with_caps(h);
        }
        s_rx_task_handles[i] = NULL;
    }

    for (int ch = 1; ch <= ASYNC_UART_MAX_CHANNELS; ch++) {
        uart_tx_led_off_by_channel(ch);
        uart_rx_led_off_by_channel(ch);
    }
}

void stop_rx_task(void)
{
    stop_all_uart_tasks();
}

esp_err_t send_data_with_temp_config(int channel,
                                     const channel_uart_config_t *temp_config,
                                     const uint8_t *data,
                                     size_t data_len)
{
    if (temp_config == NULL || data == NULL || data_len == 0 || temp_config->channel != channel) {
        return ESP_ERR_INVALID_ARG;
    }

    channel_uart_config_t current;
    bool need_reconfigure = true;
    if (get_current_runtime_config(channel, &current) == ESP_OK) {
        need_reconfigure = !compare_uart_config(&current, temp_config);
    }

    if (need_reconfigure) {
        ESP_RETURN_ON_ERROR(quick_reconfigure_channel(channel, temp_config), TAG,
                            "quick reconfigure failed for CH%d", channel);
        set_current_runtime_config(channel, temp_config);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    tx_tasks_to_channel((uint8_t *)data, data_len, channel);
    return ESP_OK;
}

uart_config_mode_t get_uart_config_mode(void)
{
    return s_uart_config_mode;
}

void set_uart_config_mode(uart_config_mode_t mode)
{
    s_uart_config_mode = mode;
}

esp_err_t smart_send_data_to_ch5(uart_config_mode_t work_mode,
                                 int source_channel,
                                 const uint8_t *data,
                                 size_t data_len)
{
    const int target_channel = 2; /* 项目现实映射：CH2(UART2) */

    if (data == NULL || data_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    if (work_mode == UART_CONFIG_MODE_SLAVE_FOLLOW) {
        channel_uart_config_t source_cfg;
        ESP_RETURN_ON_ERROR(get_current_runtime_config(source_channel, &source_cfg), TAG,
                            "source CH%d config unavailable", source_channel);

        channel_uart_config_t target_cfg = source_cfg;
        target_cfg.channel = target_channel;
        ESP_RETURN_ON_ERROR(send_data_with_temp_config(target_channel, &target_cfg, data, data_len), TAG,
                            "follow-mode send failed");
        return ESP_OK;
    }

    tx_tasks_to_channel((uint8_t *)data, data_len, target_channel);
    return ESP_OK;
}

void uart_reinit(int baudrate1, uart_word_length_t data_bits1, uart_parity_t parity1, uart_stop_bits_t stop_bits1,
                 int frame_time1, int frame_len1,
                 int baudrate2, uart_word_length_t data_bits2, uart_parity_t parity2, uart_stop_bits_t stop_bits2,
                 int frame_time2, int frame_len2,
                 int baudrate3, uart_word_length_t data_bits3, uart_parity_t parity3, uart_stop_bits_t stop_bits3,
                 int frame_time3, int frame_len3,
                 int baudrate4, uart_word_length_t data_bits4, uart_parity_t parity4, uart_stop_bits_t stop_bits4,
                 int frame_time4, int frame_len4,
                 int baudrate5, uart_word_length_t data_bits5, uart_parity_t parity5, uart_stop_bits_t stop_bits5,
                 int frame_time5, int frame_len5)
{
    (void)baudrate4;
    (void)data_bits4;
    (void)parity4;
    (void)stop_bits4;
    (void)frame_time4;
    (void)frame_len4;
    (void)baudrate5;
    (void)data_bits5;
    (void)parity5;
    (void)stop_bits5;
    (void)frame_time5;
    (void)frame_len5;

    set_reinit_in_progress(true);
    suspend_all_uart_rx_tasks();

    channel_uart_config_t c1 = {
        .channel = 1,
        .baudrate = baudrate1,
        .data_bits = data_bits1,
        .parity = parity1,
        .stop_bits = stop_bits1,
        .frame_time = frame_time1,
        .frame_len = frame_len1,
        .timeout = DEFAULT_TIMEOUT,
    };
    channel_uart_config_t c2 = {
        .channel = 2,
        .baudrate = baudrate2,
        .data_bits = data_bits2,
        .parity = parity2,
        .stop_bits = stop_bits2,
        .frame_time = frame_time2,
        .frame_len = frame_len2,
        .timeout = DEFAULT_TIMEOUT,
    };
    channel_uart_config_t c3 = {
        .channel = 3,
        .baudrate = baudrate3,
        .data_bits = data_bits3,
        .parity = parity3,
        .stop_bits = stop_bits3,
        .frame_time = frame_time3,
        .frame_len = frame_len3,
        .timeout = DEFAULT_TIMEOUT,
    };

    (void)restart_single_channel(1, &c1);
    (void)restart_single_channel(2, &c2);
    (void)restart_single_channel(3, &c3);

    ESP_LOGW(TAG, "uart_reinit: CH4/CH5 args ignored (not present in this hardware project)");

    clear_all_channel_buffers();
    set_reinit_in_progress(false);
    resume_all_uart_rx_tasks();
}

void deinit_all_uart_drivers_for_ota(void)
{
    stop_all_uart_tasks();

    for (int i = 0; i < ASYNC_UART_MAX_CHANNELS; i++) {
        if (!s_channel_hw[i].enabled) {
            continue;
        }
        (void)uart_driver_delete(s_channel_hw[i].port);
    }
}

void uart_init(void)
{
    if (sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422 &&
        !sx_serial_port_manager_uart0_reserved()) {
        s_channel_hw[0].tx_pin = U0TXD;
        s_channel_hw[0].rx_pin = U1RXD;
        s_channel_hw[0].tx_led = LED_COM1;
        s_channel_hw[0].rx_led = LED_COM2;
        s_channel_hw[0].name = "RS422 (COM1 TX + COM2 RX, CH1/UART1)";
        s_channel_hw[2].enabled = false;
        ESP_LOGI(TAG, "RS422 compatibility layout active: logical TX=COM1, RX=COM2");
    }
    uart_led_init();
    uart_led_boot_animation();

    clear_all_channel_buffers();

    for (int ch = 1; ch <= ASYNC_UART_MAX_CHANNELS; ch++) {
        const uart_channel_hw_t *hw = get_channel_hw(ch);
        if (hw == NULL) {
            continue;
        }
        channel_uart_config_t cfg;
        esp_err_t err = get_channel_uart_config_from_nvs(ch, &cfg);
        if (err != ESP_OK) {
            fill_default_config(ch, &cfg);
            (void)write_channel_config_to_nvs(&cfg);
        }

        err = restart_single_channel(ch, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "init config failed for %s: %s", hw->name, esp_err_to_name(err));
        }
    }
}
