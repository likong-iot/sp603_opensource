#include "sx_network_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "modem_manager.h"
#include "sx_init_wifi.h"
#include "sx_led_manager.h"
#include "sx_web_server.h"
#include "w5500_manager.h"

static const char *TAG = "network_mgr";
static SemaphoreHandle_t s_mutex;
static sx_network_status_t s_status;
static sx_network_config_t s_config;
static esp_netif_ip_info_t s_wifi_ip;
static esp_netif_ip_info_t s_w5500_ip;
static esp_netif_ip_info_t s_modem_ip;
static esp_netif_t *s_napt_netif;
static bool s_management_started;
static bool s_remaining_started;
static bool s_application_ready;
static bool s_wifi_start_error;
static bool s_w5500_start_error;
static bool s_modem_start_error;

static void ip_to_text(const esp_netif_ip_info_t *info, char out[16])
{
    memset(out, 0, 16);
    if (info != NULL) inet_ntoa_r(info->ip, out, 16);
}

static void set_status_ip_locked(const esp_netif_ip_info_t *info)
{
    memset(s_status.ip, 0, sizeof(s_status.ip));
    memset(s_status.gateway, 0, sizeof(s_status.gateway));
    memset(s_status.netmask, 0, sizeof(s_status.netmask));
    if (info == NULL) return;
    inet_ntoa_r(info->ip, s_status.ip, sizeof(s_status.ip));
    inet_ntoa_r(info->gw, s_status.gateway, sizeof(s_status.gateway));
    inet_ntoa_r(info->netmask, s_status.netmask, sizeof(s_status.netmask));
}

static bool is_uplink(sx_network_role_t role)
{
    return role == SX_NETWORK_ROLE_UPLINK || role == SX_NETWORK_ROLE_BACKUP || role == SX_NETWORK_ROLE_LAST;
}


static int interface_tiebreak_score(sx_network_interface_t id)
{
    return id == SX_NETWORK_IF_4G ? 0 : (id == SX_NETWORK_IF_W5500 ? 1 : 2);
}

static int role_score(sx_network_role_t role)
{
    if (role == SX_NETWORK_ROLE_UPLINK) return 0;
    if (role == SX_NETWORK_ROLE_BACKUP) return 100;
    if (role == SX_NETWORK_ROLE_LAST) return 200;
    return 1000;
}

static int uplink_score(sx_network_interface_t id, sx_network_role_t role)
{
    return role_score(role) + interface_tiebreak_score(id);
}

static esp_netif_t *get_netif(sx_network_interface_t id)
{
    if (id == SX_NETWORK_IF_WIFI) return sx_wifi_get_sta_netif();
    if (id == SX_NETWORK_IF_W5500) return w5500_manager_get_netif();
    if (id == SX_NETWORK_IF_4G) return esp_netif_get_handle_from_ifkey("PPP_DEF");
    return NULL;
}

static bool usable_ipv4_dns(const esp_netif_dns_info_t *dns)
{
    return dns != NULL && dns->ip.type == ESP_IPADDR_TYPE_V4 &&
           dns->ip.u_addr.ip4.addr != 0;
}

static const char *configured_dns_locked(sx_network_interface_t id)
{
    if (id == SX_NETWORK_IF_WIFI) return s_config.wifi_sta_dns;
    if (id == SX_NETWORK_IF_W5500) return s_config.ethernet_dns;
    return NULL;
}

