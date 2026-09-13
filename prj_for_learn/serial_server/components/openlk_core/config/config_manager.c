/**
 * @file config_manager.c
 * @brief Configuration Manager Implementation
 */

#include "config_manager.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "config_mgr";

#define CONFIG_NAMESPACE "openlk_cfg"
#define MAX_KEY_NAME_LEN 15

// Configuration metadata
typedef struct {
    const char *name;           // NVS key name
    config_type_t type;         // Value type
    const void *default_value;  // Default value pointer
} config_meta_t;

// Default values
static const char *DEFAULT_DEVICE_NAME = "OpenLK-Device";
static const char *DEFAULT_DEVICE_ID = "OPENLK-000000";
static const char *DEFAULT_FIRMWARE_VERSION = "1.0.0";
static const char *DEFAULT_WIFI_MODE = "ap";
static const char *DEFAULT_WIFI_STA_SSID = "";
static const char *DEFAULT_WIFI_STA_PASSWORD = "";
static const char *DEFAULT_WIFI_AP_SSID = "OpenLK-AP";
static const char *DEFAULT_WIFI_AP_PASSWORD = "12345678";
static const int32_t DEFAULT_WIFI_AP_CHANNEL = 6;
static const int32_t DEFAULT_WIFI_AP_MAX_CONN = 4;
static const bool DEFAULT_ETH_ENABLED = false;
static const bool DEFAULT_ETH_USE_DHCP = true;
static const char *DEFAULT_ETH_IP = "192.168.1.100";
static const char *DEFAULT_ETH_NETMASK = "255.255.255.0";
static const char *DEFAULT_ETH_GATEWAY = "192.168.1.1";
static const char *DEFAULT_ETH_DNS = "8.8.8.8";
static const int32_t DEFAULT_UART_PORT = 1;
static const int32_t DEFAULT_UART_BAUDRATE = 115200;
static const int32_t DEFAULT_UART_DATA_BITS = 8;
static const int32_t DEFAULT_UART_STOP_BITS = 1;
static const int32_t DEFAULT_UART_PARITY = 0;
static const int32_t DEFAULT_UART_FLOW_CONTROL = 0;
static const int32_t DEFAULT_UART_RX_BUFFER_SIZE = 1024;
static const int32_t DEFAULT_UART_TX_BUFFER_SIZE = 1024;
static const bool DEFAULT_TCP_SERVER_ENABLED = true;
static const int32_t DEFAULT_TCP_SERVER_PORT = 8080;
static const int32_t DEFAULT_TCP_SERVER_MAX_CLIENTS = 5;
static const int32_t DEFAULT_TCP_SERVER_TIMEOUT = 120;
static const bool DEFAULT_MQTT_ENABLED = false;
static const char *DEFAULT_MQTT_BROKER = "mqtt://broker.hivemq.com";
static const int32_t DEFAULT_MQTT_PORT = 1883;
static const char *DEFAULT_MQTT_USERNAME = "";
static const char *DEFAULT_MQTT_PASSWORD = "";
static const char *DEFAULT_MQTT_CLIENT_ID = "openlk_client";
static const char *DEFAULT_MQTT_TOPIC_PUB = "openlk/data";
static const char *DEFAULT_MQTT_TOPIC_SUB = "openlk/cmd";
static const int32_t DEFAULT_MQTT_QOS = 0;
static const int32_t DEFAULT_MQTT_KEEPALIVE = 120;
static const int32_t DEFAULT_GPIO_LED_PIN = 2;
static const bool DEFAULT_GPIO_LED_ACTIVE_LEVEL = true;
static const int32_t DEFAULT_GPIO_BUTTON_PIN = 0;
static const bool DEFAULT_GPIO_BUTTON_ACTIVE_LEVEL = false;

