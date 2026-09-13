#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SX_LED_NETWORK_OFF = 0,
    SX_LED_NETWORK_STARTING,
    SX_LED_NETWORK_ONLINE,
    SX_LED_NETWORK_ERROR,
} sx_led_network_state_t;

esp_err_t sx_led_manager_init(void);
void sx_led_manager_set_lan(sx_led_network_state_t state);
void sx_led_manager_set_wifi(sx_led_network_state_t state);
void sx_led_manager_set_4g(sx_led_network_state_t state);

/* Request a non-blocking panel indication for a user action. */
void sx_led_manager_indicate_restart(void);
void sx_led_manager_indicate_factory_reset(void);

#ifdef __cplusplus
}
#endif
