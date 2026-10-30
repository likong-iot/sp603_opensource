#include "sx_work_mode.h"

#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_check.h"
#include "esp_log.h"
#include "sx_auto_collect.h"
#include "sx_serial_port_manager.h"
#include "sx_serial_server.h"

static const char *TAG = "work_mode";
static _Atomic(const char *) s_current_mode;
static atomic_bool s_mode_started;
static SemaphoreHandle_t s_mode_mutex;
static const char *const s_supported_modes[] = {"serial_server", "auto_collect"};
static bool s_initialized;

esp_err_t sx_work_mode_init(void)
{
    if (s_initialized) return ESP_OK;
    ESP_RETURN_ON_ERROR(sx_serial_server_init(), TAG, "serial server init failed");
    if (s_mode_mutex == NULL) s_mode_mutex = xSemaphoreCreateMutex();
    if (s_mode_mutex == NULL) return ESP_ERR_NO_MEM;
    s_initialized = true;
    return ESP_OK;
}

bool sx_work_mode_is_valid(const char *name)
{
    if (name == NULL) return false;
    if (strcmp(name, "serial_server") == 0) return true;
    if (strcmp(name, "auto_collect") == 0) {
        return sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_DUAL_RS485 &&
               sx_serial_port_manager_port_available(1) &&
               sx_serial_port_manager_port_available(3);
    }
    return false;
}

/* Mode transitions are serialized; UART callbacks never take this long lock. */
static esp_err_t stop_current_locked(void)
{
    const char *current = atomic_load(&s_current_mode);
    if (current == NULL) return ESP_OK;
    atomic_store(&s_mode_started, false);
    esp_err_t err = strcmp(current, "auto_collect") == 0
                        ? sx_auto_collect_deinit() : sx_serial_server_stop();
    if (err == ESP_OK) atomic_store(&s_current_mode, NULL);
    return err;
}

esp_err_t sx_work_mode_stop_current(void)
{
    if (!s_initialized) return ESP_OK;
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    esp_err_t err = stop_current_locked();
    xSemaphoreGive(s_mode_mutex);
    return err;
}

static esp_err_t start_mode_locked(const char *name)
{
    esp_err_t err = stop_current_locked();
    if (err != ESP_OK) return err;
    const char *mode = strcmp(name, "auto_collect") == 0
                         ? "auto_collect" : "serial_server";
    /* Retain ownership on failure so a timed-out cleanup can be retried. */
    atomic_store(&s_current_mode, mode);
    err = strcmp(mode, "auto_collect") == 0
              ? sx_auto_collect_init() : sx_serial_server_start();
    if (err != ESP_OK) {
        (void)stop_current_locked();
        ESP_LOGE(TAG, "start mode %s failed: %s", mode, esp_err_to_name(err));
        return err;
    }
    atomic_store(&s_mode_started, true);
    ESP_LOGI(TAG, "current mode: %s", mode);
    return ESP_OK;
}

esp_err_t sx_work_mode_start_by_name(const char *name)
{
    if (!s_initialized)
        ESP_RETURN_ON_ERROR(sx_work_mode_init(), TAG, "mode init failed");
    if (!sx_work_mode_is_valid(name)) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    const char *current = atomic_load(&s_current_mode);
    esp_err_t err = ESP_OK;
    if (!atomic_load(&s_mode_started) || current == NULL || strcmp(current, name) != 0)
        err = start_mode_locked(name);
    xSemaphoreGive(s_mode_mutex);
    return err;
}

esp_err_t sx_work_mode_reload_auto_collect(void)
{
    if (!s_initialized) return ESP_OK;
    xSemaphoreTake(s_mode_mutex, portMAX_DELAY);
    const char *current = atomic_load(&s_current_mode);
    esp_err_t err = ESP_OK;
    if (current != NULL && strcmp(current, "auto_collect") == 0)
        err = start_mode_locked("auto_collect");
    xSemaphoreGive(s_mode_mutex);
    return err;
}

const char *sx_work_mode_get_current_name(void)
{
    return atomic_load(&s_current_mode);
}

size_t sx_work_mode_get_supported_names(const char **names, size_t capacity)
{
    size_t count = 0;
    for (size_t i = 0; i < sizeof(s_supported_modes) / sizeof(s_supported_modes[0]); ++i) {
        if (!sx_work_mode_is_valid(s_supported_modes[i])) continue;
        if (names != NULL && count < capacity) names[count] = s_supported_modes[i];
        ++count;
    }
    return count;
}

sx_work_mode_uart_handler_t sx_work_mode_get_uart_handler(void)
{
    const char *current = atomic_load(&s_current_mode);
    return atomic_load(&s_mode_started) && current != NULL && strcmp(current, "serial_server") == 0
               ? sx_serial_server_handle_uart_data : NULL;
}

void sx_work_mode_handle_uart_data(int port, const uint8_t *data, size_t len)
{
    sx_work_mode_uart_handler_t handler = sx_work_mode_get_uart_handler();
    if (handler != NULL) handler(port, data, len);
}


int sx_work_mode_get_serial_server_port(int port)
{
    return sx_serial_server_get_port(port);
}

bool sx_work_mode_serial_server_listening(int port)
{
    return sx_serial_server_is_listening(port);
}

bool sx_work_mode_serial_client_connected(int port)
{
    return sx_serial_server_client_connected(port);
}

esp_err_t sx_work_mode_save_serial_server_ports(const uint16_t ports[3])
{
    return sx_serial_server_save_ports(ports);
}
