#include "sx_led_manager.h"

#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "s3_gpio.h"

static const char *TAG = "led_manager";
static volatile sx_led_network_state_t s_lan = SX_LED_NETWORK_OFF;
static volatile sx_led_network_state_t s_wifi = SX_LED_NETWORK_OFF;
static volatile sx_led_network_state_t s_4g = SX_LED_NETWORK_OFF;
static volatile sx_led_system_state_t s_system_state = SX_LED_SYSTEM_STARTING;
static volatile uint8_t s_action_blinks_requested = 0;
static volatile bool s_action_active = false;
static bool s_initialized = false;

#define SERIAL_LED_HOLD_US 100000LL
#define LED_SCAN_MS 10
#define NETWORK_LED_TICK_MS 250

typedef struct {
    gpio_num_t pin;
    unsigned active_users;
    int64_t hold_until_us;
    bool lit;
} serial_led_t;

static serial_led_t s_serial_leds[] = {
    {.pin = LED_COM1}, {.pin = LED_COM2}, {.pin = LED_232},
};
static portMUX_TYPE s_serial_led_lock = portMUX_INITIALIZER_UNLOCKED;

static serial_led_t *serial_led_for_pin(gpio_num_t pin)
{
    for (size_t i = 0; i < sizeof(s_serial_leds) / sizeof(s_serial_leds[0]); ++i) {
        if (s_serial_leds[i].pin == pin) return &s_serial_leds[i];
    }
    return NULL;
}

/* Caller holds s_serial_led_lock, including the GPIO write, so an expired
 * pulse cannot turn off a newer TX/RX indication from the other core. */
static void serial_led_apply_locked(serial_led_t *led, int64_t now)
{
    bool lit = led->active_users != 0 || now < led->hold_until_us;
    if (s_action_active) {
        led->lit = lit;
        return;
    }
    if (lit != led->lit) {
        gpio_set_level(led->pin, lit ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL);
        led->lit = lit;
    }
}

static void serial_led_update(void)
{
    portENTER_CRITICAL(&s_serial_led_lock);
    int64_t now = esp_timer_get_time();
    for (size_t i = 0; i < sizeof(s_serial_leds) / sizeof(s_serial_leds[0]); ++i) {
        serial_led_apply_locked(&s_serial_leds[i], now);
    }
    portEXIT_CRITICAL(&s_serial_led_lock);
}

void sx_led_manager_serial_activity(gpio_num_t pin)
{
    serial_led_t *led = serial_led_for_pin(pin);
    if (led == NULL) return;
    portENTER_CRITICAL(&s_serial_led_lock);
    int64_t now = esp_timer_get_time();
    led->hold_until_us = now + SERIAL_LED_HOLD_US;
    serial_led_apply_locked(led, now);
    portEXIT_CRITICAL(&s_serial_led_lock);
}

void sx_led_manager_serial_begin(gpio_num_t pin)
{
    serial_led_t *led = serial_led_for_pin(pin);
    if (led == NULL) return;
    portENTER_CRITICAL(&s_serial_led_lock);
    ++led->active_users;
    int64_t now = esp_timer_get_time();
    led->hold_until_us = now + SERIAL_LED_HOLD_US;
    serial_led_apply_locked(led, now);
    portEXIT_CRITICAL(&s_serial_led_lock);
}

void sx_led_manager_serial_end(gpio_num_t pin)
{
    serial_led_t *led = serial_led_for_pin(pin);
    if (led == NULL) return;
    portENTER_CRITICAL(&s_serial_led_lock);
    int64_t now = esp_timer_get_time();
    if (led->active_users != 0) {
        --led->active_users;
        led->hold_until_us = now + SERIAL_LED_HOLD_US;
    }
    serial_led_apply_locked(led, now);
    portEXIT_CRITICAL(&s_serial_led_lock);
}

void sx_led_manager_serial_clear(gpio_num_t pin)
{
    serial_led_t *led = serial_led_for_pin(pin);
    if (led == NULL) return;
    portENTER_CRITICAL(&s_serial_led_lock);
    led->active_users = 0;
    led->hold_until_us = 0;
    serial_led_apply_locked(led, esp_timer_get_time());
    portEXIT_CRITICAL(&s_serial_led_lock);
}

static int state_level(sx_led_network_state_t state, unsigned tick)
{
    switch (state) {
    case SX_LED_NETWORK_ONLINE: return SX_LED_ON_LEVEL;
    /* 1 s on / 1 s off: available provider or standby uplink. */
    case SX_LED_NETWORK_STARTING: return (tick % 8U) < 4U ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL;
    /* 1 s on / 1 s off: interface is serving the downstream network. */
    case SX_LED_NETWORK_PROVIDER: return (tick % 8U) < 4U ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL;
    /* Two 250 ms pulses every 2 s: Wi-Fi STA is the uplink and AP provides service. */
    case SX_LED_NETWORK_ONLINE_AND_PROVIDER:
        return (tick % 8U) == 0U || (tick % 8U) == 2U
                   ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL;
    /* 250 ms on / 750 ms off: interface error. */
    case SX_LED_NETWORK_ERROR: return (tick % 4U) == 0U ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL;
    default: return SX_LED_OFF_LEVEL;
    }
}

