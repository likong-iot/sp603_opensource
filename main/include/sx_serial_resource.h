#pragma once
#include <stdbool.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;
    const char *label;
    int logical_channel;
    int tx_channel;
    int rx_channel;
    bool available;
} sx_serial_resource_info_t;

size_t sx_serial_resource_get_all(sx_serial_resource_info_t *resources, size_t capacity);
int sx_serial_resource_tx_channel(int logical_channel);
int sx_serial_resource_rx_channel(int logical_channel);
int sx_serial_resource_logical_from_physical_rx(int physical_channel);

#ifdef __cplusplus
}
#endif
