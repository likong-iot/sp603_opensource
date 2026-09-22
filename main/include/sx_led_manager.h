#pragma once

#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SX_LED_NETWORK_OFF = 0,
    SX_LED_NETWORK_STARTING, /* enabled/available but not the active uplink */
    SX_LED_NETWORK_PROVIDER, /* downstream network is ready and serving clients */
    SX_LED_NETWORK_ONLINE,   /* current default uplink */
    SX_LED_NETWORK_ONLINE_AND_PROVIDER, /* active uplink and downstream provider */
    SX_LED_NETWORK_ERROR,
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
