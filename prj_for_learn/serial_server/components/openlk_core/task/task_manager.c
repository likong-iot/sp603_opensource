/**
 * @file task_manager.c
 * @brief Task Manager Implementation
 */

#include "task_manager.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "task_mgr";

#define MAX_REGISTERED_TASKS 32
#define MONITOR_TASK_STACK_SIZE 3072
#define MONITOR_TASK_PRIORITY 3

/**
 * @brief Registered task entry
 */
typedef struct {
    TaskHandle_t handle;
    char name[configMAX_TASK_NAME_LEN];
    task_health_check_t health_check;
    void *user_data;
    bool in_use;
} registered_task_t;

// Task manager state
static struct {
    bool initialized;
    SemaphoreHandle_t mutex;
    registered_task_t tasks[MAX_REGISTERED_TASKS];
    int task_count;

    // Monitoring
    bool monitoring_enabled;
    TaskHandle_t monitor_task;
    uint32_t monitor_interval_ms;
} task_mgr = {0};

static void monitor_task_func(void *pvParameters);

const char *task_get_state_name(eTaskState state)
{
    switch (state) {
        case eRunning:   return "Running";
        case eReady:     return "Ready";
        case eBlocked:   return "Blocked";
        case eSuspended: return "Suspended";
        case eDeleted:   return "Deleted";
        default:         return "Unknown";
    }
}

esp_err_t task_manager_init(void)
{
    if (task_mgr.initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    // Create mutex
    task_mgr.mutex = xSemaphoreCreateMutex();
    if (!task_mgr.mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    // Initialize task array
    memset(task_mgr.tasks, 0, sizeof(task_mgr.tasks));
    task_mgr.task_count = 0;
    task_mgr.monitoring_enabled = false;
    task_mgr.monitor_task = NULL;

    task_mgr.initialized = true;
    ESP_LOGI(TAG, "Task manager initialized");
    return ESP_OK;
}

esp_err_t task_manager_deinit(void)
{
    if (!task_mgr.initialized) {
        return ESP_OK;
    }

    // Stop monitoring if active
    task_stop_monitoring();

    // Clear registered tasks
    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);
    memset(task_mgr.tasks, 0, sizeof(task_mgr.tasks));
    task_mgr.task_count = 0;
    xSemaphoreGive(task_mgr.mutex);

    // Delete mutex
    if (task_mgr.mutex) {
        vSemaphoreDelete(task_mgr.mutex);
        task_mgr.mutex = NULL;
    }

    task_mgr.initialized = false;
    ESP_LOGI(TAG, "Task manager deinitialized");
    return ESP_OK;
}

esp_err_t task_register(TaskHandle_t handle,
                        const char *name,
                        task_health_check_t health_check,
                        void *user_data)
{
    if (!task_mgr.initialized || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);

    // Check if already registered
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (task_mgr.tasks[i].in_use && task_mgr.tasks[i].handle == handle) {
            xSemaphoreGive(task_mgr.mutex);
            ESP_LOGW(TAG, "Task already registered");
            return ESP_ERR_INVALID_STATE;
        }
    }

    // Find free slot
    int slot = -1;
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (!task_mgr.tasks[i].in_use) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        xSemaphoreGive(task_mgr.mutex);
        ESP_LOGE(TAG, "No free slots for task registration");
        return ESP_ERR_NO_MEM;
    }

    // Register task
    registered_task_t *task = &task_mgr.tasks[slot];
    task->handle = handle;
    task->health_check = health_check;
    task->user_data = user_data;
    task->in_use = true;

    if (name) {
        strncpy(task->name, name, configMAX_TASK_NAME_LEN - 1);
        task->name[configMAX_TASK_NAME_LEN - 1] = '\0';
    } else {
        const char *task_name = pcTaskGetName(handle);
        strncpy(task->name, task_name, configMAX_TASK_NAME_LEN - 1);
        task->name[configMAX_TASK_NAME_LEN - 1] = '\0';
    }

    task_mgr.task_count++;
    xSemaphoreGive(task_mgr.mutex);

    ESP_LOGI(TAG, "Task registered: %s (count: %d)", task->name, task_mgr.task_count);
    return ESP_OK;
}

