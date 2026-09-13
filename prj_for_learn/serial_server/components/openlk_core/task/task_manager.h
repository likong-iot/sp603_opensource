/**
 * @file task_manager.h
 * @brief Task Manager Interface
 *
 * Provides task registration, monitoring, and resource tracking capabilities.
 * Helps manage FreeRTOS tasks and monitor system health.
 */

#ifndef OPENLK_TASK_MANAGER_H
#define OPENLK_TASK_MANAGER_H

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Task information structure
 */
typedef struct {
    const char *name;           // Task name
    TaskHandle_t handle;        // FreeRTOS task handle
    UBaseType_t priority;       // Task priority
    uint32_t stack_size;        // Stack size in bytes
    uint32_t stack_free;        // Free stack space (high water mark)
    uint32_t cpu_usage;         // CPU usage percentage (0-100)
    uint64_t run_time;          // Total runtime in microseconds
    eTaskState state;           // Task state
} task_info_t;

/**
 * @brief Task statistics structure
 */
typedef struct {
    int total_tasks;            // Total registered tasks
    int running_tasks;          // Currently running tasks
    uint32_t total_heap;        // Total heap size
    uint32_t free_heap;         // Free heap size
    uint32_t min_free_heap;     // Minimum free heap ever
    uint32_t cpu_freq_mhz;      // CPU frequency in MHz
} task_stats_t;

/**
 * @brief Task health check callback
 *
 * @param user_data User-provided context data
 * @return true if task is healthy, false otherwise
 */
typedef bool (*task_health_check_t)(void *user_data);

/**
 * @brief Initialize task manager
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_manager_init(void);

/**
 * @brief Deinitialize task manager
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_manager_deinit(void);

/**
 * @brief Register a task with the task manager
 *
 * @param handle Task handle
 * @param name Task name (optional, will use FreeRTOS name if NULL)
 * @param health_check Health check callback (optional)
 * @param user_data User data for health check callback
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_register(TaskHandle_t handle,
                        const char *name,
                        task_health_check_t health_check,
                        void *user_data);

/**
 * @brief Unregister a task
 *
 * @param handle Task handle
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_unregister(TaskHandle_t handle);

/**
 * @brief Get information about a specific task
 *
 * @param handle Task handle
 * @param info Output task information structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_get_info(TaskHandle_t handle, task_info_t *info);

/**
 * @brief Get information about all registered tasks
 *
 * @param tasks Array to store task information
 * @param max_tasks Maximum number of tasks to retrieve
 * @param out_count Actual number of tasks retrieved
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_get_all_info(task_info_t *tasks, int max_tasks, int *out_count);

/**
 * @brief Get system statistics
 *
 * @param stats Output statistics structure
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_get_stats(task_stats_t *stats);

/**
 * @brief Run health checks on all registered tasks
 *
 * @param unhealthy_count Output number of unhealthy tasks
 * @return ESP_OK if all tasks are healthy, ESP_FAIL otherwise
 */
esp_err_t task_check_health(int *unhealthy_count);

/**
 * @brief Print task list to console
 *
 * Displays all registered tasks with their status, stack usage, and CPU time.
 */
void task_print_list(void);

/**
 * @brief Print system statistics to console
 */
void task_print_stats(void);

/**
 * @brief Start task monitoring (periodic statistics collection)
 *
 * @param interval_ms Monitoring interval in milliseconds
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_start_monitoring(uint32_t interval_ms);

/**
 * @brief Stop task monitoring
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t task_stop_monitoring(void);

/**
 * @brief Get task state name
 *
 * @param state Task state
 * @return State name string
 */
const char *task_get_state_name(eTaskState state);

#ifdef __cplusplus
}
#endif

#endif // OPENLK_TASK_MANAGER_H
