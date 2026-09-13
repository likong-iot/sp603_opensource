#include "sx_network_manager.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/inet.h"
#include "modem_manager.h"
#include "sx_init_wifi.h"
#include "sx_led_manager.h"
#include "w5500_manager.h"

static const char *TAG = "network_mgr";
static SemaphoreHandle_t s_mutex;
static sx_network_status_t s_status;
static sx_network_config_t s_config;
static esp_netif_ip_info_t s_wifi_ip;
static esp_netif_ip_info_t s_w5500_ip;
static esp_netif_ip_info_t s_modem_ip;
static esp_netif_t *s_napt_netif;
static bool s_started;

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


static int score(sx_network_interface_t id)
{
    return id == SX_NETWORK_IF_4G ? 0 : (id == SX_NETWORK_IF_W5500 ? 1 : 2);
}

static esp_netif_t *get_netif(sx_network_interface_t id)
{
    if (id == SX_NETWORK_IF_WIFI) return sx_wifi_get_sta_netif();
    if (id == SX_NETWORK_IF_W5500) return w5500_manager_get_netif();
    if (id == SX_NETWORK_IF_4G) return esp_netif_get_handle_from_ifkey("PPP_DEF");
    return NULL;
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
        best = score(SX_NETWORK_IF_W5500);
        selected = SX_NETWORK_IF_W5500;
        selected_ip = &s_w5500_ip;
    }
    if (s_config.wifi_sta_enabled && s_status.wifi_got_ip && is_uplink(s_config.wifi_sta_role)) {
        int value = score(SX_NETWORK_IF_WIFI);
        if (value < best) {
            best = value;
            selected = SX_NETWORK_IF_WIFI;
            selected_ip = &s_wifi_ip;
        }
    }
    if (s_config.modem_enabled && s_status.modem_got_ip && is_uplink(s_config.modem_role)) {
        int value = score(SX_NETWORK_IF_4G);
        if (value < best) {
            best = value;
            selected = SX_NETWORK_IF_4G;
            selected_ip = &s_modem_ip;
        }
    }
    if (selected != s_status.active_interface) {
        ESP_LOGI(TAG, "default uplink: %s -> %s",
                 sx_network_interface_name(s_status.active_interface),
                 sx_network_interface_name(selected));
        s_status.active_interface = selected;
        esp_netif_t *netif = get_netif(selected);
        if (netif != NULL) {
            esp_err_t err = esp_netif_set_default_netif(netif);
            if (err != ESP_OK) ESP_LOGE(TAG, "set default netif failed: %s", esp_err_to_name(err));
        } else if (selected == SX_NETWORK_IF_NONE) {
            esp_err_t err = esp_netif_set_default_netif(NULL);
            if (err != ESP_OK) ESP_LOGE(TAG, "clear default netif failed: %s", esp_err_to_name(err));
        }
    }
    set_status_ip_locked(selected_ip);
    update_napt_locked();
}

