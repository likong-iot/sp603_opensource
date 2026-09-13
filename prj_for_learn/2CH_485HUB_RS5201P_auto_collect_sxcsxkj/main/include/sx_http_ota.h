/*
 * @Author: Orion
 * @Date: 2024-01-11 11:20:05
 * @LastEditors: Orion
 * @LastEditTime: 2024-05-07 11:28:08
 * @FilePath: \SERIIAL_SERVER\main\include\sx_http_ota.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif
void simple_ota_task(char *updateUrl);
void get_sha256_of_partitions(void);
int get_ota_progress(void);
const char* get_ota_status(void);

#ifdef __cplusplus
}
#endif