static void refresh_active_dns_locked(sx_network_interface_t id, esp_netif_t *netif)
{
    s_status.active_dns[0] = '\0';
    s_status.dns_ready = false;
    if (id == SX_NETWORK_IF_NONE || netif == NULL) return;

    esp_netif_dns_info_t dns = {0};
    esp_err_t err = esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
    if (err != ESP_OK || !usable_ipv4_dns(&dns)) {
        const char *fallback = configured_dns_locked(id);
        ip4_addr_t address = {0};
        if (fallback != NULL && ip4addr_aton(fallback, &address)) {
            memset(&dns, 0, sizeof(dns));
            dns.ip.type = ESP_IPADDR_TYPE_V4;
            dns.ip.u_addr.ip4.addr = address.addr;
            err = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns);
            if (err == ESP_OK) {
                char text[128];
                snprintf(text, sizeof(text),
                         "%s did not provide DNS; using configured fallback %s",
                         sx_network_interface_name(id), fallback);
                ESP_LOGW(TAG, "%s", text);
                send_system_log("WARN", "network", text);
            } else {
                ESP_LOGE(TAG, "set %s fallback DNS failed: %s",
                         sx_network_interface_name(id), esp_err_to_name(err));
            }
        }
    }
    if (!usable_ipv4_dns(&dns)) return;

    inet_ntoa_r(dns.ip.u_addr.ip4, s_status.active_dns,
                sizeof(s_status.active_dns));
    s_status.dns_ready = true;
}

static bool interface_enabled_locked(sx_network_interface_t id)
{
    if (id == SX_NETWORK_IF_WIFI) return s_config.wifi_ap_enabled || s_config.wifi_sta_enabled;
    if (id == SX_NETWORK_IF_W5500) return s_config.ethernet_enabled;
    if (id == SX_NETWORK_IF_4G) return s_config.modem_enabled;
    return false;
}

static bool wifi_ap_is_running_locked(void)
{
    return s_config.wifi_ap_enabled &&
           s_status.wifi_ap_started;
}

static bool ethernet_is_provider_locked(void)
{
    return s_config.ethernet_enabled &&
           s_config.ethernet_role == SX_NETWORK_ROLE_DOWNLINK &&
           s_status.w5500_started;
}

static sx_led_network_state_t interface_led_state_locked(sx_network_interface_t id)
{
    if (!interface_enabled_locked(id)) return SX_LED_NETWORK_OFF;
    if ((id == SX_NETWORK_IF_WIFI && s_wifi_start_error) ||
        (id == SX_NETWORK_IF_W5500 && s_w5500_start_error) ||
        (id == SX_NETWORK_IF_4G && s_modem_start_error)) {
        return SX_LED_NETWORK_ERROR;
    }
    if (id == SX_NETWORK_IF_WIFI && s_status.active_interface == id) {
        return wifi_ap_is_running_locked()
                   ? SX_LED_NETWORK_ONLINE_AND_PROVIDER
                   : SX_LED_NETWORK_ONLINE;
    }
    if (id == SX_NETWORK_IF_WIFI && wifi_ap_is_running_locked()) {
        return SX_LED_NETWORK_PROVIDER;
    }
    if (s_status.active_interface == id) return SX_LED_NETWORK_ONLINE;
    if (id == SX_NETWORK_IF_W5500 && ethernet_is_provider_locked()) {
        return SX_LED_NETWORK_PROVIDER;
    }
    return SX_LED_NETWORK_STARTING;
}

static void update_network_leds_locked(void)
{
    sx_led_manager_set_lan(interface_led_state_locked(SX_NETWORK_IF_W5500));
    sx_led_manager_set_wifi(interface_led_state_locked(SX_NETWORK_IF_WIFI));
    sx_led_manager_set_4g(interface_led_state_locked(SX_NETWORK_IF_4G));
}

static bool has_configured_uplink_locked(void)
{
    return (s_config.ethernet_enabled && is_uplink(s_config.ethernet_role)) ||
           (s_config.wifi_sta_enabled && is_uplink(s_config.wifi_sta_role)) ||
           (s_config.modem_enabled && is_uplink(s_config.modem_role));
}

static void update_system_indicator_locked(void)
{
    if (!s_application_ready) return;
    if (s_wifi_start_error || s_w5500_start_error || s_modem_start_error ||
        (has_configured_uplink_locked() &&
         (s_status.active_interface == SX_NETWORK_IF_NONE || !s_status.dns_ready))) {
        sx_led_manager_set_system_state(SX_LED_SYSTEM_WARNING);
    } else {
        sx_led_manager_set_system_state(SX_LED_SYSTEM_NORMAL);
    }
}

