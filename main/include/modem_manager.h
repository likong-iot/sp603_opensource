#pragma once

#include <stdbool.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 4G 调制解调器初始化任务
 */
esp_err_t modem_manager_init(void);

/**
 * @brief 查询 4G 是否已成功拨号并获取 IP
 */
bool modem_manager_is_connected(void);

#ifdef __cplusplus
}
#endif