// Configuration metadata table
static const config_meta_t config_meta_table[CONFIG_KEY_MAX] = {
    [CONFIG_DEVICE_NAME] = {"dev_name", CONFIG_TYPE_STRING, DEFAULT_DEVICE_NAME},
    [CONFIG_DEVICE_ID] = {"dev_id", CONFIG_TYPE_STRING, DEFAULT_DEVICE_ID},
    [CONFIG_FIRMWARE_VERSION] = {"fw_ver", CONFIG_TYPE_STRING, DEFAULT_FIRMWARE_VERSION},

    [CONFIG_WIFI_MODE] = {"wifi_mode", CONFIG_TYPE_STRING, DEFAULT_WIFI_MODE},
    [CONFIG_WIFI_STA_SSID] = {"sta_ssid", CONFIG_TYPE_STRING, DEFAULT_WIFI_STA_SSID},
    [CONFIG_WIFI_STA_PASSWORD] = {"sta_pass", CONFIG_TYPE_STRING, DEFAULT_WIFI_STA_PASSWORD},
    [CONFIG_WIFI_AP_SSID] = {"ap_ssid", CONFIG_TYPE_STRING, DEFAULT_WIFI_AP_SSID},
    [CONFIG_WIFI_AP_PASSWORD] = {"ap_pass", CONFIG_TYPE_STRING, DEFAULT_WIFI_AP_PASSWORD},
    [CONFIG_WIFI_AP_CHANNEL] = {"ap_channel", CONFIG_TYPE_INT, &DEFAULT_WIFI_AP_CHANNEL},
    [CONFIG_WIFI_AP_MAX_CONN] = {"ap_max_conn", CONFIG_TYPE_INT, &DEFAULT_WIFI_AP_MAX_CONN},

    [CONFIG_ETH_ENABLED] = {"eth_en", CONFIG_TYPE_BOOL, &DEFAULT_ETH_ENABLED},
    [CONFIG_ETH_USE_DHCP] = {"eth_dhcp", CONFIG_TYPE_BOOL, &DEFAULT_ETH_USE_DHCP},
    [CONFIG_ETH_IP] = {"eth_ip", CONFIG_TYPE_STRING, DEFAULT_ETH_IP},
    [CONFIG_ETH_NETMASK] = {"eth_netmask", CONFIG_TYPE_STRING, DEFAULT_ETH_NETMASK},
    [CONFIG_ETH_GATEWAY] = {"eth_gateway", CONFIG_TYPE_STRING, DEFAULT_ETH_GATEWAY},
    [CONFIG_ETH_DNS] = {"eth_dns", CONFIG_TYPE_STRING, DEFAULT_ETH_DNS},

    [CONFIG_UART_PORT] = {"uart_port", CONFIG_TYPE_INT, &DEFAULT_UART_PORT},
    [CONFIG_UART_BAUDRATE] = {"uart_baud", CONFIG_TYPE_INT, &DEFAULT_UART_BAUDRATE},
    [CONFIG_UART_DATA_BITS] = {"uart_data", CONFIG_TYPE_INT, &DEFAULT_UART_DATA_BITS},
    [CONFIG_UART_STOP_BITS] = {"uart_stop", CONFIG_TYPE_INT, &DEFAULT_UART_STOP_BITS},
    [CONFIG_UART_PARITY] = {"uart_parity", CONFIG_TYPE_INT, &DEFAULT_UART_PARITY},
    [CONFIG_UART_FLOW_CONTROL] = {"uart_flow", CONFIG_TYPE_INT, &DEFAULT_UART_FLOW_CONTROL},
    [CONFIG_UART_RX_BUFFER_SIZE] = {"uart_rx_buf", CONFIG_TYPE_INT, &DEFAULT_UART_RX_BUFFER_SIZE},
    [CONFIG_UART_TX_BUFFER_SIZE] = {"uart_tx_buf", CONFIG_TYPE_INT, &DEFAULT_UART_TX_BUFFER_SIZE},

    [CONFIG_TCP_SERVER_ENABLED] = {"tcp_en", CONFIG_TYPE_BOOL, &DEFAULT_TCP_SERVER_ENABLED},
    [CONFIG_TCP_SERVER_PORT] = {"tcp_port", CONFIG_TYPE_INT, &DEFAULT_TCP_SERVER_PORT},
    [CONFIG_TCP_SERVER_MAX_CLIENTS] = {"tcp_max_cli", CONFIG_TYPE_INT, &DEFAULT_TCP_SERVER_MAX_CLIENTS},
    [CONFIG_TCP_SERVER_TIMEOUT] = {"tcp_timeout", CONFIG_TYPE_INT, &DEFAULT_TCP_SERVER_TIMEOUT},

    [CONFIG_MQTT_ENABLED] = {"mqtt_en", CONFIG_TYPE_BOOL, &DEFAULT_MQTT_ENABLED},
    [CONFIG_MQTT_BROKER] = {"mqtt_broker", CONFIG_TYPE_STRING, DEFAULT_MQTT_BROKER},
    [CONFIG_MQTT_PORT] = {"mqtt_port", CONFIG_TYPE_INT, &DEFAULT_MQTT_PORT},
    [CONFIG_MQTT_USERNAME] = {"mqtt_user", CONFIG_TYPE_STRING, DEFAULT_MQTT_USERNAME},
    [CONFIG_MQTT_PASSWORD] = {"mqtt_pass", CONFIG_TYPE_STRING, DEFAULT_MQTT_PASSWORD},
    [CONFIG_MQTT_CLIENT_ID] = {"mqtt_cid", CONFIG_TYPE_STRING, DEFAULT_MQTT_CLIENT_ID},
    [CONFIG_MQTT_TOPIC_PUB] = {"mqtt_pub", CONFIG_TYPE_STRING, DEFAULT_MQTT_TOPIC_PUB},
    [CONFIG_MQTT_TOPIC_SUB] = {"mqtt_sub", CONFIG_TYPE_STRING, DEFAULT_MQTT_TOPIC_SUB},
    [CONFIG_MQTT_QOS] = {"mqtt_qos", CONFIG_TYPE_INT, &DEFAULT_MQTT_QOS},
    [CONFIG_MQTT_KEEPALIVE] = {"mqtt_keep", CONFIG_TYPE_INT, &DEFAULT_MQTT_KEEPALIVE},

    [CONFIG_GPIO_LED_PIN] = {"led_pin", CONFIG_TYPE_INT, &DEFAULT_GPIO_LED_PIN},
    [CONFIG_GPIO_LED_ACTIVE_LEVEL] = {"led_level", CONFIG_TYPE_BOOL, &DEFAULT_GPIO_LED_ACTIVE_LEVEL},
    [CONFIG_GPIO_BUTTON_PIN] = {"btn_pin", CONFIG_TYPE_INT, &DEFAULT_GPIO_BUTTON_PIN},
    [CONFIG_GPIO_BUTTON_ACTIVE_LEVEL] = {"btn_level", CONFIG_TYPE_BOOL, &DEFAULT_GPIO_BUTTON_ACTIVE_LEVEL},
};