static void update_napt_locked(void)
{
    esp_netif_t *wanted = NULL;
    if (s_config.routing_enabled && s_status.active_interface != SX_NETWORK_IF_NONE) {
        wanted = s_config.nat_downlink == SX_NAT_DOWNLINK_ETHERNET
                     ? w5500_manager_get_netif() : sx_wifi_get_ap_netif();
    }
    if (wanted == s_napt_netif) return;
    if (s_napt_netif != NULL) {
        esp_netif_napt_disable(s_napt_netif);
        s_napt_netif = NULL;
    }
    if (wanted != NULL) {
        esp_err_t err = esp_netif_napt_enable(wanted);
        if (err == ESP_OK) {
            s_napt_netif = wanted;
            ESP_LOGI(TAG, "NAPT enabled on %s", sx_nat_downlink_name(s_config.nat_downlink));
        } else {
            ESP_LOGE(TAG, "enable NAPT failed: %s", esp_err_to_name(err));
        }
    }
    s_status.napt_active = s_napt_netif != NULL;
}

static void select_uplink_locked(void)
{
    sx_network_interface_t selected = SX_NETWORK_IF_NONE;
    const esp_netif_ip_info_t *selected_ip = NULL;
    int best = 100000;
    if (s_config.ethernet_enabled && s_status.w5500_got_ip && is_uplink(s_config.ethernet_role)) {
        best = uplink_score(SX_NETWORK_IF_W5500, s_config.ethernet_role);
        selected = SX_NETWORK_IF_W5500;
        selected_ip = &s_w5500_ip;
    }
    if (s_config.wifi_sta_enabled && s_status.wifi_got_ip && is_uplink(s_config.wifi_sta_role)) {
        int value = uplink_score(SX_NETWORK_IF_WIFI, s_config.wifi_sta_role);
        if (value < best) {
            best = value;
            selected = SX_NETWORK_IF_WIFI;
            selected_ip = &s_wifi_ip;
        }
    }
    if (s_config.modem_enabled && s_status.modem_got_ip && is_uplink(s_config.modem_role)) {
        int value = uplink_score(SX_NETWORK_IF_4G, s_config.modem_role);
        if (value < best) {
            best = value;
            selected = SX_NETWORK_IF_4G;
            selected_ip = &s_modem_ip;
        }
    }
    const sx_network_interface_t previous = s_status.active_interface;
    const bool uplink_changed = selected != previous;
    esp_netif_t *netif = get_netif(selected);
    if (uplink_changed) {
        ESP_LOGI(TAG, "default uplink: %s -> %s",
                 sx_network_interface_name(previous),
                 sx_network_interface_name(selected));
        s_status.active_interface = selected;
        if (netif != NULL) {
            esp_err_t err = esp_netif_set_default_netif(netif);
            if (err != ESP_OK) ESP_LOGE(TAG, "set default netif failed: %s", esp_err_to_name(err));
        } else if (selected == SX_NETWORK_IF_NONE) {
            esp_err_t err = esp_netif_set_default_netif(NULL);
            if (err != ESP_OK) ESP_LOGE(TAG, "clear default netif failed: %s", esp_err_to_name(err));
        }
        ++s_status.uplink_generation;
    }
    set_status_ip_locked(selected_ip);
    refresh_active_dns_locked(selected, netif);
    if (uplink_changed) {
        char text[160];
        if (selected == SX_NETWORK_IF_NONE) {
            snprintf(text, sizeof(text), "No usable uplink; previous=%s",
                     sx_network_interface_name(previous));
            send_system_log("WARN", "network", text);
        } else {
            snprintf(text, sizeof(text),
                     "Active uplink=%s ip=%s gateway=%s dns=%s",
                     sx_network_interface_name(selected), s_status.ip,
                     s_status.gateway,
                     s_status.dns_ready ? s_status.active_dns : "unavailable");
            send_system_log(s_status.dns_ready ? "INFO" : "WARN", "network", text);
        }
    }
    update_napt_locked();
    update_network_leds_locked();
    update_system_indicator_locked();
}

