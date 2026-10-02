#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SX_LED_NETWORK_OFF = 0, /* interface disabled */
    SX_LED_NETWORK_CONNECTING, /* enabled but not connected */
    SX_LED_NETWORK_LOCAL,   /* connected without Internet reachability */
    SX_LED_NETWORK_PROVIDER, /* serving a downstream network */
    SX_LED_NETWORK_ONLINE,  /* Internet reachable */
} sx_led_network_state_t;

typedef enum {
    SX_LED_SYSTEM_STARTING = 0,
    SX_LED_SYSTEM_NORMAL,
    SX_LED_SYSTEM_WARNING,
    SX_LED_SYSTEM_ERROR,
} sx_led_system_state_t;

esp_err_t sx_led_manager_init(void);
void sx_led_manager_set_lan(sx_led_network_state_t state);
void sx_led_manager_set_wifi(sx_led_network_state_t state);
void sx_led_manager_set_4g(sx_led_network_state_t state);
void sx_led_manager_set_system_state(sx_led_system_state_t state);

/* Task-context only. Physical LED pins also handle the split RS422 TX/RX LEDs.
 * Activity is visible for 100 ms; begin/end keeps a shared LED on while any
 * transfer is active. GPIO writes and expiry are serialized by the manager. */
void sx_led_manager_serial_activity(gpio_num_t pin);
void sx_led_manager_serial_begin(gpio_num_t pin);
void sx_led_manager_serial_end(gpio_num_t pin);
void sx_led_manager_serial_clear(gpio_num_t pin);

/* Request a non-blocking panel indication for a user action. */
void sx_led_manager_indicate_restart(void);
void sx_led_manager_indicate_factory_reset(void);

#ifdef __cplusplus
}
#endif
