#include "sx_storage_defaults.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#include "sx_network_config.h"

#define TAG "storage_defaults"

typedef enum {
    DEFAULT_STRING,
    DEFAULT_U8,
} default_type_t;

typedef struct {
    const char *key;
    default_type_t type;
    const char *string_value;
    uint8_t u8_value;
} default_parameter_t;

#define STRING_DEFAULT(name, value) {name, DEFAULT_STRING, value, 0}
#define U8_DEFAULT(name, value) {name, DEFAULT_U8, NULL, (uint8_t)(value)}

/*
 * Authoritative first-boot parameter list. Network role values use the enum
 * constants so this table and sx_network_config_set_defaults() stay aligned.
 */
static const default_parameter_t s_default_parameters[] = {
    STRING_DEFAULT("netconn", "2"),
    STRING_DEFAULT("is_dhcp", "1"),
    STRING_DEFAULT("static_ip", ""),
    STRING_DEFAULT("static_netmask", ""),
    STRING_DEFAULT("static_gateway", ""),
    STRING_DEFAULT("static_dns1", "8.8.8.8"),
    STRING_DEFAULT("static_dns2", "114.114.114.114"),
    STRING_DEFAULT("wifi_ssid", ""),
    STRING_DEFAULT("wifi_password", ""),
    STRING_DEFAULT("ap_name", ""),
    STRING_DEFAULT("ap_password", ""),
    STRING_DEFAULT("ap_wait_time", "10"),
    STRING_DEFAULT("host_names", "SP603 多网络 IoT 网关"),
    STRING_DEFAULT("ntp_server", ""),
    STRING_DEFAULT("lgname", "admin"),
    STRING_DEFAULT("lgpwd", "12345678"),
    STRING_DEFAULT("device_sn", "0000000000000"),
    STRING_DEFAULT("w_mode", "serial_server"),
    STRING_DEFAULT("ap_timeout", "30"),
    STRING_DEFAULT("serial_mode", "separate"),
    STRING_DEFAULT("serialSyncMode", "separate"),
    STRING_DEFAULT("serial_layout", "dual_rs485"),
    STRING_DEFAULT("use_http", "0"),
    STRING_DEFAULT("http_port", "5000"),
    STRING_DEFAULT("httpconn", "1"),
    STRING_DEFAULT("http_url", "http://demo.likong.com"),
    STRING_DEFAULT("http_header", ""),
    STRING_DEFAULT("http_time", "5"),
    STRING_DEFAULT("use_tcp", ""),
    STRING_DEFAULT("tcpconn", ""),
    STRING_DEFAULT("tcp_server", "192.168.1.100"),
    STRING_DEFAULT("tcp_port", "8888"),
    STRING_DEFAULT("tcp_send", "0"),
    STRING_DEFAULT("tcp_time", "5"),
    STRING_DEFAULT("device_type", "SP603"),
    STRING_DEFAULT(SX_NVS_SCHEMA_KEY, SX_NVS_SCHEMA_VERSION),
    STRING_DEFAULT(SX_NVS_HW_PROFILE_KEY, SX_NVS_HW_PROFILE_VALUE),

    U8_DEFAULT("nm_eth_en", 1),
    U8_DEFAULT("nm_eth_role", SX_NETWORK_ROLE_UPLINK),
    STRING_DEFAULT("nm_eth_ip", "192.168.5.1"),
    STRING_DEFAULT("nm_eth_mask", "255.255.255.0"),
    U8_DEFAULT("nm_eth_static", 0),
    STRING_DEFAULT("nm_eth_gw", "192.168.5.254"),
    STRING_DEFAULT("nm_eth_dns", "8.8.8.8"),
    U8_DEFAULT("nm_eth_dhcp", 0),

    U8_DEFAULT("nm_sta_en", 0),
    U8_DEFAULT("nm_sta_role", SX_NETWORK_ROLE_UPLINK),
    U8_DEFAULT("nm_sta_static", 0),
    STRING_DEFAULT("nm_sta_ip", "192.168.1.100"),
    STRING_DEFAULT("nm_sta_mask", "255.255.255.0"),
    STRING_DEFAULT("nm_sta_gw", "192.168.1.1"),
    STRING_DEFAULT("nm_sta_dns", "8.8.8.8"),

    U8_DEFAULT("nm_ap_en", 1),
    U8_DEFAULT("nm_ap_role", SX_NETWORK_ROLE_DOWNLINK),
    STRING_DEFAULT("nm_ap_ip", "192.168.4.1"),
    STRING_DEFAULT("nm_ap_mask", "255.255.255.0"),
    U8_DEFAULT("nm_ap_dhcp", 1),

    U8_DEFAULT("nm_4g_en", 0),
    U8_DEFAULT("nm_4g_role", SX_NETWORK_ROLE_UPLINK),
};

static esp_err_t ensure_default(nvs_handle_t nvs, const default_parameter_t *parameter,
                                bool *added)
{
    if (parameter == NULL || parameter->key == NULL ||
        strlen(parameter->key) >= NVS_KEY_NAME_MAX_SIZE) {
        return ESP_ERR_NVS_KEY_TOO_LONG;
    }
    if (added != NULL) {
        *added = false;
    }

    esp_err_t err;
    if (parameter->type == DEFAULT_STRING) {
        size_t length = 0;
        err = nvs_get_str(nvs, parameter->key, NULL, &length);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_set_str(nvs, parameter->key, parameter->string_value);
            if (err == ESP_OK && added != NULL) {
                *added = true;
            }
        }
    } else {
        uint8_t value = 0;
        err = nvs_get_u8(nvs, parameter->key, &value);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = nvs_set_u8(nvs, parameter->key, parameter->u8_value);
            if (err == ESP_OK && added != NULL) {
                *added = true;
            }
        }
    }
    return err;
}

esp_err_t sx_storage_defaults_init(void)
{
    nvs_handle_t nvs = 0;
    ESP_RETURN_ON_ERROR(nvs_open(SX_NVS_NAMESPACE, NVS_READWRITE, &nvs), TAG,
                        "open storage namespace failed");

    size_t added_count = 0;
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(s_default_parameters) / sizeof(s_default_parameters[0]); ++i) {
        bool added = false;
        err = ensure_default(nvs, &s_default_parameters[i], &added);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "initialize %s failed: %s", s_default_parameters[i].key,
                     esp_err_to_name(err));
            break;
        }
        if (added) {
            ++added_count;
        }
    }

    if (err == ESP_OK && added_count > 0) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "default parameters ready, added=%u total=%u",
                 (unsigned)added_count,
                 (unsigned)(sizeof(s_default_parameters) / sizeof(s_default_parameters[0])));
    }
    return err;
}
