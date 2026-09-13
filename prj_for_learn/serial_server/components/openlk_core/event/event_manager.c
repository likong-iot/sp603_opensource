/**
 * @file event_manager.c
 * @brief Event Manager Implementation
 */

#include "event_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "event_mgr";

#define EVENT_QUEUE_SIZE 32
#define EVENT_TASK_STACK_SIZE 4096
#define EVENT_TASK_PRIORITY 5
#define MAX_HANDLERS_PER_EVENT 10

/**
 * @brief Event handler structure
 */
struct event_handler {
    event_type_t type;
    event_callback_t callback;
    void *user_data;
    struct event_handler *next;
};

/**
 * @brief Event queue item
 */
typedef struct {
    event_data_t event;
    uint8_t data_buffer[256];  // Embedded data storage
} event_queue_item_t;

// Event manager state
static struct {
    bool initialized;
    TaskHandle_t event_task;
    QueueHandle_t event_queue;
    SemaphoreHandle_t mutex;
    event_handler_t *handlers[EVENT_TYPE_MAX];
    int subscriber_counts[EVENT_TYPE_MAX];
    bool logging_enabled;
    int dropped_events;
} event_mgr = {0};

// Event type names
static const char *event_type_names[] = {
    [EVENT_SYSTEM_INIT] = "SYSTEM_INIT",
    [EVENT_SYSTEM_READY] = "SYSTEM_READY",
    [EVENT_SYSTEM_ERROR] = "SYSTEM_ERROR",
    [EVENT_SYSTEM_RESTART] = "SYSTEM_RESTART",

    [EVENT_NETWORK_CONNECTED] = "NETWORK_CONNECTED",
    [EVENT_NETWORK_DISCONNECTED] = "NETWORK_DISCONNECTED",
    [EVENT_NETWORK_IP_ASSIGNED] = "NETWORK_IP_ASSIGNED",
    [EVENT_NETWORK_ERROR] = "NETWORK_ERROR",

    [EVENT_WIFI_STA_CONNECTED] = "WIFI_STA_CONNECTED",
    [EVENT_WIFI_STA_DISCONNECTED] = "WIFI_STA_DISCONNECTED",
    [EVENT_WIFI_AP_STARTED] = "WIFI_AP_STARTED",
    [EVENT_WIFI_AP_STOPPED] = "WIFI_AP_STOPPED",
    [EVENT_WIFI_AP_CLIENT_CONNECTED] = "WIFI_AP_CLIENT_CONNECTED",
    [EVENT_WIFI_AP_CLIENT_DISCONNECTED] = "WIFI_AP_CLIENT_DISCONNECTED",

    [EVENT_ETH_CONNECTED] = "ETH_CONNECTED",
    [EVENT_ETH_DISCONNECTED] = "ETH_DISCONNECTED",
    [EVENT_ETH_LINK_UP] = "ETH_LINK_UP",
    [EVENT_ETH_LINK_DOWN] = "ETH_LINK_DOWN",

    [EVENT_UART_DATA_RECEIVED] = "UART_DATA_RECEIVED",
    [EVENT_UART_ERROR] = "UART_ERROR",
    [EVENT_UART_OVERFLOW] = "UART_OVERFLOW",

    [EVENT_TCP_SERVER_STARTED] = "TCP_SERVER_STARTED",
    [EVENT_TCP_SERVER_STOPPED] = "TCP_SERVER_STOPPED",
    [EVENT_TCP_CLIENT_CONNECTED] = "TCP_CLIENT_CONNECTED",
    [EVENT_TCP_CLIENT_DISCONNECTED] = "TCP_CLIENT_DISCONNECTED",
    [EVENT_TCP_DATA_RECEIVED] = "TCP_DATA_RECEIVED",
    [EVENT_TCP_ERROR] = "TCP_ERROR",

    [EVENT_UDP_SERVER_STARTED] = "UDP_SERVER_STARTED",
    [EVENT_UDP_SERVER_STOPPED] = "UDP_SERVER_STOPPED",
    [EVENT_UDP_DATA_RECEIVED] = "UDP_DATA_RECEIVED",
    [EVENT_UDP_ERROR] = "UDP_ERROR",

    [EVENT_MQTT_CONNECTED] = "MQTT_CONNECTED",
    [EVENT_MQTT_DISCONNECTED] = "MQTT_DISCONNECTED",
    [EVENT_MQTT_SUBSCRIBED] = "MQTT_SUBSCRIBED",
    [EVENT_MQTT_UNSUBSCRIBED] = "MQTT_UNSUBSCRIBED",
    [EVENT_MQTT_PUBLISHED] = "MQTT_PUBLISHED",
    [EVENT_MQTT_DATA_RECEIVED] = "MQTT_DATA_RECEIVED",
    [EVENT_MQTT_ERROR] = "MQTT_ERROR",

    [EVENT_HTTP_SERVER_STARTED] = "HTTP_SERVER_STARTED",
    [EVENT_HTTP_SERVER_STOPPED] = "HTTP_SERVER_STOPPED",
    [EVENT_HTTP_REQUEST_RECEIVED] = "HTTP_REQUEST_RECEIVED",
    [EVENT_HTTP_ERROR] = "HTTP_ERROR",

    [EVENT_GPIO_BUTTON_PRESSED] = "GPIO_BUTTON_PRESSED",
    [EVENT_GPIO_BUTTON_RELEASED] = "GPIO_BUTTON_RELEASED",
    [EVENT_GPIO_INTERRUPT] = "GPIO_INTERRUPT",

    [EVENT_CONFIG_CHANGED] = "CONFIG_CHANGED",
    [EVENT_CONFIG_RESET] = "CONFIG_RESET",
    [EVENT_CONFIG_SAVED] = "CONFIG_SAVED",
};

