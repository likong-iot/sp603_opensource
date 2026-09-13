#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void time_manager_init(void);
esp_err_t time_manager_sync_from_browser(uint64_t browser_timestamp_ms);
uint64_t time_manager_get_current_ms(void);
uint64_t time_manager_get_current_us(void);
bool time_manager_is_synced(void);

#ifdef __cplusplus
}
#endif
