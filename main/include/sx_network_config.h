#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX_NETWORK_TEXT_LEN 64
#define SX_NETWORK_IP_LEN 16

typedef enum {
    SX_NETWORK_ROLE_OFF = 0,
    SX_NETWORK_ROLE_UPLINK,
    SX_NETWORK_ROLE_BACKUP,
    SX_NETWORK_ROLE_DOWNLINK,
    SX_NETWORK_ROLE_LOCAL,
    SX_NETWORK_ROLE_LAST, /* Append to preserve stored role values. */
} sx_network_role_t;

typedef enum {
    SX_NAT_DOWNLINK_WIFI_AP = 0,
    SX_NAT_DOWNLINK_ETHERNET,
} sx_nat_downlink_t;

typedef struct {
    bool ethernet_enabled;
    sx_network_role_t ethernet_role;
    char ethernet_lan_ip[SX_NETWORK_IP_LEN];
    char ethernet_lan_netmask[SX_NETWORK_IP_LEN];
    bool ethernet_dhcp_enabled;
    bool ethernet_static;
    char ethernet_gateway[SX_NETWORK_IP_LEN];
    char ethernet_dns[SX_NETWORK_IP_LEN];

    bool wifi_sta_enabled;
    sx_network_role_t wifi_sta_role;
    char wifi_ssid[SX_NETWORK_TEXT_LEN];
    char wifi_password[SX_NETWORK_TEXT_LEN];
    bool wifi_sta_static;
    char wifi_sta_ip[SX_NETWORK_IP_LEN];
    char wifi_sta_netmask[SX_NETWORK_IP_LEN];
    char wifi_sta_gateway[SX_NETWORK_IP_LEN];
    char wifi_sta_dns[SX_NETWORK_IP_LEN];

    bool wifi_ap_enabled;
    sx_network_role_t wifi_ap_role;
    char ap_ssid[SX_NETWORK_TEXT_LEN];
    char ap_password[SX_NETWORK_TEXT_LEN];
    char ap_ip[SX_NETWORK_IP_LEN];
    char ap_netmask[SX_NETWORK_IP_LEN];
    uint16_t ap_timeout_minutes;
    bool wifi_ap_dhcp_enabled;

    bool modem_enabled;
    sx_network_role_t modem_role;

    bool routing_enabled;
    sx_nat_downlink_t nat_downlink;
} sx_network_config_t;

void sx_network_config_normalize(sx_network_config_t *config);
void sx_network_config_set_defaults(sx_network_config_t *config);
esp_err_t sx_network_config_load(sx_network_config_t *config);
esp_err_t sx_network_config_save(const sx_network_config_t *config);
esp_err_t sx_network_config_validate(const sx_network_config_t *config,
                                     char *reason,
                                     size_t reason_size);
const char *sx_network_role_name(sx_network_role_t role);
bool sx_network_role_from_name(const char *name, sx_network_role_t *role);
const char *sx_nat_downlink_name(sx_nat_downlink_t downlink);
bool sx_nat_downlink_from_name(const char *name, sx_nat_downlink_t *downlink);

#ifdef __cplusplus
}
#endif
