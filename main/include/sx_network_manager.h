#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"
#include "sx_network_config.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SX_NETWORK_IF_NONE = 0,
    SX_NETWORK_IF_WIFI,
    SX_NETWORK_IF_W5500,
    SX_NETWORK_IF_4G,
} sx_network_interface_t;

typedef struct {
    sx_network_interface_t active_interface;
    bool wifi_sta_started;
    bool wifi_connected;
    bool wifi_got_ip;
    bool wifi_ap_started;
    uint8_t wifi_ap_client_count;
    bool w5500_started;
    bool w5500_link_up;
    bool w5500_got_ip;
    bool modem_started;
    bool modem_usb_connected;
    bool modem_got_ip;
    char wifi_ip[16];
    char ethernet_ip[16];
    char modem_ip[16];
    char ip[16];
    char gateway[16];
    char netmask[16];
    char active_dns[16];
    bool dns_ready;
    uint32_t uplink_generation;
    bool routing_enabled;
    bool napt_active;
    bool reboot_required;
} sx_network_status_t;

esp_err_t sx_network_manager_init(void);
esp_err_t sx_network_manager_start_management_network(void);
esp_err_t sx_network_manager_start_remaining_networks(void);
esp_err_t sx_network_manager_start_configured(void);
void sx_network_manager_mark_application_ready(bool startup_ok);
void sx_network_manager_get_status(sx_network_status_t *status);
void sx_network_manager_get_config(sx_network_config_t *config);
esp_err_t sx_network_manager_save_config(const sx_network_config_t *config);
const char *sx_network_interface_name(sx_network_interface_t interface_id);

void sx_network_manager_wifi_connected(void);
void sx_network_manager_wifi_disconnected(void);
void sx_network_manager_wifi_got_ip(const esp_netif_ip_info_t *ip_info);
void sx_network_manager_wifi_lost_ip(void);
void sx_network_manager_wifi_ap_client_joined(void);
void sx_network_manager_wifi_ap_client_left(void);
void sx_network_manager_w5500_link_up(void);
void sx_network_manager_w5500_link_down(void);
void sx_network_manager_w5500_got_ip(const esp_netif_ip_info_t *ip_info);
void sx_network_manager_w5500_lost_ip(void);
void sx_network_manager_modem_connected(void);
void sx_network_manager_modem_disconnected(void);
void sx_network_manager_modem_net_disconnected(void);
void sx_network_manager_modem_got_ip(const esp_netif_ip_info_t *ip_info);

#ifdef __cplusplus
}
#endif
