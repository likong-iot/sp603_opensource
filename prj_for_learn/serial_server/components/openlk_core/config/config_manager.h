/**
 * @file config_manager.h
 * @brief Configuration Manager - Unified NVS configuration management
 *
 * Provides type-safe configuration management with validation, defaults,
 * and import/export capabilities.
 */

#ifndef OPENLK_CONFIG_MANAGER_H
#define OPENLK_CONFIG_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration key enumeration
 *
 * All configuration items are defined here for type safety
 * and centralized management.
 */
typedef enum {
    // ===== System Configuration =====
    CONFIG_DEVICE_NAME,              ///< Device name (string)
    CONFIG_DEVICE_ID,                ///< Device unique ID (string)
    CONFIG_FIRMWARE_VERSION,         ///< Firmware version (string)

    // ===== Network Configuration =====
    // WiFi
    CONFIG_WIFI_MODE,                ///< WiFi mode: "ap", "sta", "apsta" (string)
    CONFIG_WIFI_STA_SSID,            ///< WiFi STA SSID (string)
    CONFIG_WIFI_STA_PASSWORD,        ///< WiFi STA password (string)
    CONFIG_WIFI_AP_SSID,             ///< WiFi AP SSID (string)
    CONFIG_WIFI_AP_PASSWORD,         ///< WiFi AP password (string)
    CONFIG_WIFI_AP_CHANNEL,          ///< WiFi AP channel (int: 1-13)
    CONFIG_WIFI_AP_MAX_CONN,         ///< WiFi AP max connections (int: 1-10)

    // Ethernet
    CONFIG_ETH_ENABLED,              ///< Ethernet enabled (bool)
    CONFIG_ETH_USE_DHCP,             ///< Use DHCP (bool)
    CONFIG_ETH_IP,                   ///< Static IP address (string)
    CONFIG_ETH_NETMASK,              ///< Netmask (string)
    CONFIG_ETH_GATEWAY,              ///< Gateway (string)
    CONFIG_ETH_DNS,                  ///< DNS server (string)

    // ===== UART Configuration =====
    CONFIG_UART_PORT,                ///< UART port number (int: 0-2)
    CONFIG_UART_BAUDRATE,            ///< Baud rate (int: 9600-921600)
    CONFIG_UART_DATA_BITS,           ///< Data bits (int: 5-8)
    CONFIG_UART_STOP_BITS,           ///< Stop bits (int: 1-2)
    CONFIG_UART_PARITY,              ///< Parity: 0=none, 1=odd, 2=even (int)
    CONFIG_UART_FLOW_CONTROL,        ///< Flow control: 0=none, 1=rts, 2=cts, 3=rts+cts (int)
    CONFIG_UART_RX_BUFFER_SIZE,      ///< RX buffer size (int)
    CONFIG_UART_TX_BUFFER_SIZE,      ///< TX buffer size (int)

    // ===== Protocol Configuration =====
    // TCP Server
    CONFIG_TCP_SERVER_ENABLED,       ///< TCP server enabled (bool)
    CONFIG_TCP_SERVER_PORT,          ///< TCP server port (int: 1-65535)
    CONFIG_TCP_SERVER_MAX_CLIENTS,   ///< Max clients (int: 1-10)
    CONFIG_TCP_SERVER_TIMEOUT,       ///< Client timeout in seconds (int)

    // MQTT Client
    CONFIG_MQTT_ENABLED,             ///< MQTT enabled (bool)
    CONFIG_MQTT_BROKER,              ///< Broker address (string)
    CONFIG_MQTT_PORT,                ///< Broker port (int: 1-65535)
    CONFIG_MQTT_USERNAME,            ///< Username (string)
    CONFIG_MQTT_PASSWORD,            ///< Password (string)
    CONFIG_MQTT_CLIENT_ID,           ///< Client ID (string)
    CONFIG_MQTT_TOPIC_PUB,           ///< Publish topic (string)
    CONFIG_MQTT_TOPIC_SUB,           ///< Subscribe topic (string)
    CONFIG_MQTT_QOS,                 ///< QoS level (int: 0-2)
    CONFIG_MQTT_KEEPALIVE,           ///< Keep-alive interval in seconds (int)

    // ===== GPIO Configuration =====
    CONFIG_GPIO_LED_PIN,             ///< LED GPIO pin (int)
    CONFIG_GPIO_LED_ACTIVE_LEVEL,    ///< LED active level (bool: 0=low, 1=high)
    CONFIG_GPIO_BUTTON_PIN,          ///< Button GPIO pin (int)
    CONFIG_GPIO_BUTTON_ACTIVE_LEVEL, ///< Button active level (bool)

    CONFIG_KEY_MAX                   ///< Maximum key count
} config_key_t;

