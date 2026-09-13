#include "sx_network_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "nvs.h"

#define NVS_NAMESPACE "storage"

static const char *TAG = "network_cfg";

static void copy_text(char *dst, size_t size, const char *src)
{
    if (dst == NULL || size == 0) return;
    snprintf(dst, size, "%s", src != NULL ? src : "");
}

static void get_string(nvs_handle_t nvs, const char *key, char *out, size_t size)
{
    size_t len = size;
    esp_err_t err = nvs_get_str(nvs, key, out, &len);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "read %s failed: %s", key, esp_err_to_name(err));
    }
}

static void get_u8(nvs_handle_t nvs, const char *key, uint8_t *value)
{
    uint8_t loaded = 0;
    if (nvs_get_u8(nvs, key, &loaded) == ESP_OK) *value = loaded;
}

static void get_ap_timeout(nvs_handle_t nvs, uint16_t *value)
{
    char text[12] = {0};
    size_t len = sizeof(text);
    if (nvs_get_str(nvs, "ap_timeout", text, &len) == ESP_OK) {
        long minutes = strtol(text, NULL, 10);
        if (minutes >= 0 && minutes <= 1440) *value = (uint16_t)minutes;
    }
}

void sx_network_config_normalize(sx_network_config_t *config)
{
    if (config == NULL) return;
    sx_network_role_t *roles[] = {&config->ethernet_role, &config->wifi_sta_role, &config->modem_role};
    for (int i = 0; i < 3; ++i) {
        if (*roles[i] == SX_NETWORK_ROLE_BACKUP || *roles[i] == SX_NETWORK_ROLE_LAST)
            *roles[i] = SX_NETWORK_ROLE_UPLINK;
    }
    /* The current NAPT design supports one downstream interface. */
    if (config->wifi_ap_enabled && config->wifi_ap_role == SX_NETWORK_ROLE_DOWNLINK &&
        config->ethernet_enabled && config->ethernet_role == SX_NETWORK_ROLE_DOWNLINK) {
        config->ethernet_role = SX_NETWORK_ROLE_UPLINK;
    }
    /* DHCP servers only make sense on a downstream network. */
    if (config->ethernet_role != SX_NETWORK_ROLE_DOWNLINK) {
        config->ethernet_dhcp_enabled = false;
    }
    if (config->wifi_ap_role == SX_NETWORK_ROLE_LOCAL) {
        /* Management AP must be able to assign an address to the operator. */
        config->wifi_ap_dhcp_enabled = true;
    } else if (config->wifi_ap_role != SX_NETWORK_ROLE_DOWNLINK) {
        config->wifi_ap_dhcp_enabled = false;
    }
    if (!config->wifi_ap_enabled) config->wifi_ap_dhcp_enabled = false;
    if (!config->ethernet_enabled) config->ethernet_dhcp_enabled = false;
    if (config->wifi_ap_enabled && config->wifi_ap_role == SX_NETWORK_ROLE_DOWNLINK) { config->nat_downlink = SX_NAT_DOWNLINK_WIFI_AP; config->routing_enabled = true; }
    else if (config->ethernet_enabled && config->ethernet_role == SX_NETWORK_ROLE_DOWNLINK) { config->nat_downlink = SX_NAT_DOWNLINK_ETHERNET; config->routing_enabled = true; }
    else config->routing_enabled = false;
}

