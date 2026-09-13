/**
 * @file openlk.h
 * @brief OpenLK Serial Server SDK - Unified Header
 *
 * This is the main header file for the OpenLK Serial Server SDK.
 * Include this file to access all SDK components:
 * - Hardware Abstraction Layer (HAL)
 * - Protocol Layer
 * - Core Services
 */

#ifndef OPENLK_H
#define OPENLK_H

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================
 * Hardware Abstraction Layer (HAL)
 * ============================================ */

// UART HAL
#include "openlk_hal/uart/uart_hal.h"

// GPIO HAL
#include "openlk_hal/gpio/gpio_hal.h"

// Ethernet HAL (if available)
// #include "openlk_hal/ethernet/ethernet_hal.h"

// WiFi HAL (if available)
// #include "openlk_hal/wifi/wifi_hal.h"


/* ============================================
 * Protocol Layer
 * ============================================ */

// TCP Server
#include "openlk_protocol/tcp/tcp_server.h"

// MQTT Client
#include "openlk_protocol/mqtt/mqtt_client.h"

// UDP (if available)
// #include "openlk_protocol/udp/udp_server.h"

// HTTP (if available)
// #include "openlk_protocol/http/http_server.h"


/* ============================================
 * Core Services
 * ============================================ */

// Configuration Manager
#include "openlk_core/config/config_manager.h"

// Event Manager
#include "openlk_core/event/event_manager.h"

// Task Manager
#include "openlk_core/task/task_manager.h"


/* ============================================
 * SDK Version Information
 * ============================================ */

#define OPENLK_VERSION_MAJOR 1
#define OPENLK_VERSION_MINOR 0
#define OPENLK_VERSION_PATCH 0

#define OPENLK_VERSION_STRING "1.0.0"


/* ============================================
 * SDK Initialization
 * ============================================ */

/**
 * @brief Initialize all OpenLK SDK components
 *
 * This function initializes:
 * - Configuration Manager
 * - Event Manager
 * - Task Manager
 *
 * Call this function before using any SDK features.
 *
 * @return ESP_OK on success, error code otherwise
 */
static inline esp_err_t openlk_init(void)
{
    esp_err_t ret;

    // Initialize Config Manager
    ret = config_manager_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // Initialize Event Manager
    ret = event_manager_init();
    if (ret != ESP_OK) {
        config_manager_deinit();
        return ret;
    }

    // Initialize Task Manager
    ret = task_manager_init();
    if (ret != ESP_OK) {
        event_manager_deinit();
        config_manager_deinit();
        return ret;
    }

    return ESP_OK;
}

/**
 * @brief Deinitialize all OpenLK SDK components
 *
 * @return ESP_OK on success, error code otherwise
 */
static inline esp_err_t openlk_deinit(void)
{
    task_manager_deinit();
    event_manager_deinit();
    config_manager_deinit();
    return ESP_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* OPENLK_H */
