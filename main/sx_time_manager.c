#include "sx_time_manager.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static portMUX_TYPE s_time_spinlock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_time_offset_us = 0;
static bool s_time_synced = false;

void time_manager_init(void)
{
    portENTER_CRITICAL(&s_time_spinlock);
    s_time_offset_us = 0;
    s_time_synced = false;
    portEXIT_CRITICAL(&s_time_spinlock);
}

esp_err_t time_manager_sync_from_browser(uint64_t browser_timestamp_ms)
{
    int64_t now_us = (int64_t)esp_timer_get_time();
    int64_t target_us = (int64_t)(browser_timestamp_ms * 1000ULL);

    portENTER_CRITICAL(&s_time_spinlock);
    s_time_offset_us = target_us - now_us;
    s_time_synced = true;
    portEXIT_CRITICAL(&s_time_spinlock);

    return ESP_OK;
}

uint64_t time_manager_get_current_us(void)
{
    int64_t now_us = (int64_t)esp_timer_get_time();
    int64_t offset_us;

    portENTER_CRITICAL(&s_time_spinlock);
    offset_us = s_time_offset_us;
    portEXIT_CRITICAL(&s_time_spinlock);

    int64_t current = now_us + offset_us;
    if (current < 0) {
        current = 0;
    }

    return (uint64_t)current;
}

uint64_t time_manager_get_current_ms(void)
{
    return time_manager_get_current_us() / 1000ULL;
}

bool time_manager_is_synced(void)
{
    bool synced;
    portENTER_CRITICAL(&s_time_spinlock);
    synced = s_time_synced;
    portEXIT_CRITICAL(&s_time_spinlock);
    return synced;
}