static void log_status_locked(const char *event)
{
    ESP_LOGI(TAG, "%s: active=%s wifi=%d/%d eth=%d/%d 4g=%d/%d napt=%d ip=%s dns=%s gen=%lu",
             event, sx_network_interface_name(s_status.active_interface),
             s_status.wifi_connected, s_status.wifi_got_ip,
             s_status.w5500_link_up, s_status.w5500_got_ip,
             s_status.modem_usb_connected, s_status.modem_got_ip,
             s_status.napt_active, s_status.ip[0] ? s_status.ip : "0.0.0.0",
             s_status.dns_ready ? s_status.active_dns : "unavailable",
             (unsigned long)s_status.uplink_generation);
    char text[192];
    snprintf(text, sizeof(text),
             "%s active=%s ip=%s dns=%s",
             event, sx_network_interface_name(s_status.active_interface),
             s_status.ip[0] ? s_status.ip : "0.0.0.0",
             s_status.dns_ready ? s_status.active_dns : "unavailable");
    send_system_log(s_status.dns_ready ? "INFO" : "WARN", "network", text);
}

esp_err_t sx_network_manager_init(void)
{
    if (s_mutex != NULL) return ESP_OK;
    s_mutex = xSemaphoreCreateMutex();
    if (s_mutex == NULL) return ESP_ERR_NO_MEM;
    memset(&s_status, 0, sizeof(s_status));
    ESP_RETURN_ON_ERROR(sx_network_config_load(&s_config), TAG, "load config failed");
    sx_network_config_normalize(&s_config);
    s_status.routing_enabled = s_config.routing_enabled;
    ESP_LOGI(TAG, "SP603 config: Ethernet=%d WiFi-STA=%d WiFi-AP=%d 4G=%d route=%d",
             s_config.ethernet_enabled, s_config.wifi_sta_enabled,
             s_config.wifi_ap_enabled, s_config.modem_enabled, s_config.routing_enabled);
    return ESP_OK;
}

esp_err_t sx_network_manager_start_management_network(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (s_management_started) return ESP_OK;
    s_management_started = true;
    s_wifi_start_error = false;
    esp_err_t first_error = ESP_OK;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    update_network_leds_locked();
    xSemaphoreGive(s_mutex);

    sx_led_manager_set_wifi((s_config.wifi_ap_enabled || s_config.wifi_sta_enabled)
                                ? SX_LED_NETWORK_STARTING : SX_LED_NETWORK_OFF);
    if (s_config.wifi_ap_enabled || s_config.wifi_sta_enabled) {
        esp_err_t err = sx_wifi_start_configured(s_config.wifi_ap_enabled,
                                                 s_config.wifi_sta_enabled,
                                                 s_config.wifi_ssid,
                                                 s_config.wifi_password,
                                                 s_config.wifi_sta_static,
                                                 s_config.wifi_sta_ip, s_config.wifi_sta_netmask,
                                                 s_config.wifi_sta_gateway, s_config.wifi_sta_dns,
                                                 s_config.ap_ip,
                                                 s_config.ap_netmask,
                                                 s_config.wifi_ap_role == SX_NETWORK_ROLE_LOCAL ||
                                                     s_config.wifi_ap_dhcp_enabled);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start Wi-Fi failed: %s", esp_err_to_name(err));
            first_error = err;
        }
        if (err == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.wifi_ap_started = s_config.wifi_ap_enabled;
            s_status.wifi_sta_started = s_config.wifi_sta_enabled;
            update_network_leds_locked();
            xSemaphoreGive(s_mutex);
        } else {
            s_wifi_start_error = true;
        }
    }

    return first_error;
}