static void log_status_locked(const char *event)
{
    ESP_LOGI(TAG, "%s: active=%s wifi=%d/%d eth=%d/%d 4g=%d/%d napt=%d ip=%s",
             event, sx_network_interface_name(s_status.active_interface),
             s_status.wifi_connected, s_status.wifi_got_ip,
             s_status.w5500_link_up, s_status.w5500_got_ip,
             s_status.modem_usb_connected, s_status.modem_got_ip,
             s_status.napt_active, s_status.ip[0] ? s_status.ip : "0.0.0.0");
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

esp_err_t sx_network_manager_start_configured(void)
{
    if (s_mutex == NULL) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;
    s_started = true;
    esp_err_t first_error = ESP_OK;

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
            sx_led_manager_set_wifi(SX_LED_NETWORK_ERROR);
            ESP_LOGE(TAG, "start Wi-Fi failed: %s", esp_err_to_name(err));
            first_error = err;
        } else if (s_config.wifi_ap_enabled) {
            sx_led_manager_set_wifi(SX_LED_NETWORK_ONLINE);
        }
        if (err == ESP_OK) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.wifi_ap_started = s_config.wifi_ap_enabled;
            s_status.wifi_sta_started = s_config.wifi_sta_enabled;
            xSemaphoreGive(s_mutex);
        }
    }

    sx_led_manager_set_lan(s_config.ethernet_enabled ? SX_LED_NETWORK_STARTING : SX_LED_NETWORK_OFF);
    if (s_config.ethernet_enabled) {
        esp_err_t err = w5500_manager_init_with_role(
            s_config.ethernet_role == SX_NETWORK_ROLE_DOWNLINK,
            s_config.ethernet_lan_ip, s_config.ethernet_lan_netmask,
            s_config.ethernet_static, s_config.ethernet_gateway, s_config.ethernet_dns,
            s_config.ethernet_dhcp_enabled);
        if (err != ESP_OK) {
            sx_led_manager_set_lan(SX_LED_NETWORK_ERROR);
            ESP_LOGE(TAG, "start Ethernet failed: %s", esp_err_to_name(err));
            if (first_error == ESP_OK) first_error = err;
        } else {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.w5500_started = true;
            xSemaphoreGive(s_mutex);
        }
    }

    sx_led_manager_set_4g(s_config.modem_enabled ? SX_LED_NETWORK_STARTING : SX_LED_NETWORK_OFF);
    if (s_config.modem_enabled) {
        esp_err_t err = modem_manager_init();
        if (err != ESP_OK) {
            sx_led_manager_set_4g(SX_LED_NETWORK_ERROR);
            ESP_LOGE(TAG, "start 4G failed: %s", esp_err_to_name(err));
            if (first_error == ESP_OK) first_error = err;
        } else {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_status.modem_started = true;
            xSemaphoreGive(s_mutex);
        }
    }
    return first_error;
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

const char *sx_network_interface_name(sx_network_interface_t id)
{
    if (id == SX_NETWORK_IF_WIFI) return "wifi_sta";
    if (id == SX_NETWORK_IF_W5500) return "ethernet";
    if (id == SX_NETWORK_IF_4G) return "4g";
    return "none";
}

void sx_network_manager_wifi_connected(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.wifi_connected = true;
    sx_led_manager_set_wifi(SX_LED_NETWORK_STARTING);
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
    sx_led_manager_set_wifi(s_config.wifi_ap_enabled ? SX_LED_NETWORK_ONLINE : SX_LED_NETWORK_STARTING);
    select_uplink_locked();
    log_status_locked("Wi-Fi STA disconnected");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_wifi_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_mutex || !info) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.wifi_connected = s_status.wifi_got_ip = true;
    s_wifi_ip = *info;
    ip_to_text(info, s_status.wifi_ip);
    sx_led_manager_set_wifi(SX_LED_NETWORK_ONLINE);
    select_uplink_locked();
    log_status_locked("Wi-Fi STA got IP");
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
    sx_led_manager_set_lan(s_config.ethernet_role == SX_NETWORK_ROLE_DOWNLINK
                               ? SX_LED_NETWORK_ONLINE : SX_LED_NETWORK_STARTING);
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
    sx_led_manager_set_lan(SX_LED_NETWORK_STARTING);
    select_uplink_locked();
    log_status_locked("Ethernet link down");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_w5500_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_mutex || !info) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.w5500_link_up = s_status.w5500_got_ip = true;
    s_w5500_ip = *info;
    ip_to_text(info, s_status.ethernet_ip);
    sx_led_manager_set_lan(SX_LED_NETWORK_ONLINE);
    select_uplink_locked();
    log_status_locked("Ethernet got IP");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_modem_connected(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.modem_usb_connected = true;
    sx_led_manager_set_4g(SX_LED_NETWORK_STARTING);
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
    sx_led_manager_set_4g(SX_LED_NETWORK_STARTING);
    select_uplink_locked();
    log_status_locked("4G disconnected");
    xSemaphoreGive(s_mutex);
}

void sx_network_manager_modem_got_ip(const esp_netif_ip_info_t *info)
{
    if (!s_mutex || !info) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.modem_usb_connected = s_status.modem_got_ip = true;
    s_modem_ip = *info;
    ip_to_text(info, s_status.modem_ip);
    sx_led_manager_set_4g(SX_LED_NETWORK_ONLINE);
    select_uplink_locked();
    log_status_locked("4G got IP");
    xSemaphoreGive(s_mutex);
}