static int system_level(sx_led_system_state_t state, int64_t now_us)
{
    uint64_t period_us;
    uint64_t on_us;
    switch (state) {
    case SX_LED_SYSTEM_NORMAL:
        period_us = 1000000ULL;
        on_us = 500000ULL;
        break;
    case SX_LED_SYSTEM_STARTING:
        period_us = 400000ULL;
        on_us = 200000ULL;
        break;
    case SX_LED_SYSTEM_WARNING:
        period_us = 2000000ULL;
        on_us = 200000ULL;
        break;
    case SX_LED_SYSTEM_ERROR:
    default:
        period_us = 250000ULL;
        on_us = 125000ULL;
        break;
    }
    return ((uint64_t)now_us % period_us) < on_us ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL;
}

static void set_all_leds_level(int level)
{
    gpio_set_level(LED_SYS, level);
    gpio_set_level(LED_LAN, level);
    gpio_set_level(LED_COM1, level);
    gpio_set_level(LED_WIFI, level);
    gpio_set_level(LED_COM2, level);
    gpio_set_level(LED_4G, level);
    gpio_set_level(LED_232, level);
}

static void led_task(void *arg)
{
    (void)arg;
    unsigned tick = 0;
    int64_t next_network_update_us = 0;
    int64_t next_action_update_us = 0;
    uint8_t action_blinks = 0;
    bool action_on = false;
    for (;;) {
        int64_t now = esp_timer_get_time();
        if (action_blinks == 0 && s_action_blinks_requested != 0) {
            action_blinks = s_action_blinks_requested;
            s_action_blinks_requested = 0;
            s_action_active = true;
            action_on = false;
            next_action_update_us = 0;
        }
        if (action_blinks != 0) {
            if (now >= next_action_update_us) {
                action_on = !action_on;
                set_all_leds_level(action_on ? SX_LED_ON_LEVEL : SX_LED_OFF_LEVEL);
                next_action_update_us = now + NETWORK_LED_TICK_MS * 1000LL;
                if (!action_on && --action_blinks == 0) s_action_active = false;
            }
            vTaskDelay(pdMS_TO_TICKS(LED_SCAN_MS));
            continue;
        }

        serial_led_update();
        gpio_set_level(LED_SYS, system_level(s_system_state, now));
        if (now < next_network_update_us) {
            vTaskDelay(pdMS_TO_TICKS(LED_SCAN_MS));
            continue;
        }
        next_network_update_us = now + NETWORK_LED_TICK_MS * 1000LL;
        gpio_set_level(LED_LAN, state_level(s_lan, tick));
        gpio_set_level(LED_WIFI, state_level(s_wifi, tick));
        gpio_set_level(LED_4G, state_level(s_4g, tick));
        ++tick;
        vTaskDelay(pdMS_TO_TICKS(LED_SCAN_MS));
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
    gpio_set_level(LED_SYS, SX_LED_OFF_LEVEL);
    gpio_set_level(LED_LAN, SX_LED_OFF_LEVEL);
    gpio_set_level(LED_WIFI, SX_LED_OFF_LEVEL);
    gpio_set_level(LED_4G, SX_LED_OFF_LEVEL);
    gpio_set_level(LED_COM1, SX_LED_OFF_LEVEL);
    gpio_set_level(LED_COM2, SX_LED_OFF_LEVEL);
    gpio_set_level(LED_232, SX_LED_OFF_LEVEL);
    if (xTaskCreate(led_task, "sp603_leds", 2048, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "create LED task failed");
        return ESP_ERR_NO_MEM;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "SP603 LEDs: SYS=LED4 LAN=LED1 COM1=LED2 WIFI=LED3 COM2=LED7 4G=LED5 232=LED6 (active-high)");
    return ESP_OK;
}

void sx_led_manager_set_lan(sx_led_network_state_t state) { s_lan = state; }
void sx_led_manager_set_wifi(sx_led_network_state_t state) { s_wifi = state; }
void sx_led_manager_set_4g(sx_led_network_state_t state) { s_4g = state; }
void sx_led_manager_set_system_state(sx_led_system_state_t state) { s_system_state = state; }

void sx_led_manager_indicate_restart(void)
{
    s_action_active = true;
    s_action_blinks_requested = 1;
}

void sx_led_manager_indicate_factory_reset(void)
{
    s_action_active = true;
    s_action_blinks_requested = 3;
}
