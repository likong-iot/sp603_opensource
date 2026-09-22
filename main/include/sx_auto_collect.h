#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "async_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AC_COLLECT_PORT_COUNT 2
#define AC_MAX_ITEMS_PER_PORT 60

typedef struct {
    bool enabled;
    uint8_t real_slave_addr;
    uint8_t function_code;
    uint16_t register_addr;
    uint16_t mapped_register_addr;
    uint16_t register_num;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    serial_port_config_t uart;
} ac_item_config_t;

typedef struct {
    uint8_t mapped_slave_addr;
    int items_count;
    ac_item_config_t items[AC_MAX_ITEMS_PER_PORT];
} ac_port_config_t;

typedef struct {
    ac_port_config_t ports[AC_COLLECT_PORT_COUNT];
} ac_config_t;

esp_err_t sx_auto_collect_init(void);
esp_err_t sx_auto_collect_deinit(void);
bool sx_auto_collect_is_running(void);

#ifdef __cplusplus
}
#endif
