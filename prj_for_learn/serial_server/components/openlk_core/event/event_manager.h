/**
 * @file event_manager.h
 * @brief Event Manager Interface
 *
 * Provides a centralized event system for inter-component communication.
 * Supports event subscription, publishing, and asynchronous event handling.
 */

#ifndef OPENLK_EVENT_MANAGER_H
#define OPENLK_EVENT_MANAGER_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Event types
 */
typedef enum {
    // System events
    EVENT_SYSTEM_INIT = 0,
    EVENT_SYSTEM_READY,
    EVENT_SYSTEM_ERROR,
    EVENT_SYSTEM_RESTART,

    // Network events
    EVENT_NETWORK_CONNECTED,
    EVENT_NETWORK_DISCONNECTED,
    EVENT_NETWORK_IP_ASSIGNED,
    EVENT_NETWORK_ERROR,

    // WiFi events
    EVENT_WIFI_STA_CONNECTED,
    EVENT_WIFI_STA_DISCONNECTED,
    EVENT_WIFI_AP_STARTED,
    EVENT_WIFI_AP_STOPPED,
    EVENT_WIFI_AP_CLIENT_CONNECTED,
    EVENT_WIFI_AP_CLIENT_DISCONNECTED,

    // Ethernet events
    EVENT_ETH_CONNECTED,
    EVENT_ETH_DISCONNECTED,
    EVENT_ETH_LINK_UP,
    EVENT_ETH_LINK_DOWN,

    // UART events
    EVENT_UART_DATA_RECEIVED,
    EVENT_UART_ERROR,
    EVENT_UART_OVERFLOW,

    // TCP events
    EVENT_TCP_SERVER_STARTED,
    EVENT_TCP_SERVER_STOPPED,
    EVENT_TCP_CLIENT_CONNECTED,
    EVENT_TCP_CLIENT_DISCONNECTED,
    EVENT_TCP_DATA_RECEIVED,
    EVENT_TCP_ERROR,

    // UDP events
    EVENT_UDP_SERVER_STARTED,
    EVENT_UDP_SERVER_STOPPED,
    EVENT_UDP_DATA_RECEIVED,
    EVENT_UDP_ERROR,

    // MQTT events
    EVENT_MQTT_CONNECTED,
    EVENT_MQTT_DISCONNECTED,
    EVENT_MQTT_SUBSCRIBED,
    EVENT_MQTT_UNSUBSCRIBED,
    EVENT_MQTT_PUBLISHED,
    EVENT_MQTT_DATA_RECEIVED,
    EVENT_MQTT_ERROR,

    // HTTP events
    EVENT_HTTP_SERVER_STARTED,
    EVENT_HTTP_SERVER_STOPPED,
    EVENT_HTTP_REQUEST_RECEIVED,
    EVENT_HTTP_ERROR,

    // GPIO events
    EVENT_GPIO_BUTTON_PRESSED,
    EVENT_GPIO_BUTTON_RELEASED,
    EVENT_GPIO_INTERRUPT,

    // Configuration events
    EVENT_CONFIG_CHANGED,
    EVENT_CONFIG_RESET,
    EVENT_CONFIG_SAVED,

    // User-defined events
    EVENT_USER_DEFINED_START = 1000,

    EVENT_TYPE_MAX = 2000
} event_type_t;

/**
 * @brief Event data structure
 */
typedef struct {
    event_type_t type;          // Event type
    void *data;                 // Event data (can be NULL)
    size_t data_len;            // Data length
    uint32_t timestamp;         // Event timestamp (milliseconds since boot)
} event_data_t;

/**
 * @brief Event callback function
 *
 * @param event Event data
 * @param user_data User-provided context data
 */
typedef void (*event_callback_t)(const event_data_t *event, void *user_data);

/**
 * @brief Event handler structure
 */
typedef struct event_handler event_handler_t;

/**
 * @brief Initialize event manager
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_manager_init(void);

/**
 * @brief Deinitialize event manager
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_manager_deinit(void);

/**
 * @brief Subscribe to an event
 *
 * @param type Event type to subscribe to
 * @param callback Callback function to invoke when event occurs
 * @param user_data User data passed to callback
 * @param out_handler Output handler (can be used to unsubscribe)
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_subscribe(event_type_t type,
                          event_callback_t callback,
                          void *user_data,
                          event_handler_t **out_handler);

/**
 * @brief Unsubscribe from an event
 *
 * @param handler Event handler returned by event_subscribe
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_unsubscribe(event_handler_t *handler);

/**
 * @brief Publish an event (synchronous)
 *
 * All registered callbacks will be invoked immediately in the calling context.
 *
 * @param type Event type
 * @param data Event data (can be NULL)
 * @param data_len Data length
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_publish(event_type_t type, const void *data, size_t data_len);

/**
 * @brief Publish an event (asynchronous)
 *
 * Event is queued and callbacks will be invoked by the event task.
 *
 * @param type Event type
 * @param data Event data (will be copied)
 * @param data_len Data length
 * @param timeout_ms Timeout in milliseconds to wait for queue space
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_post(event_type_t type, const void *data, size_t data_len, uint32_t timeout_ms);

/**
 * @brief Get event type name
 *
 * @param type Event type
 * @return Event type name string
 */
const char *event_get_type_name(event_type_t type);

/**
 * @brief Get number of subscribers for an event type
 *
 * @param type Event type
 * @return Number of subscribers
 */
int event_get_subscriber_count(event_type_t type);

/**
 * @brief Enable/disable event logging
 *
 * @param enable true to enable, false to disable
 */
void event_set_logging(bool enable);

/**
 * @brief Get event queue statistics
 *
 * @param queue_size Current queue size
 * @param queue_capacity Maximum queue capacity
 * @param dropped_events Number of dropped events
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t event_get_stats(int *queue_size, int *queue_capacity, int *dropped_events);

#ifdef __cplusplus
}
#endif

#endif // OPENLK_EVENT_MANAGER_H
