#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize the panel key scanner (active-low, debounced at 10 ms). */
esp_err_t sx_button_manager_init(void);

#ifdef __cplusplus
}
#endif
