#include "sx_button_manager.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "async_uart.h"
#include "s3_gpio.h"
#include "sx_led_manager.h"
#include "sx_work_mode.h"

static const char *TAG = "button_mgr";

#define BUTTON_SCAN_PERIOD_MS 10
#define BUTTON_DEBOUNCE_TICKS 5
#define BUTTON_SHORT_MIN_TICKS 5       /* 50 ms */
#define BUTTON_SHORT_MAX_TICKS 100     /* 1 s */
#define BUTTON_FACTORY_RESET_TICKS 500 /* 5 s */

static bool s_initialized;
static volatile bool s_action_running;

static void restart_task(void *arg)
{
    const bool factory_reset = (bool)(uintptr_t)arg;
    if (factory_reset) {
        ESP_LOGW(TAG, "factory reset requested by panel key");
        sx_led_manager_indicate_factory_reset();
        (void)sx_work_mode_stop_current();
        stop_all_uart_tasks();
        vTaskDelay(pdMS_TO_TICKS(1600));
        esp_err_t err = nvs_flash_erase();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "NVS erase failed: %s; rebooting without retry", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "software restart requested by panel key");
        sx_led_manager_indicate_restart();
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    esp_restart();
    vTaskDelete(NULL);
}

static bool request_action(bool factory_reset)
{
    if (s_action_running) return false;
    s_action_running = true;
    BaseType_t ret = xTaskCreate(restart_task, factory_reset ? "factory_reset" : "key_restart",
                                 3072, (void *)(uintptr_t)factory_reset, 5, NULL);
    if (ret != pdPASS) {
        s_action_running = false;
        ESP_LOGE(TAG, "create key action task failed");
        return false;
    }
    return true;
}

static void button_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    bool reset_triggered = false;
    unsigned low_ticks = 0;
    unsigned high_ticks = 0;
    unsigned held_ticks = 0;

    for (;;) {
        const int level = gpio_get_level(KEY);
        if (level == 0) {
            high_ticks = 0;
            if (!pressed) {
                if (++low_ticks >= BUTTON_DEBOUNCE_TICKS) {
                    pressed = true;
                    reset_triggered = false;
                    held_ticks = 0;
                    low_ticks = 0;
                }
            } else {
                if (held_ticks < BUTTON_FACTORY_RESET_TICKS) ++held_ticks;
                if (!reset_triggered && held_ticks >= BUTTON_FACTORY_RESET_TICKS) {
                    reset_triggered = true;
                    (void)request_action(true);
                }
            }
        } else {
            low_ticks = 0;
            if (pressed) {
                if (++high_ticks >= BUTTON_DEBOUNCE_TICKS) {
                    if (!reset_triggered && held_ticks >= BUTTON_SHORT_MIN_TICKS &&
                        held_ticks < BUTTON_SHORT_MAX_TICKS) {
                        (void)request_action(false);
                    }
                    pressed = false;
                    high_ticks = 0;
                    held_ticks = 0;
                    reset_triggered = false;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_SCAN_PERIOD_MS));
    }
}

esp_err_t sx_button_manager_init(void)
{
    if (s_initialized) return ESP_OK;
    const gpio_config_t key_cfg = {
        .pin_bit_mask = (1ULL << KEY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&key_cfg);
    if (err != ESP_OK) return err;
    if (xTaskCreate(button_task, "panel_button", 3072, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "panel key scanner ready: short=%d..%d ms, reset=%d ms",
             BUTTON_SHORT_MIN_TICKS * BUTTON_SCAN_PERIOD_MS,
             BUTTON_SHORT_MAX_TICKS * BUTTON_SCAN_PERIOD_MS,
             BUTTON_FACTORY_RESET_TICKS * BUTTON_SCAN_PERIOD_MS);
    return ESP_OK;
}
