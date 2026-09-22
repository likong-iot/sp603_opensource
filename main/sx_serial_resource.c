#include "sx_serial_resource.h"
#include "sx_serial_port_manager.h"
#include <string.h>

size_t sx_serial_resource_get_all(sx_serial_resource_info_t *resources, size_t capacity)
{
    const bool rs422 = sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422;
    const bool debug_console = sx_serial_port_manager_uart0_reserved();
    const sx_serial_resource_info_t dual[] = {
        {"com1", "COM1", 3, 3, 3, !debug_console},
        {"com2", "COM2", 1, 1, 1, true},
        {"rs232", "RS232", 2, 2, 2, true},
    };
    const sx_serial_resource_info_t rs422_resources[] = {
        {"rs422", "RS422", 1, 1, 1, !debug_console},
        {"rs232", "RS232", 2, 2, 2, true},
    };
    const sx_serial_resource_info_t *source = rs422 ? rs422_resources : dual;
    const size_t count = rs422 ? (sizeof(rs422_resources) / sizeof(rs422_resources[0]))
                               : (sizeof(dual) / sizeof(dual[0]));
    if (resources != NULL) {
        const size_t copy_count = capacity < count ? capacity : count;
        memcpy(resources, source, copy_count * sizeof(*resources));
    }
    return count;
}

int sx_serial_resource_tx_port(int logical_port)
{
    /* async-uart keeps the RS422 direction split inside internal index 1. */
    return logical_port;
}

int sx_serial_resource_rx_port(int logical_port)
{
    return logical_port;
}

int sx_serial_resource_logical_from_physical_rx(int physical_port)
{
    if (sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422 && physical_port == 1) {
        return 1;
    }
    return physical_port;
}
