/*
 * @Author: Orion
 * @Date: 2024-07-16 10:00:00
 * @LastEditors: Orion
 * @LastEditTime: 2024-07-16 10:00:00
 * @FilePath: \SERIIAL_SERVER\main\include\sx_udp_multicast.h
 * @Description: UDP组播功能头文件，用于跨网段设备搜索
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "cJSON.h"
#include "sx_utils.h"

/**
 * @brief 启动UDP组播服务
 *
 * 该函数创建一个任务来处理UDP组播通信，用于设备搜索和配置
 */
void start_udp_multicast(void);

/**
 * @brief 停止UDP组播服务
 *
 * 该函数删除UDP组播任务
 */
void kill_udp_multicast(void);

/**
 * @brief 获取设备名称
 *
 * @return char* 设备名称
 */
char *get_device_name(void);

#ifdef __cplusplus
}
#endif