void sx_network_config_set_defaults(sx_network_config_t *config)
{
    if (config == NULL) return;
    memset(config, 0, sizeof(*config));

    config->ethernet_enabled = true;
    config->ethernet_role = SX_NETWORK_ROLE_UPLINK;
    config->ethernet_dhcp_enabled = false;
    config->ethernet_static = false;
    copy_text(config->ethernet_lan_ip, sizeof(config->ethernet_lan_ip), "192.168.5.1");
    copy_text(config->ethernet_lan_netmask, sizeof(config->ethernet_lan_netmask), "255.255.255.0");
    copy_text(config->ethernet_gateway, sizeof(config->ethernet_gateway), "192.168.5.254");
    copy_text(config->ethernet_dns, sizeof(config->ethernet_dns), "8.8.8.8");

    config->wifi_sta_enabled = false;
    config->wifi_sta_role = SX_NETWORK_ROLE_UPLINK;
    config->wifi_sta_static = false;
    copy_text(config->wifi_sta_ip, sizeof(config->wifi_sta_ip), "192.168.1.100");
    copy_text(config->wifi_sta_netmask, sizeof(config->wifi_sta_netmask), "255.255.255.0");
    copy_text(config->wifi_sta_gateway, sizeof(config->wifi_sta_gateway), "192.168.1.1");
    copy_text(config->wifi_sta_dns, sizeof(config->wifi_sta_dns), "8.8.8.8");

    config->wifi_ap_enabled = true;
    config->wifi_ap_role = SX_NETWORK_ROLE_DOWNLINK;
    config->ap_timeout_minutes = 30;
    config->wifi_ap_dhcp_enabled = true;
    copy_text(config->ap_ip, sizeof(config->ap_ip), "192.168.4.1");
    copy_text(config->ap_netmask, sizeof(config->ap_netmask), "255.255.255.0");

    config->modem_enabled = false;
    config->modem_role = SX_NETWORK_ROLE_UPLINK;

    config->routing_enabled = true;
    config->nat_downlink = SX_NAT_DOWNLINK_WIFI_AP;
}

esp_err_t sx_network_config_load(sx_network_config_t *config)
{
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    sx_network_config_set_defaults(config);

    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    ESP_RETURN_ON_ERROR(err, TAG, "open NVS failed");

    uint8_t value = config->ethernet_enabled;
    get_u8(nvs, "nm_eth_en", &value); config->ethernet_enabled = value != 0;
    value = (uint8_t)config->ethernet_role;
    get_u8(nvs, "nm_eth_role", &value); config->ethernet_role = (sx_network_role_t)value;
    get_string(nvs, "nm_eth_ip", config->ethernet_lan_ip, sizeof(config->ethernet_lan_ip));
    get_string(nvs, "nm_eth_mask", config->ethernet_lan_netmask, sizeof(config->ethernet_lan_netmask));
    value = config->ethernet_static; get_u8(nvs, "nm_eth_static", &value); config->ethernet_static = value != 0;
    get_string(nvs, "nm_eth_gw", config->ethernet_gateway, sizeof(config->ethernet_gateway));
    get_string(nvs, "nm_eth_dns", config->ethernet_dns, sizeof(config->ethernet_dns));
    value = config->ethernet_dhcp_enabled;
    get_u8(nvs, "nm_eth_dhcp", &value); config->ethernet_dhcp_enabled = value != 0;

    value = config->wifi_sta_enabled;
    get_u8(nvs, "nm_sta_en", &value); config->wifi_sta_enabled = value != 0;
    value = (uint8_t)config->wifi_sta_role;
    get_u8(nvs, "nm_sta_role", &value); config->wifi_sta_role = (sx_network_role_t)value;
    get_string(nvs, "wifi_ssid", config->wifi_ssid, sizeof(config->wifi_ssid));
    get_string(nvs, "wifi_password", config->wifi_password, sizeof(config->wifi_password));
    value = config->wifi_sta_static; get_u8(nvs, "nm_sta_static", &value); config->wifi_sta_static = value != 0;
    get_string(nvs, "nm_sta_ip", config->wifi_sta_ip, sizeof(config->wifi_sta_ip));
    get_string(nvs, "nm_sta_mask", config->wifi_sta_netmask, sizeof(config->wifi_sta_netmask));
    get_string(nvs, "nm_sta_gw", config->wifi_sta_gateway, sizeof(config->wifi_sta_gateway));
    get_string(nvs, "nm_sta_dns", config->wifi_sta_dns, sizeof(config->wifi_sta_dns));

    value = config->wifi_ap_enabled;
    get_u8(nvs, "nm_ap_en", &value); config->wifi_ap_enabled = value != 0;
    value = (uint8_t)config->wifi_ap_role;
    get_u8(nvs, "nm_ap_role", &value); config->wifi_ap_role = (sx_network_role_t)value;
    get_string(nvs, "ap_name", config->ap_ssid, sizeof(config->ap_ssid));
    get_string(nvs, "ap_password", config->ap_password, sizeof(config->ap_password));
    get_string(nvs, "nm_ap_ip", config->ap_ip, sizeof(config->ap_ip));
    get_string(nvs, "nm_ap_mask", config->ap_netmask, sizeof(config->ap_netmask));
    get_ap_timeout(nvs, &config->ap_timeout_minutes);
    value = config->wifi_ap_dhcp_enabled;
    get_u8(nvs, "nm_ap_dhcp", &value); config->wifi_ap_dhcp_enabled = value != 0;

    value = config->modem_enabled;
    get_u8(nvs, "nm_4g_en", &value); config->modem_enabled = value != 0;
    value = (uint8_t)config->modem_role;
    get_u8(nvs, "nm_4g_role", &value); config->modem_role = (sx_network_role_t)value;

    config->routing_enabled = false;
    config->nat_downlink = SX_NAT_DOWNLINK_WIFI_AP;
    nvs_close(nvs);

    sx_network_config_normalize(config);
    char reason[96];
    err = sx_network_config_validate(config, reason, sizeof(reason));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "invalid saved config (%s), use defaults", reason);
        sx_network_config_set_defaults(config);
    }
    ESP_LOGI(TAG,
             "loaded: ethernet=%s/%s wifi_sta=%s/%s wifi_ap=%s/%s modem=%s/%s route=%s downlink=%s",
             config->ethernet_enabled ? "on" : "off", sx_network_role_name(config->ethernet_role),
             config->wifi_sta_enabled ? "on" : "off", sx_network_role_name(config->wifi_sta_role),
             config->wifi_ap_enabled ? "on" : "off", sx_network_role_name(config->wifi_ap_role),
             config->modem_enabled ? "on" : "off", sx_network_role_name(config->modem_role),
             config->routing_enabled ? "on" : "off", sx_nat_downlink_name(config->nat_downlink));
    return ESP_OK;
}

