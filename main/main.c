#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

#include <stdio.h>

#include "async_uart.h"
#include "sx_led_manager.h"
#include "sx_button_manager.h"
#include "sx_serial_port_manager.h"
#include "sx_storage_defaults.h"
#include "sx_time_manager.h"
#include "sx_web_server.h"
#include "sx_work_mode.h"
#include "sx_network_manager.h"

static const char *TAG = "main";

static void set_all_gpio_drive_max(void)
{
    for (gpio_num_t pin = GPIO_NUM_0; pin <= GPIO_NUM_48; ++pin) {
        if (GPIO_IS_VALID_GPIO(pin)) {
            (void)gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
        }
    }
}

static esp_err_t platform_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp-netif init failed");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(err, TAG, "default event loop init failed");
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_ERROR_CHECK(platform_init());
    ESP_ERROR_CHECK(sx_storage_defaults_init());
    set_all_gpio_drive_max();
    ESP_ERROR_CHECK(sx_led_manager_init());
    ESP_ERROR_CHECK(sx_button_manager_init());
    ESP_ERROR_CHECK(sx_serial_port_manager_init());
    esp_err_t network_init_err = sx_network_manager_init();
    if (network_init_err != ESP_OK) {
        sx_led_manager_set_system_state(SX_LED_SYSTEM_ERROR);
        ESP_ERROR_CHECK(network_init_err);
    }
    esp_err_t network_err = sx_network_manager_start_management_network();
    if (network_err != ESP_OK) {
        ESP_LOGE(TAG, "management network failed: %s",
                 esp_err_to_name(network_err));
    }
    /* Make the management UI reachable before optional uplinks and serial
     * services are initialized. Their status endpoints tolerate interfaces
     * that are still starting. */
    http_server_init();
    ESP_ERROR_CHECK(sx_web_server_init_storage_defaults());

    esp_err_t remaining_network_err = sx_network_manager_start_remaining_networks();
    if (remaining_network_err != ESP_OK) {
        ESP_LOGE(TAG, "one or more configured uplinks failed: %s",
                 esp_err_to_name(remaining_network_err));
        if (network_err == ESP_OK) network_err = remaining_network_err;
    }
    uart_init();
    create_multi_uart_rx_tasks();
    time_manager_init();
    esp_err_t work_mode_init_err = sx_work_mode_init();
    if (work_mode_init_err != ESP_OK) {
        sx_led_manager_set_system_state(SX_LED_SYSTEM_ERROR);
        ESP_ERROR_CHECK(work_mode_init_err);
    }

    char work_mode[32] = {0};
    nvs_handle_t nvs = 0;
    if (nvs_open("storage", NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(work_mode);
        if (nvs_get_str(nvs, "w_mode", work_mode, &len) != ESP_OK ||
            !sx_work_mode_is_valid(work_mode)) {
            snprintf(work_mode, sizeof(work_mode), "serial_server");
        }
        nvs_close(nvs);
    } else {
        snprintf(work_mode, sizeof(work_mode), "serial_server");
    }
    esp_err_t work_mode_start_err = sx_work_mode_start_by_name(work_mode);
    if (work_mode_start_err != ESP_OK) {
        sx_led_manager_set_system_state(SX_LED_SYSTEM_ERROR);
        ESP_ERROR_CHECK(work_mode_start_err);
    }
    sx_network_manager_mark_application_ready(network_err == ESP_OK);

    ESP_LOGI(TAG, "hardware drivers initialized");
}
