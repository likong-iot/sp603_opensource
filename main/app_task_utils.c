#include "app_task_utils.h"

#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

BaseType_t create_app_task_psram(TaskFunction_t task_fn,
                                 const char *name,
                                 uint32_t stack_size_bytes,
                                 void *params,
                                 UBaseType_t priority,
                                 TaskHandle_t *handle,
                                 BaseType_t core_id)
{
    const UBaseType_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    return xTaskCreatePinnedToCoreWithCaps(task_fn,
                                           name,
                                           stack_size_bytes,
                                           params,
                                           priority,
                                           handle,
                                           core_id,
                                           caps);
}

void delete_app_task_with_caps(TaskHandle_t handle)
{
    vTaskDeleteWithCaps(handle);
}

void delete_self_app_task_with_caps(void)
{
    vTaskDeleteWithCaps(NULL);
}