esp_err_t sx_network_config_save(const sx_network_config_t *config)
{
    char reason[96];
    ESP_RETURN_ON_ERROR(sx_network_config_validate(config, reason, sizeof(reason)), TAG,
                        "invalid config: %s", reason);

    nvs_handle_t nvs = 0;
    ESP_LOGI(TAG, "[NETSAVE] NVS open begin");
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG, "open NVS failed");
    ESP_LOGI(TAG, "[NETSAVE] NVS open OK");
#define SET_U8(key, value) do { err = nvs_set_u8(nvs, key, (uint8_t)(value)); if (err != ESP_OK) { ESP_LOGE(TAG, "[NETSAVE] write %s failed: %s", key, esp_err_to_name(err)); goto done; } } while (0)
#define SET_STR(key, value) do { err = nvs_set_str(nvs, key, value); if (err != ESP_OK) { ESP_LOGE(TAG, "[NETSAVE] write %s failed: %s", key, esp_err_to_name(err)); goto done; } } while (0)
    esp_err_t err = ESP_OK;
    ESP_LOGI(TAG, "[NETSAVE] write Ethernet begin");
    SET_U8("nm_eth_en", config->ethernet_enabled);
    SET_U8("nm_eth_role", config->ethernet_role);
    SET_STR("nm_eth_ip", config->ethernet_lan_ip);
    SET_STR("nm_eth_mask", config->ethernet_lan_netmask);
    SET_U8("nm_eth_static", config->ethernet_static);
    SET_STR("nm_eth_gw", config->ethernet_gateway);
    SET_STR("nm_eth_dns", config->ethernet_dns);
    SET_U8("nm_eth_dhcp", config->ethernet_dhcp_enabled);
    ESP_LOGI(TAG, "[NETSAVE] write Ethernet OK; Wi-Fi STA begin");
    SET_U8("nm_sta_en", config->wifi_sta_enabled);
    SET_U8("nm_sta_role", config->wifi_sta_role);
    SET_STR("wifi_ssid", config->wifi_ssid);
    SET_STR("wifi_password", config->wifi_password);
    SET_U8("nm_sta_static", config->wifi_sta_static);
    SET_STR("nm_sta_ip", config->wifi_sta_ip);
    SET_STR("nm_sta_mask", config->wifi_sta_netmask);
    SET_STR("nm_sta_gw", config->wifi_sta_gateway);
    SET_STR("nm_sta_dns", config->wifi_sta_dns);
    ESP_LOGI(TAG, "[NETSAVE] write Wi-Fi STA OK; Wi-Fi AP begin");
    SET_U8("nm_ap_en", config->wifi_ap_enabled);
    SET_U8("nm_ap_role", config->wifi_ap_role);
    SET_STR("ap_name", config->ap_ssid);
    SET_STR("ap_password", config->ap_password);
    SET_STR("nm_ap_ip", config->ap_ip);
    SET_STR("nm_ap_mask", config->ap_netmask);
    char ap_timeout[12];
    snprintf(ap_timeout, sizeof(ap_timeout), "%u", config->ap_timeout_minutes);
    SET_STR("ap_timeout", ap_timeout);
    SET_U8("nm_ap_dhcp", config->wifi_ap_dhcp_enabled);
    ESP_LOGI(TAG, "[NETSAVE] write Wi-Fi AP OK; modem begin");
    SET_U8("nm_4g_en", config->modem_enabled);
    SET_U8("nm_4g_role", config->modem_role);
    ESP_LOGI(TAG, "[NETSAVE] write modem OK; commit begin");
    err = nvs_commit(nvs);
    ESP_LOGI(TAG, "[NETSAVE] commit result=%s", esp_err_to_name(err));