esp_err_t sx_network_manager_start_remaining_networks(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (s_remaining_started) return ESP_OK;
    s_remaining_started = true;
    s_w5500_start_error = false;
    s_modem_start_error = false;
    esp_err_t first_error = ESP_OK;

    sx_led_manager_set_lan(s_config.ethernet_enabled ? SX_LED_NETWORK_STARTING : SX_LED_NETWORK_OFF);
    if (s_config.ethernet_enabled) {
        esp_err_t err = w5500_manager_init_with_role(
            s_config.ethernet_role == SX_NETWORK_ROLE_DOWNLINK,
            s_config.ethernet_lan_ip, s_config.ethernet_lan_netmask,
            s_config.ethernet_static, s_config.ethernet_gateway, s_config.ethernet_dns,
            s_config.ethernet_dhcp_enabled);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start Ethernet failed: %s", esp_err_to_name(err));
            if (first_error == ESP_OK) first_error = err;
        } else {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.w5500_started = true;
            update_network_leds_locked();
            xSemaphoreGive(s_mutex);
        }
        if (err != ESP_OK) s_w5500_start_error = true;
    }

    sx_led_manager_set_4g(s_config.modem_enabled ? SX_LED_NETWORK_STARTING : SX_LED_NETWORK_OFF);
    if (s_config.modem_enabled) {
        esp_err_t err = modem_manager_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "start 4G failed: %s", esp_err_to_name(err));
            if (first_error == ESP_OK) first_error = err;
        } else {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.modem_started = true;
            update_network_leds_locked();
            xSemaphoreGive(s_mutex);
        }
        if (err != ESP_OK) s_modem_start_error = true;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    update_network_leds_locked();
    xSemaphoreGive(s_mutex);
    return first_error;
}

esp_err_t sx_network_manager_start_configured(void)
{
    esp_err_t management_err = sx_network_manager_start_management_network();
    esp_err_t remaining_err = sx_network_manager_start_remaining_networks();
    return management_err != ESP_OK ? management_err : remaining_err;
}