static void event_task(void *pvParameters);
static void invoke_callbacks(const event_data_t *event);

esp_err_t event_manager_init(void)
{
    if (event_mgr.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Create mutex
    event_mgr.mutex = xSemaphoreCreateMutex();
    if (!event_mgr.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    // Create event queue
    event_mgr.event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_queue_item_t));
    if (!event_mgr.event_queue) {
        ESP_LOGE(TAG, "Failed to create event queue");
        vSemaphoreDelete(event_mgr.mutex);
        return ESP_FAIL;
    }

    // Create event task
    BaseType_t ret = xTaskCreate(event_task, "event_task", EVENT_TASK_STACK_SIZE,
                                  NULL, EVENT_TASK_PRIORITY, &event_mgr.event_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create event task");
        vQueueDelete(event_mgr.event_queue);
        vSemaphoreDelete(event_mgr.mutex);
        return ESP_FAIL;
    }

    event_mgr.initialized = true;
    event_mgr.logging_enabled = false;
    event_mgr.dropped_events = 0;

    ESP_LOGI(TAG, "Event manager initialized");
    return ESP_OK;
}

esp_err_t event_manager_deinit(void)
{
    if (!event_mgr.initialized) {
        return ESP_OK;
    }

    // Stop event task
    if (event_mgr.event_task) {
        vTaskDelete(event_mgr.event_task);
        event_mgr.event_task = NULL;
    }

    // Delete queue
    if (event_mgr.event_queue) {
        vQueueDelete(event_mgr.event_queue);
        event_mgr.event_queue = NULL;
    }

    // Free all handlers
    xSemaphoreTake(event_mgr.mutex, portMAX_DELAY);
    for (int i = 0; i < EVENT_TYPE_MAX; i++) {
        event_handler_t *handler = event_mgr.handlers[i];
        while (handler) {
            event_handler_t *next = handler->next;
            free(handler);
            handler = next;
        }
        event_mgr.handlers[i] = NULL;
        event_mgr.subscriber_counts[i] = 0;
    }
    xSemaphoreGive(event_mgr.mutex);

    // Delete mutex
    if (event_mgr.mutex) {
        vSemaphoreDelete(event_mgr.mutex);
        event_mgr.mutex = NULL;
    }

    event_mgr.initialized = false;
    ESP_LOGI(TAG, "Event manager deinitialized");
    return ESP_OK;
}

