#pragma once

#include <esp_err.h>
#include <esp_eth.h>
#include <esp_netif.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t w5500_manager_init(void);
esp_err_t w5500_manager_init_with_role(bool lan_mode,
                                       const char *lan_ip,
                                       const char *lan_netmask,
                                       bool static_enabled,
                                       const char *gateway,
                                       const char *dns,
                                       bool dhcp_server_enabled);
esp_eth_handle_t w5500_manager_get_handle(void);
esp_netif_t *w5500_manager_get_netif(void);
bool w5500_manager_is_connected(void);
esp_err_t w5500_manager_get_dns_info(esp_netif_dns_info_t *dns);

#ifdef __cplusplus
}
#endif
