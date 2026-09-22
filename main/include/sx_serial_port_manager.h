#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SX_SERIAL_LAYOUT_DUAL_RS485 = 0,
    SX_SERIAL_LAYOUT_RS422,
} sx_serial_layout_t;

typedef struct {
    const char *id;
    const char *label;
    int port;
    bool present;
    bool available;
    const char *reserved_by;
} sx_serial_port_capability_t;

esp_err_t sx_serial_port_manager_init(void);
sx_serial_layout_t sx_serial_port_manager_get_layout(void);
const char *sx_serial_layout_name(sx_serial_layout_t layout);
bool sx_serial_layout_from_name(const char *name, sx_serial_layout_t *layout);
esp_err_t sx_serial_port_manager_save_layout(sx_serial_layout_t layout,
                                             char *reason,
                                             size_t reason_size);
bool sx_serial_port_manager_uart0_reserved(void);
size_t sx_serial_port_manager_get_capabilities(sx_serial_port_capability_t *ports,
                                               size_t capacity);
bool sx_serial_port_manager_port_available(int port);
/* Physical port identifiers are also used as the NVS configuration prefix. */
const char *sx_serial_port_manager_port_key(int port);
const char *sx_serial_port_manager_port_label(int port);

#ifdef __cplusplus
}
#endif