esp_err_t event_subscribe(event_type_t type,
                          event_callback_t callback,
                          void *user_data,
                          event_handler_t **out_handler)
{
    if (!event_mgr.initialized || !callback || type >= EVENT_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    // Allocate handler
    event_handler_t *handler = (event_handler_t *)malloc(sizeof(event_handler_t));
    if (!handler) {
        ESP_LOGE(TAG, "Failed to allocate handler");
        return ESP_ERR_NO_MEM;
    }

    handler->type = type;
    handler->callback = callback;
    handler->user_data = user_data;
    handler->next = NULL;

    // Add to handler list
    xSemaphoreTake(event_mgr.mutex, portMAX_DELAY);

    if (event_mgr.handlers[type] == NULL) {
        event_mgr.handlers[type] = handler;
    } else {
        event_handler_t *current = event_mgr.handlers[type];
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = handler;
    }

    event_mgr.subscriber_counts[type]++;
    xSemaphoreGive(event_mgr.mutex);

    if (out_handler) {
        *out_handler = handler;
    }

    ESP_LOGD(TAG, "Subscribed to event %s (count: %d)",
             event_get_type_name(type), event_mgr.subscriber_counts[type]);

    return ESP_OK;
}

esp_err_t event_unsubscribe(event_handler_t *handler)
{
    if (!event_mgr.initialized || !handler) {
        return ESP_ERR_INVALID_ARG;
    }

    event_type_t type = handler->type;
    if (type >= EVENT_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(event_mgr.mutex, portMAX_DELAY);

    event_handler_t *current = event_mgr.handlers[type];
    event_handler_t *prev = NULL;

    while (current) {
        if (current == handler) {
            if (prev) {
                prev->next = current->next;
            } else {
                event_mgr.handlers[type] = current->next;
            }
            free(current);
            event_mgr.subscriber_counts[type]--;
            xSemaphoreGive(event_mgr.mutex);

            ESP_LOGD(TAG, "Unsubscribed from event %s (count: %d)",
                     event_get_type_name(type), event_mgr.subscriber_counts[type]);
            return ESP_OK;
        }
        prev = current;
        current = current->next;
    }

    xSemaphoreGive(event_mgr.mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t event_publish(event_type_t type, const void *data, size_t data_len)
{
    if (!event_mgr.initialized || type >= EVENT_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    event_data_t event = {
        .type = type,
        .data = (void *)data,
        .data_len = data_len,
        .timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS
    };

    if (event_mgr.logging_enabled) {
        ESP_LOGI(TAG, "Event published: %s (data_len=%d)",
                 event_get_type_name(type), data_len);
    }

    invoke_callbacks(&event);
    return ESP_OK;
}

esp_err_t event_post(event_type_t type, const void *data, size_t data_len, uint32_t timeout_ms)
{
    if (!event_mgr.initialized || type >= EVENT_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    if (data_len > sizeof(((event_queue_item_t *)0)->data_buffer)) {
        ESP_LOGE(TAG, "Event data too large: %d bytes", data_len);
        return ESP_ERR_INVALID_SIZE;
    }

    event_queue_item_t item;
    item.event.type = type;
    item.event.data_len = data_len;
    item.event.timestamp = xTaskGetTickCount() * portTICK_PERIOD_MS;

    if (data && data_len > 0) {
        memcpy(item.data_buffer, data, data_len);
        item.event.data = item.data_buffer;
    } else {
        item.event.data = NULL;
    }

    TickType_t ticks = (timeout_ms == 0) ? 0 : pdMS_TO_TICKS(timeout_ms);
    if (xQueueSend(event_mgr.event_queue, &item, ticks) != pdTRUE) {
        event_mgr.dropped_events++;
        ESP_LOGW(TAG, "Event queue full, dropped event: %s", event_get_type_name(type));
        return ESP_ERR_TIMEOUT;
    }

    if (event_mgr.logging_enabled) {
        ESP_LOGI(TAG, "Event posted: %s (data_len=%d)",
                 event_get_type_name(type), data_len);
    }

    return ESP_OK;
}

const char *event_get_type_name(event_type_t type)
{
    if (type >= EVENT_USER_DEFINED_START) {
        return "USER_DEFINED";
    }

    if (type < sizeof(event_type_names) / sizeof(event_type_names[0])
        && event_type_names[type] != NULL) {
        return event_type_names[type];
    }

    return "UNKNOWN";
}

int event_get_subscriber_count(event_type_t type)
{
    if (type >= EVENT_TYPE_MAX) {
        return 0;
    }
    return event_mgr.subscriber_counts[type];
}

void event_set_logging(bool enable)
{
    event_mgr.logging_enabled = enable;
    ESP_LOGI(TAG, "Event logging %s", enable ? "enabled" : "disabled");
}

esp_err_t event_get_stats(int *queue_size, int *queue_capacity, int *dropped_events)
{
    if (!event_mgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (queue_size) {
        *queue_size = uxQueueMessagesWaiting(event_mgr.event_queue);
    }
    if (queue_capacity) {
        *queue_capacity = EVENT_QUEUE_SIZE;
    }
    if (dropped_events) {
        *dropped_events = event_mgr.dropped_events;
    }

    return ESP_OK;
}

static void invoke_callbacks(const event_data_t *event)
{
    xSemaphoreTake(event_mgr.mutex, portMAX_DELAY);

    event_handler_t *handler = event_mgr.handlers[event->type];
    while (handler) {
        if (handler->callback) {
            handler->callback(event, handler->user_data);
        }
        handler = handler->next;
    }

    xSemaphoreGive(event_mgr.mutex);
}

static void event_task(void *pvParameters)
{
    event_queue_item_t item;

    ESP_LOGI(TAG, "Event task started");

    while (1) {
        if (xQueueReceive(event_mgr.event_queue, &item, portMAX_DELAY) == pdTRUE) {
            invoke_callbacks(&item.event);
        }
    }
}
