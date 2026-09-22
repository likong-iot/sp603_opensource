#include "sx_serial_port_manager.h"
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "nvs.h"
#include "sdkconfig.h"

#define NVS_NAMESPACE "storage"
#define NVS_LAYOUT_KEY "serial_layout"
_Static_assert(sizeof(NVS_LAYOUT_KEY) <= NVS_KEY_NAME_MAX_SIZE,
               "serial layout NVS key exceeds the ESP-IDF limit");
static const char *TAG = "serial_ports";
static sx_serial_layout_t s_layout = SX_SERIAL_LAYOUT_DUAL_RS485;

bool sx_serial_port_manager_uart0_reserved(void)
{
#if defined(CONFIG_ESP_CONSOLE_UART) && CONFIG_ESP_CONSOLE_UART && defined(CONFIG_ESP_CONSOLE_UART_NUM) && CONFIG_ESP_CONSOLE_UART_NUM == 0
    return true;
#elif defined(CONFIG_ESP_CONSOLE_UART_DEFAULT) && CONFIG_ESP_CONSOLE_UART_DEFAULT
    return true;
#else
    return false;
#endif
}

const char *sx_serial_layout_name(sx_serial_layout_t layout)
{
    return layout == SX_SERIAL_LAYOUT_RS422 ? "rs422" : "dual_rs485";
}

sx_serial_layout_t sx_serial_port_manager_get_layout(void) { return s_layout; }

bool sx_serial_layout_from_name(const char *name, sx_serial_layout_t *layout)
{
    if (!name || !layout) return false;
    if (!strcmp(name, "dual_rs485")) *layout = SX_SERIAL_LAYOUT_DUAL_RS485;
    else if (!strcmp(name, "rs422")) *layout = SX_SERIAL_LAYOUT_RS422;
    else return false;
    return true;
}

esp_err_t sx_serial_port_manager_init(void)
{
    s_layout = SX_SERIAL_LAYOUT_DUAL_RS485;
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;
    char value[20] = {0};
    size_t len = sizeof(value);
    if (nvs_get_str(nvs, NVS_LAYOUT_KEY, value, &len) == ESP_OK) {
        sx_serial_layout_t loaded;
        if (sx_serial_layout_from_name(value, &loaded)) s_layout = loaded;
    }
    nvs_close(nvs);
    if (s_layout == SX_SERIAL_LAYOUT_RS422 && sx_serial_port_manager_uart0_reserved()) {
        ESP_LOGW(TAG, "RS422 requested but UART0 debug console is reserved; fallback to dual_rs485");
        s_layout = SX_SERIAL_LAYOUT_DUAL_RS485;
    }
    ESP_LOGI(TAG, "layout=%s, UART0=%s", sx_serial_layout_name(s_layout),
             sx_serial_port_manager_uart0_reserved() ? "reserved_by_debug_console" : "available");
    return ESP_OK;
}

esp_err_t sx_serial_port_manager_save_layout(sx_serial_layout_t layout, char *reason, size_t reason_size)
{
    if (reason && reason_size) reason[0] = '\0';
    if (layout != SX_SERIAL_LAYOUT_DUAL_RS485 && layout != SX_SERIAL_LAYOUT_RS422) {
        if (reason && reason_size) snprintf(reason, reason_size, "unsupported serial layout");
        return ESP_ERR_INVALID_ARG;
    }
    if (layout == SX_SERIAL_LAYOUT_RS422 && sx_serial_port_manager_uart0_reserved()) {
        if (reason && reason_size) snprintf(reason, reason_size, "COM1 is reserved by UART0 debug console");
        return ESP_ERR_INVALID_STATE;
    }

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, NVS_LAYOUT_KEY, sx_serial_layout_name(layout));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "layout saved to NVS: key=%s value=%s (active after reboot)",
                 NVS_LAYOUT_KEY, sx_serial_layout_name(layout));
    } else {
        const char *error_name = esp_err_to_name(err);
        ESP_LOGE(TAG, "save layout failed: key=%s value=%s err=%s (0x%x)",
                 NVS_LAYOUT_KEY, sx_serial_layout_name(layout), error_name, (unsigned)err);
        if (reason && reason_size) {
            snprintf(reason, reason_size, "NVS save failed: %s", error_name);
        }
    }
    return err;
}

size_t sx_serial_port_manager_get_capabilities(sx_serial_port_capability_t *ports, size_t capacity)
{
    const bool debug = sx_serial_port_manager_uart0_reserved();
    const bool rs422 = s_layout == SX_SERIAL_LAYOUT_RS422;
    const sx_serial_port_capability_t dual[] = {
        {"com1", "COM1", 3, true, !debug, debug ? "uart0_debug_console" : ""},
        {"com2", "COM2", 1, true, true, ""},
        {"rs232", "RS232", 2, true, true, ""},
    };
    const sx_serial_port_capability_t rs422_resource[] = {
        {"rs422", "RS422", 1, true, !debug, debug ? "uart0_debug_console" : ""},
        {"rs232", "RS232", 2, true, true, ""},
    };
    const sx_serial_port_capability_t *all = rs422 ? rs422_resource : dual;
    const size_t count = rs422 ? sizeof(rs422_resource) / sizeof(rs422_resource[0])
                               : sizeof(dual) / sizeof(dual[0]);
    if (ports) memcpy(ports, all, (capacity < count ? capacity : count) * sizeof(*ports));
    return count;
}

bool sx_serial_port_manager_port_available(int port)
{
    if (s_layout == SX_SERIAL_LAYOUT_RS422) return port == 1 || port == 2;
    if (port == 1 || port == 2) return true;
    return port == 3 && !sx_serial_port_manager_uart0_reserved();
}

const char *sx_serial_port_manager_port_key(int port)
{
    if (s_layout == SX_SERIAL_LAYOUT_RS422 && port == 1) return "rs422";
    if (port == 1) return "com2";
    if (port == 2) return "rs232";
    if (port == 3) return "com1";
    return "unknown";
}

const char *sx_serial_port_manager_port_label(int port)
{
    if (s_layout == SX_SERIAL_LAYOUT_RS422 && port == 1) return "RS422";
    if (port == 1) return "COM2";
    if (port == 2) return "RS232";
    if (port == 3) return "COM1";
    return "未知串口";
}
