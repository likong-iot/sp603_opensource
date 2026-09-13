#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t create_app_task_psram(TaskFunction_t task_fn,
                                 const char *name,
                                 uint32_t stack_size_bytes,
                                 void *params,
                                 UBaseType_t priority,
                                 TaskHandle_t *handle,
                                 BaseType_t core_id);

void delete_app_task_with_caps(TaskHandle_t handle);
void delete_self_app_task_with_caps(void);

#ifdef __cplusplus
}
#endif