/**
 * @brief Configuration value type
 */
typedef enum {
    CONFIG_TYPE_STRING,              ///< String value
    CONFIG_TYPE_INT,                 ///< Integer value
    CONFIG_TYPE_BOOL,                ///< Boolean value
    CONFIG_TYPE_BLOB                 ///< Binary blob
} config_type_t;

/**
 * @brief Initialize configuration manager
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_FAIL: Initialization failed
 */
esp_err_t config_manager_init(void);

/**
 * @brief Deinitialize configuration manager
 *
 * @return ESP_OK on success
 */
esp_err_t config_manager_deinit(void);

// ===== Read Configuration =====

/**
 * @brief Get string configuration value
 *
 * @param key Configuration key
 * @param out_value Output buffer
 * @param max_len Maximum buffer length
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NVS_NOT_FOUND: Key not found (default value returned)
 */
esp_err_t config_get_string(config_key_t key, char *out_value, size_t max_len);

/**
 * @brief Get integer configuration value
 *
 * @param key Configuration key
 * @param out_value Output value pointer
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NVS_NOT_FOUND: Key not found (default value returned)
 */
esp_err_t config_get_int(config_key_t key, int32_t *out_value);

/**
 * @brief Get boolean configuration value
 *
 * @param key Configuration key
 * @param out_value Output value pointer
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NVS_NOT_FOUND: Key not found (default value returned)
 */
esp_err_t config_get_bool(config_key_t key, bool *out_value);

/**
 * @brief Get blob configuration value
 *
 * @param key Configuration key
 * @param out_value Output buffer
 * @param length Input: max length, Output: actual length
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_ERR_NVS_NOT_FOUND: Key not found
 */
esp_err_t config_get_blob(config_key_t key, void *out_value, size_t *length);

// ===== Write Configuration =====

/**
 * @brief Set string configuration value
 *
 * @param key Configuration key
 * @param value String value
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments or validation failed
 *     - ESP_FAIL: Write failed
 */
esp_err_t config_set_string(config_key_t key, const char *value);

/**
 * @brief Set integer configuration value
 *
 * @param key Configuration key
 * @param value Integer value
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments or validation failed
 *     - ESP_FAIL: Write failed
 */
esp_err_t config_set_int(config_key_t key, int32_t value);

/**
 * @brief Set boolean configuration value
 *
 * @param key Configuration key
 * @param value Boolean value
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_FAIL: Write failed
 */
esp_err_t config_set_bool(config_key_t key, bool value);

/**
 * @brief Set blob configuration value
 *
 * @param key Configuration key
 * @param value Blob data
 * @param length Data length
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid arguments
 *     - ESP_FAIL: Write failed
 */
esp_err_t config_set_blob(config_key_t key, const void *value, size_t length);

// ===== Management =====

/**
 * @brief Delete a configuration value
 *
 * @param key Configuration key
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_NVS_NOT_FOUND: Key not found
 */
esp_err_t config_delete(config_key_t key);

/**
 * @brief Reset all configuration to factory defaults
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_FAIL: Reset failed
 */
esp_err_t config_reset_to_defaults(void);

/**
 * @brief Check if a configuration key exists
 *
 * @param key Configuration key
 *
 * @return true if key exists, false otherwise
 */
bool config_exists(config_key_t key);

/**
 * @brief Validate configuration value
 *
 * @param key Configuration key
 * @param value Value pointer (type depends on key)
 *
 * @return
 *     - ESP_OK: Valid
 *     - ESP_ERR_INVALID_ARG: Invalid value
 */
esp_err_t config_validate(config_key_t key, const void *value);

// ===== Import/Export =====

/**
 * @brief Export all configuration to JSON string
 *
 * @param json_buf Output JSON buffer
 * @param buf_size Buffer size
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_SIZE: Buffer too small
 */
esp_err_t config_export_json(char *json_buf, size_t buf_size);

/**
 * @brief Import configuration from JSON string
 *
 * @param json_str JSON string
 *
 * @return
 *     - ESP_OK: Success
 *     - ESP_ERR_INVALID_ARG: Invalid JSON
 */
esp_err_t config_import_json(const char *json_str);

/**
 * @brief Print all configuration (for debugging)
 */
void config_print_all(void);

#ifdef __cplusplus
}
#endif

#endif // OPENLK_CONFIG_MANAGER_H
