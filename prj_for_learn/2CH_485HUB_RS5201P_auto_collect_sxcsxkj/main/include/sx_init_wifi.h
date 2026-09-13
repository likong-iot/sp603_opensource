/*
 * @Author: Orion
 * @Date: 2024-07-16 14:16:52
 * @LastEditors: Orion
 * @LastEditTime: 2024-08-23 17:25:56
 * @Description:
 *
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void wifi_init_sta(const char *w_ssid, const char *w_passwd);
void start_wifi_task(const char *ssid, const char *password);
// 暂停WiFi重连，避免在AP模式下Web服务器卡顿
void pause_wifi_reconnect(void);
// 恢复WiFi重连
void resume_wifi_reconnect(void);
// 检测WiFi扫描是否被允许
bool is_wifi_scan_allowed(void);
extern int is_netif_initialized;
#ifdef __cplusplus
}
#endif