esp_err_t task_unregister(TaskHandle_t handle)
{
    if (!task_mgr.initialized || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);

    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (task_mgr.tasks[i].in_use && task_mgr.tasks[i].handle == handle) {
            ESP_LOGI(TAG, "Task unregistered: %s", task_mgr.tasks[i].name);
            memset(&task_mgr.tasks[i], 0, sizeof(registered_task_t));
            task_mgr.task_count--;
            xSemaphoreGive(task_mgr.mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(task_mgr.mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t task_get_info(TaskHandle_t handle, task_info_t *info)
{
    if (!task_mgr.initialized || !handle || !info) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);

    // Find registered task
    const char *task_name = NULL;
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (task_mgr.tasks[i].in_use && task_mgr.tasks[i].handle == handle) {
            task_name = task_mgr.tasks[i].name;
            break;
        }
    }

    if (!task_name) {
        task_name = pcTaskGetName(handle);
    }

    // Get task information
    info->name = task_name;
    info->handle = handle;
    info->priority = uxTaskPriorityGet(handle);
    info->state = eTaskGetState(handle);

    // Get stack high water mark (minimum free stack)
    info->stack_free = uxTaskGetStackHighWaterMark(handle);
    info->stack_size = 0; // Not directly available from FreeRTOS

    // Runtime stats (if enabled)
#if configGENERATE_RUN_TIME_STATS
    TaskStatus_t task_status;
    vTaskGetInfo(handle, &task_status, pdTRUE, info->state);
    info->run_time = task_status.ulRunTimeCounter;
#else
    info->run_time = 0;
#endif

    info->cpu_usage = 0; // Calculated separately in monitoring

    xSemaphoreGive(task_mgr.mutex);
    return ESP_OK;
}

esp_err_t task_get_all_info(task_info_t *tasks, int max_tasks, int *out_count)
{
    if (!task_mgr.initialized || !tasks || !out_count || max_tasks <= 0) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);

    int count = 0;
    for (int i = 0; i < MAX_REGISTERED_TASKS && count < max_tasks; i++) {
        if (task_mgr.tasks[i].in_use) {
            task_get_info(task_mgr.tasks[i].handle, &tasks[count]);
            count++;
        }
    }

    *out_count = count;
    xSemaphoreGive(task_mgr.mutex);

    return ESP_OK;
}

esp_err_t task_get_stats(task_stats_t *stats)
{
    if (!task_mgr.initialized || !stats) {
        return ESP_ERR_INVALID_ARG;
    }

    stats->total_tasks = task_mgr.task_count;

    // Count running tasks
    int running = 0;
    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (task_mgr.tasks[i].in_use) {
            eTaskState state = eTaskGetState(task_mgr.tasks[i].handle);
            if (state == eRunning || state == eReady) {
                running++;
            }
        }
    }
    xSemaphoreGive(task_mgr.mutex);

    stats->running_tasks = running;

    // Heap statistics
    stats->total_heap = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    stats->free_heap = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    stats->min_free_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);

    // CPU frequency
    stats->cpu_freq_mhz = esp_clk_cpu_freq() / 1000000;

    return ESP_OK;
}

esp_err_t task_check_health(int *unhealthy_count)
{
    if (!task_mgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    int unhealthy = 0;

    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (task_mgr.tasks[i].in_use && task_mgr.tasks[i].health_check) {
            bool healthy = task_mgr.tasks[i].health_check(task_mgr.tasks[i].user_data);
            if (!healthy) {
                unhealthy++;
                ESP_LOGW(TAG, "Task unhealthy: %s", task_mgr.tasks[i].name);
            }
        }
    }
    xSemaphoreGive(task_mgr.mutex);

    if (unhealthy_count) {
        *unhealthy_count = unhealthy;
    }

    return (unhealthy == 0) ? ESP_OK : ESP_FAIL;
}