static nvs_handle_t nvs_handle = 0;

esp_err_t config_manager_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition was truncated, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize NVS");
        return ret;
    }

    ret = nvs_open(CONFIG_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace");
        return ret;
    }

    ESP_LOGI(TAG, "Configuration manager initialized");
    return ESP_OK;
}

esp_err_t config_manager_deinit(void)
{
    if (nvs_handle) {
        nvs_close(nvs_handle);
        nvs_handle = 0;
    }
    return ESP_OK;
}

esp_err_t config_get_string(config_key_t key, char *out_value, size_t max_len)
{
    if (key >= CONFIG_KEY_MAX || !out_value || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];
    size_t length = max_len;

    esp_err_t ret = nvs_get_str(nvs_handle, meta->name, out_value, &length);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Return default value
        strncpy(out_value, (const char *)meta->default_value, max_len - 1);
        out_value[max_len - 1] = '\0';
        return ESP_OK;
    }

    return ret;
}

esp_err_t config_get_int(config_key_t key, int32_t *out_value)
{
    if (key >= CONFIG_KEY_MAX || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];

    esp_err_t ret = nvs_get_i32(nvs_handle, meta->name, out_value);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Return default value
        *out_value = *(const int32_t *)meta->default_value;
        return ESP_OK;
    }

    return ret;
}

esp_err_t config_get_bool(config_key_t key, bool *out_value)
{
    if (key >= CONFIG_KEY_MAX || !out_value) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];
    uint8_t value;

    esp_err_t ret = nvs_get_u8(nvs_handle, meta->name, &value);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        // Return default value
        *out_value = *(const bool *)meta->default_value;
        return ESP_OK;
    }

    *out_value = (value != 0);
    return ret;
}

esp_err_t config_get_blob(config_key_t key, void *out_value, size_t *length)
{
    if (key >= CONFIG_KEY_MAX || !out_value || !length) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];
    return nvs_get_blob(nvs_handle, meta->name, out_value, length);
}