done:
    nvs_close(nvs);
    return err;
#undef SET_U8
#undef SET_STR
}

static bool valid_ip(const char *text)
{
    ip4_addr_t address;
    return text != NULL && ip4addr_aton(text, &address) != 0;
}

static esp_err_t fail(char *reason, size_t size, const char *text)
{
    if (reason != NULL && size > 0) snprintf(reason, size, "%s", text);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t sx_network_config_validate(const sx_network_config_t *config,
                                     char *reason,
                                     size_t reason_size)
{
    if (config == NULL) return fail(reason, reason_size, "missing config");
    if (config->ethernet_role < SX_NETWORK_ROLE_OFF || config->ethernet_role > SX_NETWORK_ROLE_LAST ||
        config->wifi_sta_role < SX_NETWORK_ROLE_OFF || config->wifi_sta_role > SX_NETWORK_ROLE_LAST ||
        config->wifi_ap_role < SX_NETWORK_ROLE_OFF || config->wifi_ap_role > SX_NETWORK_ROLE_LAST ||
        config->modem_role < SX_NETWORK_ROLE_OFF || config->modem_role > SX_NETWORK_ROLE_LAST) {
        return fail(reason, reason_size, "invalid interface role");
    }
    if (config->wifi_sta_enabled && config->wifi_ssid[0] == '\0')
        return fail(reason, reason_size, "Wi-Fi STA SSID is empty");
    if (config->ethernet_enabled && config->ethernet_role == SX_NETWORK_ROLE_OFF)
        return fail(reason, reason_size, "Ethernet is enabled but has no network purpose");
    if (config->ethernet_enabled && config->ethernet_role != SX_NETWORK_ROLE_UPLINK &&
        config->ethernet_role != SX_NETWORK_ROLE_BACKUP && config->ethernet_role != SX_NETWORK_ROLE_LAST &&
        config->ethernet_role != SX_NETWORK_ROLE_DOWNLINK)
        return fail(reason, reason_size, "Ethernet only supports uplink, backup, last or downlink");
    if (config->wifi_sta_enabled && config->wifi_sta_role == SX_NETWORK_ROLE_OFF)
        return fail(reason, reason_size, "Wi-Fi STA is enabled but has no network purpose");
    if (config->wifi_ap_enabled && config->wifi_ap_role == SX_NETWORK_ROLE_OFF)
        return fail(reason, reason_size, "Wi-Fi AP is enabled but has no network purpose");
    if (config->ethernet_dhcp_enabled && config->ethernet_role != SX_NETWORK_ROLE_DOWNLINK)
        return fail(reason, reason_size, "Ethernet DHCP server requires a downstream purpose");
    if (config->wifi_ap_dhcp_enabled && config->wifi_ap_role != SX_NETWORK_ROLE_DOWNLINK &&
        config->wifi_ap_role != SX_NETWORK_ROLE_LOCAL)
        return fail(reason, reason_size, "Wi-Fi AP DHCP server requires a local or downstream purpose");
    if (config->wifi_ap_enabled && config->ap_password[0] != '\0' && strlen(config->ap_password) < 8)
        return fail(reason, reason_size, "Wi-Fi AP password must have at least 8 characters");
    if (config->wifi_ap_enabled && (!valid_ip(config->ap_ip) || !valid_ip(config->ap_netmask)))
        return fail(reason, reason_size, "invalid Wi-Fi AP IPv4 settings");
    if (config->ap_timeout_minutes > 1440)
        return fail(reason, reason_size, "invalid Wi-Fi AP open time");
    if (config->ethernet_enabled && (config->ethernet_static ||
        config->ethernet_role == SX_NETWORK_ROLE_DOWNLINK) &&
        (!valid_ip(config->ethernet_lan_ip) || !valid_ip(config->ethernet_lan_netmask) ||
         !valid_ip(config->ethernet_gateway) || !valid_ip(config->ethernet_dns)))
        return fail(reason, reason_size, "invalid Ethernet static IPv4 settings");
    if (config->wifi_sta_enabled && config->wifi_sta_static &&
        (!valid_ip(config->wifi_sta_ip) || !valid_ip(config->wifi_sta_netmask) ||
         !valid_ip(config->wifi_sta_gateway) || !valid_ip(config->wifi_sta_dns)))
        return fail(reason, reason_size, "invalid Wi-Fi STA static IPv4 settings");
    if (config->modem_enabled && config->modem_role != SX_NETWORK_ROLE_UPLINK &&
        config->modem_role != SX_NETWORK_ROLE_BACKUP && config->modem_role != SX_NETWORK_ROLE_LAST)
        return fail(reason, reason_size, "4G role must be uplink, backup or last");
    if (config->wifi_sta_enabled && config->wifi_sta_role != SX_NETWORK_ROLE_UPLINK &&
        config->wifi_sta_role != SX_NETWORK_ROLE_BACKUP && config->wifi_sta_role != SX_NETWORK_ROLE_LAST)
        return fail(reason, reason_size, "Wi-Fi STA role must be uplink, backup or last");
    if (config->wifi_ap_enabled && config->wifi_ap_role != SX_NETWORK_ROLE_DOWNLINK &&
        config->wifi_ap_role != SX_NETWORK_ROLE_LOCAL)
        return fail(reason, reason_size, "Wi-Fi AP role must be downlink or local");
    if (config->routing_enabled) {
        if (config->nat_downlink == SX_NAT_DOWNLINK_WIFI_AP &&
            (!config->wifi_ap_enabled || config->wifi_ap_role != SX_NETWORK_ROLE_DOWNLINK))
            return fail(reason, reason_size, "NAT target Wi-Fi AP is not an enabled downlink");
        if (config->nat_downlink == SX_NAT_DOWNLINK_ETHERNET &&
            (!config->ethernet_enabled || config->ethernet_role != SX_NETWORK_ROLE_DOWNLINK))
            return fail(reason, reason_size, "NAT target Ethernet is not an enabled downlink");
    }
    if (reason != NULL && reason_size > 0) reason[0] = '\0';
    return ESP_OK;
}

const char *sx_network_role_name(sx_network_role_t role)
{
    switch (role) {
    case SX_NETWORK_ROLE_UPLINK: return "uplink";
    case SX_NETWORK_ROLE_BACKUP: return "backup";
    case SX_NETWORK_ROLE_LAST: return "last";
    case SX_NETWORK_ROLE_DOWNLINK: return "downlink";
    case SX_NETWORK_ROLE_LOCAL: return "local";
    default: return "off";
    }
}

bool sx_network_role_from_name(const char *name, sx_network_role_t *role)
{
    if (name == NULL || role == NULL) return false;
    for (int value = SX_NETWORK_ROLE_OFF; value <= SX_NETWORK_ROLE_LAST; ++value) {
        if (strcmp(name, sx_network_role_name((sx_network_role_t)value)) == 0) {
            *role = (sx_network_role_t)value;
            return true;
        }
    }
    return false;
}

const char *sx_nat_downlink_name(sx_nat_downlink_t downlink)
{
    return downlink == SX_NAT_DOWNLINK_ETHERNET ? "ethernet" : "wifi_ap";
}

bool sx_nat_downlink_from_name(const char *name, sx_nat_downlink_t *downlink)
{
    if (name == NULL || downlink == NULL) return false;
    if (strcmp(name, "wifi_ap") == 0) *downlink = SX_NAT_DOWNLINK_WIFI_AP;
    else if (strcmp(name, "ethernet") == 0) *downlink = SX_NAT_DOWNLINK_ETHERNET;
    else return false;
    return true;
}
