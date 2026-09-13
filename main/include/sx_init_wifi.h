#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "esp_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_init_sta(const char *w_ssid, const char *w_passwd);
void start_wifi_task(const char *ssid, const char *password);
esp_err_t sx_wifi_init_softap_from_nvs(void);
esp_err_t sx_wifi_start_configured(bool ap_enabled,
                                   bool sta_enabled,
                                   const char *sta_ssid,
                                   const char *sta_password,
                                   bool sta_static,
                                   const char *sta_ip,
                                   const char *sta_netmask,
                                   const char *sta_gateway,
                                   const char *sta_dns,
                                   const char *ap_ip,
                                   const char *ap_netmask,
                                   bool ap_dhcp_enabled);
esp_netif_t *sx_wifi_get_ap_netif(void);
esp_netif_t *sx_wifi_get_sta_netif(void);
esp_err_t sx_wifi_scan_access_points(wifi_ap_record_t *records,
                                     uint16_t *record_count);

#ifdef __cplusplus
}
#endif