esp_err_t config_set_string(config_key_t key, const char *value)
{
    if (key >= CONFIG_KEY_MAX || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];

    esp_err_t ret = nvs_set_str(nvs_handle, meta->name, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    return ret;
}

esp_err_t config_set_int(config_key_t key, int32_t value)
{
    if (key >= CONFIG_KEY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];

    esp_err_t ret = nvs_set_i32(nvs_handle, meta->name, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    return ret;
}

esp_err_t config_set_bool(config_key_t key, bool value)
{
    if (key >= CONFIG_KEY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];

    esp_err_t ret = nvs_set_u8(nvs_handle, meta->name, value ? 1 : 0);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    return ret;
}

esp_err_t config_set_blob(config_key_t key, const void *value, size_t length)
{
    if (key >= CONFIG_KEY_MAX || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];

    esp_err_t ret = nvs_set_blob(nvs_handle, meta->name, value, length);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    return ret;
}

esp_err_t config_delete(config_key_t key)
{
    if (key >= CONFIG_KEY_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    const config_meta_t *meta = &config_meta_table[key];

    esp_err_t ret = nvs_erase_key(nvs_handle, meta->name);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    return ret;
}

esp_err_t config_reset_to_defaults(void)
{
    esp_err_t ret = nvs_erase_all(nvs_handle);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
        ESP_LOGI(TAG, "Configuration reset to factory defaults");
    }

    return ret;
}

bool config_exists(config_key_t key)
{
    if (key >= CONFIG_KEY_MAX) {
        return false;
    }

    const config_meta_t *meta = &config_meta_table[key];
    size_t length;

    // Try to get the key based on type
    switch (meta->type) {
        case CONFIG_TYPE_STRING: {
            esp_err_t ret = nvs_get_str(nvs_handle, meta->name, NULL, &length);
            return (ret == ESP_OK);
        }
        case CONFIG_TYPE_INT: {
            int32_t value;
            return (nvs_get_i32(nvs_handle, meta->name, &value) == ESP_OK);
        }
        case CONFIG_TYPE_BOOL: {
            uint8_t value;
            return (nvs_get_u8(nvs_handle, meta->name, &value) == ESP_OK);
        }
        case CONFIG_TYPE_BLOB: {
            esp_err_t ret = nvs_get_blob(nvs_handle, meta->name, NULL, &length);
            return (ret == ESP_OK);
        }
        default:
            return false;
    }
}

esp_err_t config_validate(config_key_t key, const void *value)
{
    if (key >= CONFIG_KEY_MAX || !value) {
        return ESP_ERR_INVALID_ARG;
    }

    // Add validation rules here
    switch (key) {
        case CONFIG_UART_BAUDRATE: {
            int32_t baud = *(const int32_t *)value;
            if (baud < 9600 || baud > 921600) {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }
        case CONFIG_UART_DATA_BITS: {
            int32_t bits = *(const int32_t *)value;
            if (bits < 5 || bits > 8) {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }
        case CONFIG_TCP_SERVER_PORT:
        case CONFIG_MQTT_PORT: {
            int32_t port = *(const int32_t *)value;
            if (port < 1 || port > 65535) {
                return ESP_ERR_INVALID_ARG;
            }
            break;
        }
        default:
            break;
    }

    return ESP_OK;
}

esp_err_t config_export_json(char *json_buf, size_t buf_size)
{
    if (!json_buf || buf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Simple JSON export (without cJSON library for minimal dependencies)
    int offset = snprintf(json_buf, buf_size, "{");

    for (int i = 0; i < CONFIG_KEY_MAX; i++) {
        const config_meta_t *meta = &config_meta_table[i];

        if (offset >= buf_size - 50) {
            return ESP_ERR_INVALID_SIZE;
        }

        if (i > 0) {
            offset += snprintf(json_buf + offset, buf_size - offset, ",");
        }

        switch (meta->type) {
            case CONFIG_TYPE_STRING: {
                char value[128];
                config_get_string(i, value, sizeof(value));
                offset += snprintf(json_buf + offset, buf_size - offset,
                                 "\"%s\":\"%s\"", meta->name, value);
                break;
            }
            case CONFIG_TYPE_INT: {
                int32_t value;
                config_get_int(i, &value);
                offset += snprintf(json_buf + offset, buf_size - offset,
                                 "\"%s\":%d", meta->name, (int)value);
                break;
            }
            case CONFIG_TYPE_BOOL: {
                bool value;
                config_get_bool(i, &value);
                offset += snprintf(json_buf + offset, buf_size - offset,
                                 "\"%s\":%s", meta->name, value ? "true" : "false");
                break;
            }
            default:
                break;
        }
    }

    snprintf(json_buf + offset, buf_size - offset, "}");
    return ESP_OK;
}

esp_err_t config_import_json(const char *json_str)
{
    // Simplified JSON parsing - in production, use cJSON library
    ESP_LOGW(TAG, "JSON import not fully implemented yet");
    return ESP_ERR_NOT_SUPPORTED;
}

void config_print_all(void)
{
    ESP_LOGI(TAG, "=== Configuration Dump ===");

    for (int i = 0; i < CONFIG_KEY_MAX; i++) {
        const config_meta_t *meta = &config_meta_table[i];

        switch (meta->type) {
            case CONFIG_TYPE_STRING: {
                char value[128];
                config_get_string(i, value, sizeof(value));
                ESP_LOGI(TAG, "%s = \"%s\"", meta->name, value);
                break;
            }
            case CONFIG_TYPE_INT: {
                int32_t value;
                config_get_int(i, &value);
                ESP_LOGI(TAG, "%s = %d", meta->name, (int)value);
                break;
            }
            case CONFIG_TYPE_BOOL: {
                bool value;
                config_get_bool(i, &value);
                ESP_LOGI(TAG, "%s = %s", meta->name, value ? "true" : "false");
                break;
            }
            default:
                break;
        }
    }

    ESP_LOGI(TAG, "=========================");
}