void sx_network_manager_get_status(sx_network_status_t *status)
{
    if (status == NULL || s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *status = s_status;
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_get_config(sx_network_config_t *config)
{
    if (config == NULL || s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *config = s_config;
    xSemaphoreGive(s_mutex);
}

esp_err_t sx_network_manager_save_config(const sx_network_config_t *config)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    sx_network_config_t normalized = *config;
    sx_network_config_normalize(&normalized);
    ESP_LOGI(TAG, "[NETSAVE] NVS save begin");
    esp_err_t err = sx_network_config_save(&normalized);
    ESP_LOGI(TAG, "[NETSAVE] NVS save result=%s", esp_err_to_name(err));
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "[NETSAVE] state lock wait");
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "[NETSAVE] state lock acquired");
    s_config = normalized;
    s_status.routing_enabled = normalized.routing_enabled;
    s_status.reboot_required = true;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

void sx_network_manager_mark_application_ready(bool startup_ok)
{
    if (s_mutex == NULL) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_application_ready = true;
    if (!startup_ok) {
        sx_led_manager_set_system_state(SX_LED_SYSTEM_WARNING);
    } else {
        update_system_indicator_locked();
    }
    xSemaphoreGive(s_mutex);
}

const char *sx_network_interface_name(sx_network_interface_t id)
{
    if (id == SX_NETWORK_IF_WIFI) return "wifi_sta";
    if (id == SX_NETWORK_IF_W5500) return "ethernet";
    if (id == SX_NETWORK_IF_4G) return "4g";
    return "none";
}

static bool ip_info_changed(const esp_netif_ip_info_t *old_info,
                            const esp_netif_ip_info_t *new_info)
{
    return old_info->ip.addr != new_info->ip.addr ||
           old_info->gw.addr != new_info->gw.addr ||
           old_info->netmask.addr != new_info->netmask.addr;
}

static void record_active_address_change_locked(sx_network_interface_t id,
                                                bool address_changed)
{
    if (!address_changed || s_status.active_interface != id) return;
    ++s_status.uplink_generation;
    char text[144];
    snprintf(text, sizeof(text), "%s address changed: ip=%s gateway=%s dns=%s",
             sx_network_interface_name(id), s_status.ip, s_status.gateway,
             s_status.dns_ready ? s_status.active_dns : "unavailable");
    send_system_log(s_status.dns_ready ? "INFO" : "WARN", "network", text);
}

void sx_network_manager_wifi_connected(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.wifi_connected = true;
    update_network_leds_locked();
    log_status_locked("Wi-Fi STA connected");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_wifi_disconnected(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.wifi_connected = false;
    s_status.wifi_got_ip = false;
    s_status.wifi_ip[0] = '\0';
    memset(&s_wifi_ip, 0, sizeof(s_wifi_ip));
    update_network_leds_locked();
    select_uplink_locked();
    log_status_locked("Wi-Fi STA disconnected");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_wifi_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_mutex || !info) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool address_changed = s_status.active_interface == SX_NETWORK_IF_WIFI &&
                                 ip_info_changed(&s_wifi_ip, info);
    s_status.wifi_connected = s_status.wifi_got_ip = true;
    s_wifi_ip = *info;
    ip_to_text(info, s_status.wifi_ip);
    update_network_leds_locked();
    select_uplink_locked();
    record_active_address_change_locked(SX_NETWORK_IF_WIFI, address_changed);
    log_status_locked("Wi-Fi STA got IP");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_wifi_lost_ip(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.wifi_got_ip = false;
    s_status.wifi_ip[0] = '\0';
    memset(&s_wifi_ip, 0, sizeof(s_wifi_ip));
    select_uplink_locked();
    log_status_locked("Wi-Fi STA lost IP");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_wifi_ap_client_joined(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_status.wifi_ap_client_count < UINT8_MAX) {
        ++s_status.wifi_ap_client_count;
    }
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_wifi_ap_client_left(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_status.wifi_ap_client_count > 0) {
        --s_status.wifi_ap_client_count;
    }
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_w5500_link_up(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.w5500_link_up = true;
    update_network_leds_locked();
    log_status_locked("Ethernet link up");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_w5500_link_down(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.w5500_link_up = s_status.w5500_got_ip = false;
    s_status.ethernet_ip[0] = '\0';
    memset(&s_w5500_ip, 0, sizeof(s_w5500_ip));
    update_network_leds_locked();
    select_uplink_locked();
    log_status_locked("Ethernet link down");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_w5500_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_mutex || !info) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool address_changed = s_status.active_interface == SX_NETWORK_IF_W5500 &&
                                 ip_info_changed(&s_w5500_ip, info);
    s_status.w5500_link_up = s_status.w5500_got_ip = true;
    s_w5500_ip = *info;
    ip_to_text(info, s_status.ethernet_ip);
    update_network_leds_locked();
    select_uplink_locked();
    record_active_address_change_locked(SX_NETWORK_IF_W5500, address_changed);
    log_status_locked("Ethernet got IP");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_w5500_lost_ip(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.w5500_got_ip = false;
    s_status.ethernet_ip[0] = '\0';
    memset(&s_w5500_ip, 0, sizeof(s_w5500_ip));
    select_uplink_locked();
    log_status_locked("Ethernet lost IP");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_modem_connected(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.modem_usb_connected = true;
    update_network_leds_locked();
    log_status_locked("4G modem connected");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_modem_disconnected(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.modem_usb_connected = s_status.modem_got_ip = false;
    s_status.modem_ip[0] = '\0';
    memset(&s_modem_ip, 0, sizeof(s_modem_ip));
    update_network_leds_locked();
    select_uplink_locked();
    log_status_locked("4G disconnected");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_modem_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_mutex || !info) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    const bool address_changed = s_status.active_interface == SX_NETWORK_IF_4G &&
                                 ip_info_changed(&s_modem_ip, info);
    s_status.modem_usb_connected = s_status.modem_got_ip = true;
    s_modem_ip = *info;
    ip_to_text(info, s_status.modem_ip);
    update_network_leds_locked();
    select_uplink_locked();
    record_active_address_change_locked(SX_NETWORK_IF_4G, address_changed);
    log_status_locked("4G got IP");
    xSemaphoreGive(s_mutex);
}
