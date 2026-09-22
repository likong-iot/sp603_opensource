#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *label;
    int logical_port;
    int tx_port;
    int rx_port;
    bool available;
} sx_serial_resource_info_t;

size_t sx_serial_resource_get_all(sx_serial_resource_info_t *resources, size_t capacity);
int sx_serial_resource_tx_port(int logical_port);
int sx_serial_resource_rx_port(int logical_port);
int sx_serial_resource_logical_from_physical_rx(int physical_port);

#ifdef __cplusplus
}
#endif
