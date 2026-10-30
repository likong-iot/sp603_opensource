#include "sx_auto_collect.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "app_task_utils.h"

#define TAG "AUTO_COLLECT"

typedef struct {
    ac_item_config_t cfg;
    uint64_t last_poll_ms;
    bool data_valid;
    uint8_t fail_count;
} ac_runtime_item_t;

typedef struct {
    int port;
    uint8_t mapped_slave_addr;
    int items_count;
    ac_runtime_item_t items[AC_MAX_ITEMS_PER_PORT];
    SemaphoreHandle_t mutex;
    TaskHandle_t polling_task;
    bool task_created;
    atomic_bool exited;
    serial_port_config_t saved_uart;
    bool saved_uart_valid;
} ac_port_runtime_t;

/* SP603 双 RS485：配置槽依次绑定 COM2、COM1。 */
static const int s_ac_collect_ports[AC_COLLECT_PORT_COUNT] = {1, 3};
static ac_port_runtime_t s_runtime[AC_COLLECT_PORT_COUNT] = {0};
static atomic_bool s_running;

static uint16_t modbus_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 0x0001) {
                crc = (crc >> 1) ^ 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

static inline uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

static bool verify_crc(const uint8_t *frame, int total_len)
{
    if (frame == NULL || total_len < 4) {
        return false;
    }
    uint16_t recv_crc = (uint16_t)frame[total_len - 2] | ((uint16_t)frame[total_len - 1] << 8);
    uint16_t calc_crc = modbus_crc16(frame, (size_t)total_len - 2);
    return recv_crc == calc_crc;
}

static void fill_default_item(int port, ac_item_config_t *it)
{
    memset(it, 0, sizeof(*it));
    it->enabled = true;
    it->real_slave_addr = 1;
    it->function_code = 0x03;
    it->register_addr = 0;
    it->mapped_register_addr = 0;
    it->register_num = 1;
    it->interval_ms = 100;
    it->timeout_ms = 1000;

    it->uart.port = port;
    it->uart.baudrate = 9600;
    it->uart.data_bits = UART_DATA_8_BITS;
    it->uart.parity = UART_PARITY_DISABLE;
    it->uart.stop_bits = UART_STOP_BITS_1;
    it->uart.frame_time = 50;
    it->uart.frame_len = 512;
    it->uart.timeout = 1000;
}

static void fill_default_port_cfg(int port, ac_port_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->mapped_slave_addr = 1;
    cfg->items_count = 1;
    fill_default_item(port, &cfg->items[0]);
}

static esp_err_t load_port_config_from_nvs(nvs_handle_t nvs,
                                              const char *prefix,
                                              int port,
                                              ac_port_config_t *cfg)
{
    if (prefix == NULL || cfg == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    fill_default_port_cfg(port, cfg);

    char key[32];
    uint8_t u8 = 0;
    int32_t count = 0;

    snprintf(key, sizeof(key), "%s_maddr", prefix);
    if (nvs_get_u8(nvs, key, &u8) == ESP_OK && u8 >= 1 && u8 <= 247) {
        cfg->mapped_slave_addr = u8;
    }

    snprintf(key, sizeof(key), "%s_count", prefix);
    if (nvs_get_i32(nvs, key, &count) != ESP_OK) {
        return ESP_OK;
    }

    if (count < 0) {
        count = 0;
    }
    if (count > AC_MAX_ITEMS_PER_PORT) {
        count = AC_MAX_ITEMS_PER_PORT;
    }

    cfg->items_count = count;
    for (int i = 0; i < cfg->items_count; i++) {
        ac_item_config_t *it = &cfg->items[i];
        fill_default_item(port, it);

        snprintf(key, sizeof(key), "%s_item%d_en", prefix, i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            it->enabled = (u8 != 0);
        }

        snprintf(key, sizeof(key), "%s_item%d_rs", prefix, i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK && u8 >= 1 && u8 <= 247) {
            it->real_slave_addr = u8;
        }

        snprintf(key, sizeof(key), "%s_item%d_fc", prefix, i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK && (u8 == 0x03 || u8 == 0x04)) {
            it->function_code = u8;
        }

        uint16_t u16 = 0;
        uint32_t u32 = 0;

        snprintf(key, sizeof(key), "%s_item%d_ra", prefix, i);
        if (nvs_get_u16(nvs, key, &u16) == ESP_OK) {
            it->register_addr = u16;
        }

        snprintf(key, sizeof(key), "%s_item%d_mra", prefix, i);
        if (nvs_get_u16(nvs, key, &u16) == ESP_OK) {
            it->mapped_register_addr = u16;
        }

        snprintf(key, sizeof(key), "%s_item%d_rn", prefix, i);
        if (nvs_get_u16(nvs, key, &u16) == ESP_OK && u16 > 0 && u16 <= 125) {
            it->register_num = u16;
        }

        snprintf(key, sizeof(key), "%s_item%d_int", prefix, i);
        if (nvs_get_u32(nvs, key, &u32) == ESP_OK && u32 > 0) {
            it->interval_ms = u32;
        }

        snprintf(key, sizeof(key), "%s_item%d_to", prefix, i);
        if (nvs_get_u32(nvs, key, &u32) == ESP_OK && u32 > 0) {
            it->timeout_ms = u32;
        }

        snprintf(key, sizeof(key), "%s_item%d_baud", prefix, i);
        if (nvs_get_u32(nvs, key, &u32) == ESP_OK && u32 > 0) {
            it->uart.baudrate = (int)u32;
        }

        snprintf(key, sizeof(key), "%s_item%d_db", prefix, i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            if (u8 == 5) {
                it->uart.data_bits = UART_DATA_5_BITS;
            } else if (u8 == 6) {
                it->uart.data_bits = UART_DATA_6_BITS;
            } else if (u8 == 7) {
                it->uart.data_bits = UART_DATA_7_BITS;
            } else {
                it->uart.data_bits = UART_DATA_8_BITS;
            }
        }

        snprintf(key, sizeof(key), "%s_item%d_par", prefix, i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            if (u8 == 1) {
                it->uart.parity = UART_PARITY_ODD;
            } else if (u8 == 2) {
                it->uart.parity = UART_PARITY_EVEN;
            } else {
                it->uart.parity = UART_PARITY_DISABLE;
            }
        }

        snprintf(key, sizeof(key), "%s_item%d_sb", prefix, i);
        if (nvs_get_u8(nvs, key, &u8) == ESP_OK) {
            if (u8 == 2) {
                it->uart.stop_bits = UART_STOP_BITS_2;
            } else {
                it->uart.stop_bits = UART_STOP_BITS_1;
            }
        }

        it->uart.port = port;
        it->uart.frame_time = 50;
        it->uart.frame_len = 512;
        it->uart.timeout = 1000;
    }

    return ESP_OK;
}

static void ac_polling_task(void *arg)
{
    ac_port_runtime_t *runtime = (ac_port_runtime_t *)arg;
    if (runtime == NULL) {
        delete_self_app_task_with_caps();
        return;
    }

    uint8_t rx_buf[1024];
    while (atomic_load(&s_running)) {

        xSemaphoreTake(runtime->mutex, portMAX_DELAY);
        int items_count = runtime->items_count;

        for (int i = 0; i < items_count && atomic_load(&s_running); i++) {
            ac_runtime_item_t *rt = &runtime->items[i];
            if (!rt->cfg.enabled) {
                continue;
            }

            uint64_t now = now_ms();
            if (now - rt->last_poll_ms < rt->cfg.interval_ms) {
                continue;
            }
            rt->last_poll_ms = now;

            uint8_t req[8];
            req[0] = rt->cfg.real_slave_addr;
            req[1] = rt->cfg.function_code;
            req[2] = (uint8_t)((rt->cfg.register_addr >> 8) & 0xFF);
            req[3] = (uint8_t)(rt->cfg.register_addr & 0xFF);
            req[4] = (uint8_t)((rt->cfg.register_num >> 8) & 0xFF);
            req[5] = (uint8_t)(rt->cfg.register_num & 0xFF);
            uint16_t crc = modbus_crc16(req, 6);
            req[6] = (uint8_t)(crc & 0xFF);
            req[7] = (uint8_t)((crc >> 8) & 0xFF);

            serial_port_config_t uart_cfg = rt->cfg.uart;
            uart_cfg.port = runtime->port;

            xSemaphoreGive(runtime->mutex);

            esp_err_t send_ret = send_data_with_temp_config(runtime->port,
                                                            &uart_cfg,
                                                            req,
                                                            sizeof(req));
            if (send_ret == ESP_OK) {
                bool got = false;
                int waited = 0;
                while (atomic_load(&s_running) && (uint32_t)waited < rt->cfg.timeout_ms) {
                    uint64_t ts = 0;
                    int n = take_port_data(runtime->port, rx_buf, sizeof(rx_buf), &ts);
                    if (n > 0 && n >= 5 && verify_crc(rx_buf, n)) {
                        if (rx_buf[0] == rt->cfg.real_slave_addr &&
                            rx_buf[1] == rt->cfg.function_code) {
                            got = true;
                            break;
                        }
                    }
                    vTaskDelay(pdMS_TO_TICKS(10));
                    waited += 10;
                }

                xSemaphoreTake(runtime->mutex, portMAX_DELAY);
                if (got) {
                    rt->data_valid = true;
                    rt->fail_count = 0;
                } else {
                    rt->data_valid = false;
                    if (rt->fail_count < 255) {
                        rt->fail_count++;
                    }
                }
                continue;
            }

            xSemaphoreTake(runtime->mutex, portMAX_DELAY);
            rt->data_valid = false;
            if (rt->fail_count < 255) {
                rt->fail_count++;
            }
        }

        xSemaphoreGive(runtime->mutex);
        if (atomic_load(&s_running)) vTaskDelay(pdMS_TO_TICKS(50));
    }
    atomic_store(&runtime->exited, true);
    delete_self_app_task_with_caps();
}

esp_err_t sx_auto_collect_deinit(void)
{
    atomic_store(&s_running, false);
    /* Let polling finish its UART operation and release its mutex itself. */
    for (int waited = 0; waited < 10000; waited += 10) {
        bool active = false;
        for (int i = 0; i < AC_COLLECT_PORT_COUNT; ++i)
            active |= s_runtime[i].task_created && !atomic_load(&s_runtime[i].exited);
        if (!active) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    for (int i = 0; i < AC_COLLECT_PORT_COUNT; ++i)
        if (s_runtime[i].task_created && !atomic_load(&s_runtime[i].exited))
            return ESP_ERR_TIMEOUT;

    for (int i = 0; i < AC_COLLECT_PORT_COUNT; i++) {
        if (s_runtime[i].saved_uart_valid) {
            esp_err_t err = quick_reconfigure_port(s_runtime[i].port, &s_runtime[i].saved_uart);
            if (err != ESP_OK) return err;
            s_runtime[i].saved_uart_valid = false;
        }
        if (s_runtime[i].mutex != NULL) vSemaphoreDelete(s_runtime[i].mutex);
        memset(&s_runtime[i], 0, sizeof(s_runtime[i]));
    }
    return ESP_OK;
}

esp_err_t sx_auto_collect_init(void)
{
    esp_err_t cleanup_err = sx_auto_collect_deinit();
    if (cleanup_err != ESP_OK) return cleanup_err;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open NVS failed: %s", esp_err_to_name(err));
        return err;
    }

    ac_port_config_t *cfg = (ac_port_config_t *)calloc(1, sizeof(ac_port_config_t));
    if (cfg == NULL) {
        nvs_close(nvs);
        ESP_LOGE(TAG, "alloc ac port config failed");
        return ESP_ERR_NO_MEM;
    }

    atomic_store(&s_running, true);
    for (int i = 0; i < AC_COLLECT_PORT_COUNT; i++) {
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "ac%d", i + 1);

        int port = s_ac_collect_ports[i];
        load_port_config_from_nvs(nvs, prefix, port, cfg);

        s_runtime[i].port = port;
        err = get_current_runtime_config(port, &s_runtime[i].saved_uart);
        if (err != ESP_OK) break;
        s_runtime[i].saved_uart_valid = true;
        s_runtime[i].mapped_slave_addr = cfg->mapped_slave_addr;
        s_runtime[i].items_count = cfg->items_count;
        if (s_runtime[i].items_count > AC_MAX_ITEMS_PER_PORT) {
            s_runtime[i].items_count = AC_MAX_ITEMS_PER_PORT;
        }

        for (int item = 0; item < s_runtime[i].items_count; item++) {
            s_runtime[i].items[item].cfg = cfg->items[item];
            s_runtime[i].items[item].last_poll_ms = 0;
            s_runtime[i].items[item].data_valid = false;
            s_runtime[i].items[item].fail_count = 0;
        }

        s_runtime[i].mutex = xSemaphoreCreateMutex();
        if (s_runtime[i].mutex == NULL) {
            ESP_LOGE(TAG, "create mutex failed for AC port %d", port);
            err = ESP_ERR_NO_MEM;
            break;
        }

        char task_name[16];
        snprintf(task_name, sizeof(task_name), "ac_poll_ch%d", port);
        atomic_store(&s_runtime[i].exited, false);
        BaseType_t ok = create_app_task_psram(ac_polling_task,
                                              task_name,
                                              8192,
                                              &s_runtime[i],
                                              4,
                                              &s_runtime[i].polling_task,
                                              tskNO_AFFINITY);
        if (ok != pdPASS) {
            vSemaphoreDelete(s_runtime[i].mutex);
            s_runtime[i].mutex = NULL;
            s_runtime[i].polling_task = NULL;
            ESP_LOGE(TAG, "create polling task failed for AC port %d", port);
            err = ESP_ERR_NO_MEM;
            break;
        } else {
            s_runtime[i].task_created = true;
            ESP_LOGI(TAG, "auto collect polling started on %s (items=%d)",
                     port == 1 ? "COM2" : "COM1",
                     s_runtime[i].items_count);
        }
    }

    free(cfg);
    nvs_close(nvs);
    if (err != ESP_OK) {
        (void)sx_auto_collect_deinit();
        return err;
    }
    return ESP_OK;
}

bool sx_auto_collect_is_running(void)
{
    return s_running;
}