void task_print_list(void)
{
    if (!task_mgr.initialized) {
        ESP_LOGW(TAG, "Task manager not initialized");
        return;
    }

    ESP_LOGI(TAG, "=== Task List ===");
    ESP_LOGI(TAG, "%-20s %-10s %-10s %-10s", "Name", "State", "Priority", "Stack Free");

    xSemaphoreTake(task_mgr.mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_REGISTERED_TASKS; i++) {
        if (task_mgr.tasks[i].in_use) {
            task_info_t info;
            task_get_info(task_mgr.tasks[i].handle, &info);

            ESP_LOGI(TAG, "%-20s %-10s %-10u %-10u",
                     info.name,
                     task_get_state_name(info.state),
                     (unsigned)info.priority,
                     (unsigned)info.stack_free);
        }
    }
    xSemaphoreGive(task_mgr.mutex);

    ESP_LOGI(TAG, "=================");
}

void task_print_stats(void)
{
    if (!task_mgr.initialized) {
        ESP_LOGW(TAG, "Task manager not initialized");
        return;
    }

    task_stats_t stats;
    task_get_stats(&stats);

    ESP_LOGI(TAG, "=== System Statistics ===");
    ESP_LOGI(TAG, "Total Tasks:      %d", stats.total_tasks);
    ESP_LOGI(TAG, "Running Tasks:    %d", stats.running_tasks);
    ESP_LOGI(TAG, "Total Heap:       %u bytes", (unsigned)stats.total_heap);
    ESP_LOGI(TAG, "Free Heap:        %u bytes", (unsigned)stats.free_heap);
    ESP_LOGI(TAG, "Min Free Heap:    %u bytes", (unsigned)stats.min_free_heap);
    ESP_LOGI(TAG, "CPU Frequency:    %u MHz", (unsigned)stats.cpu_freq_mhz);
    ESP_LOGI(TAG, "========================");
}

esp_err_t task_start_monitoring(uint32_t interval_ms)
{
    if (!task_mgr.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (task_mgr.monitoring_enabled) {
        ESP_LOGW(TAG, "Monitoring already started");
        return ESP_ERR_INVALID_STATE;
    }

    task_mgr.monitor_interval_ms = interval_ms;

    BaseType_t ret = xTaskCreate(monitor_task_func, "task_monitor",
                                  MONITOR_TASK_STACK_SIZE, NULL,
                                  MONITOR_TASK_PRIORITY, &task_mgr.monitor_task);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create monitor task");
        return ESP_FAIL;
    }

    task_mgr.monitoring_enabled = true;
    ESP_LOGI(TAG, "Task monitoring started (interval: %u ms)", (unsigned)interval_ms);
    return ESP_OK;
}

esp_err_t task_stop_monitoring(void)
{
    if (!task_mgr.monitoring_enabled) {
        return ESP_OK;
    }

    task_mgr.monitoring_enabled = false;

    if (task_mgr.monitor_task) {
        vTaskDelete(task_mgr.monitor_task);
        task_mgr.monitor_task = NULL;
    }

    ESP_LOGI(TAG, "Task monitoring stopped");
    return ESP_OK;
}

static void monitor_task_func(void *pvParameters)
{
    ESP_LOGI(TAG, "Monitor task started");

    while (task_mgr.monitoring_enabled) {
        // Print task list
        task_print_list();

        // Print system stats
        task_print_stats();

        // Check health
        int unhealthy;
        if (task_check_health(&unhealthy) != ESP_OK) {
            ESP_LOGW(TAG, "Health check failed: %d unhealthy tasks", unhealthy);
        }

        vTaskDelay(pdMS_TO_TICKS(task_mgr.monitor_interval_ms));
    }

    ESP_LOGI(TAG, "Monitor task stopped");
    vTaskDelete(NULL);
}
