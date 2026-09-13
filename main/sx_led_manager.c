#include "sx_led_manager.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "s3_gpio.h"

static const char *TAG = "led_manager";
static volatile sx_led_network_state_t s_lan = SX_LED_NETWORK_OFF;
static volatile sx_led_network_state_t s_wifi = SX_LED_NETWORK_OFF;
static volatile sx_led_network_state_t s_4g = SX_LED_NETWORK_OFF;
static volatile uint8_t s_action_blinks_requested = 0;
static bool s_initialized = false;

static int state_level(sx_led_network_state_t state, unsigned tick)
{
    switch (state) {
    /* Panel LEDs are active-low: 0 = on, 1 = off. */
    case SX_LED_NETWORK_ONLINE: return 0;
    case SX_LED_NETWORK_STARTING: return (tick % 4U) < 2U ? 0 : 1;
    case SX_LED_NETWORK_ERROR: return (tick % 4U) == 0U ? 0 : 1;
    default: return 1;
    }
}

static void led_task(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    uint8_t action_blinks = 0;
    bool action_on = false;
    for (;;) {
        if (action_blinks == 0 && s_action_blinks_requested != 0) {
            action_blinks = s_action_blinks_requested;
            s_action_blinks_requested = 0;
            action_on = false;
        }
        if (action_blinks != 0) {
            action_on = !action_on;
            gpio_set_level(LED_LAN, action_on ? 0 : 1);
            gpio_set_level(LED_WIFI, action_on ? 0 : 1);
            gpio_set_level(LED_4G, action_on ? 0 : 1);
            if (!action_on) --action_blinks;
        } else {
            gpio_set_level(LED_SYS, (tick % 8U) < 1U ? 0 : 1);
            gpio_set_level(LED_LAN, state_level(s_lan, tick));
            gpio_set_level(LED_WIFI, state_level(s_wifi, tick));
            gpio_set_level(LED_4G, state_level(s_4g, tick));
        }
        ++tick;
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

esp_err_t sx_led_manager_init(void)
{
    if (s_initialized) return ESP_OK;
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << LED_SYS) | (1ULL << LED_LAN) |
                        (1ULL << LED_COM1) | (1ULL << LED_WIFI) |
                        (1ULL << LED_COM2) | (1ULL << LED_4G) |
                        (1ULL << LED_232),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) return err;
    gpio_set_level(LED_SYS, 1);
    gpio_set_level(LED_LAN, 1);
    gpio_set_level(LED_WIFI, 1);
    gpio_set_level(LED_4G, 1);
    gpio_set_level(LED_COM1, 1);
    gpio_set_level(LED_COM2, 1);
    gpio_set_level(LED_232, 1);
    if (xTaskCreate(led_task, "sp603_leds", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create LED task failed");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "SP603 LEDs: SYS=LED1 LAN=LED2 COM1=LED3 WIFI=LED4 COM2=LED5 4G=LED6 232=LED7");
    return ESP_OK;
}

void sx_led_manager_set_lan(sx_led_network_state_t state) { s_lan = state; }
void sx_led_manager_set_wifi(sx_led_network_state_t state) { s_wifi = state; }
void sx_led_manager_set_4g(sx_led_network_state_t state) { s_4g = state; }

void sx_led_manager_indicate_restart(void)
{
    s_action_blinks_requested = 1;
}

void sx_led_manager_indicate_factory_reset(void)
{
    s_action_blinks_requested = 3;
}
