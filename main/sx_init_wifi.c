#include "sx_init_wifi.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_network_manager.h"
#include "freertos/semphr.h"

#include "app_task_utils.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "WIFI_STA";

#define WIFI_TASK_STACK_SIZE 4096
#define WIFI_TASK_PRIORITY 8

#define DEFAULT_AP_SSID_PREFIX "SP603_"
#define DEFAULT_AP_PASSWORD "12345678"
#define DEFAULT_AP_CHANNEL 5
#define DEFAULT_AP_MAX_CONN 5

#define NVS_NAMESPACE "storage"

typedef struct {
  char ssid[33];
  char password[65];
} wifi_credentials_t;

static bool s_wifi_initialized = false;
static bool s_wifi_started = false;
static bool s_wifi_handlers_registered = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static SemaphoreHandle_t s_scan_mutex = NULL;
static esp_timer_handle_t s_sta_retry_timer = NULL;
static volatile bool s_sta_connect_enabled = false;
static volatile uint8_t s_sta_retry_count = 0;

static void schedule_sta_retry(void);

static void sta_retry_timer_cb(void *arg)
{
  (void)arg;
  if (!s_sta_connect_enabled) return;
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    ESP_LOGW(TAG, "STA retry failed: %s", esp_err_to_name(err));
    schedule_sta_retry();
  }
}

static void cancel_sta_retry(void)
{
  if (s_sta_retry_timer != NULL) (void)esp_timer_stop(s_sta_retry_timer);
}

static void schedule_sta_retry(void)
{
  if (!s_sta_connect_enabled || s_sta_retry_timer == NULL) return;
  uint8_t attempt = s_sta_retry_count;
  if (s_sta_retry_count < 6) ++s_sta_retry_count;
  uint32_t delay_seconds = 1U << (attempt > 5 ? 5 : attempt);
  if (delay_seconds > 30U) delay_seconds = 30U;
  (void)esp_timer_stop(s_sta_retry_timer);
  esp_err_t err = esp_timer_start_once(s_sta_retry_timer,
                                       (uint64_t)delay_seconds * 1000000ULL);
  if (err != ESP_OK) ESP_LOGW(TAG, "schedule STA retry failed: %s", esp_err_to_name(err));
}

static esp_err_t start_wifi_if_needed(void) {
  if (s_wifi_started) {
    return ESP_OK;
  }

  esp_err_t err = esp_wifi_start();
  if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
    s_wifi_started = true;
    return ESP_OK;
  }

  return err;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
    wifi_event_ap_staconnected_t *event =
        (wifi_event_ap_staconnected_t *)event_data;
    if (event != NULL) {
      ESP_LOGI(TAG, "station " MACSTR " join, AID=%d", MAC2STR(event->mac),
               event->aid);
      sx_network_manager_wifi_ap_client_joined();
    }
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_AP_STADISCONNECTED) {
    wifi_event_ap_stadisconnected_t *event =
        (wifi_event_ap_stadisconnected_t *)event_data;
    if (event != NULL) {
      ESP_LOGI(TAG, "station " MACSTR " leave, AID=%d", MAC2STR(event->mac),
               event->aid);
      sx_network_manager_wifi_ap_client_left();
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    ESP_LOGI(TAG, "STA connected");
    sx_network_manager_wifi_connected();
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "STA disconnected, schedule retry");
    sx_network_manager_wifi_disconnected();
    schedule_sta_retry();
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (event != NULL && event->esp_netif == s_sta_netif) {
      ESP_LOGI(TAG, "STA got ip: " IPSTR, IP2STR(&event->ip_info.ip));
      s_sta_retry_count = 0;
      cancel_sta_retry();
      sx_network_manager_wifi_got_ip(&event->ip_info);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    ESP_LOGW(TAG, "STA lost IP address");
    sx_network_manager_wifi_lost_ip();
  }
}

static void copy_string_safely(char *dst, size_t dst_size, const char *src) {
  if (dst == NULL || dst_size == 0) {
    return;
  }

  if (src == NULL) {
    dst[0] = '\0';
    return;
  }

  size_t n = strnlen(src, dst_size - 1);
  memcpy(dst, src, n);
  dst[n] = '\0';
}

static void read_nvs_string_default(nvs_handle_t nvs_handle, const char *key,
                                    const char *def, char *out,
                                    size_t out_size) {
  if (out == NULL || out_size == 0) {
    return;
  }

  out[0] = '\0';
  if (def != NULL) {
    copy_string_safely(out, out_size, def);
  }

  size_t len = out_size;
  esp_err_t err = nvs_get_str(nvs_handle, key, out, &len);
  if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(TAG, "read nvs key %s failed: %s", key, esp_err_to_name(err));
  }
}

static esp_err_t ensure_wifi_driver_ready(void) {
  if (!s_wifi_initialized) {
    if (s_sta_netif == NULL) {
      s_sta_netif = esp_netif_create_default_wifi_sta();
    }
    if (s_ap_netif == NULL) {
      s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
      return err;
    }
    s_wifi_initialized = true;
  }

  if (!s_wifi_handlers_registered) {
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                   &wifi_event_handler, NULL),
        TAG, "register WIFI_EVENT handler failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                   &wifi_event_handler, NULL),
        TAG, "register IP_EVENT_STA_GOT_IP handler failed");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP,
                                   &wifi_event_handler, NULL),
        TAG, "register IP_EVENT_STA_LOST_IP handler failed");
    s_wifi_handlers_registered = true;
  }

  if (s_sta_retry_timer == NULL) {
    const esp_timer_create_args_t timer_args = {
        .callback = &sta_retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_sta_retry",
        .skip_unhandled_events = false,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &s_sta_retry_timer),
                        TAG, "create STA retry timer failed");
  }

  return ESP_OK;
}

esp_err_t sx_wifi_scan_access_points(wifi_ap_record_t *records,
                                     uint16_t *record_count) {
  if (records == NULL || record_count == NULL || *record_count == 0) {
    return ESP_ERR_INVALID_ARG;
  }

  if (s_scan_mutex == NULL) {
    s_scan_mutex = xSemaphoreCreateMutex();
    if (s_scan_mutex == NULL) return ESP_ERR_NO_MEM;
  }
  if (xSemaphoreTake(s_scan_mutex, 0) != pdTRUE) {
    return ESP_ERR_INVALID_STATE;
  }

  esp_err_t err = ensure_wifi_driver_ready();
  wifi_mode_t original_mode = WIFI_MODE_NULL;
  wifi_mode_t scan_mode = WIFI_MODE_STA;
  bool mode_changed = false;
  const bool was_started = s_wifi_started;

  if (err == ESP_OK) err = esp_wifi_get_mode(&original_mode);
  if (err == ESP_OK) {
    scan_mode = original_mode == WIFI_MODE_AP ? WIFI_MODE_APSTA :
                original_mode == WIFI_MODE_NULL ? WIFI_MODE_STA : original_mode;
    if (scan_mode != original_mode) {
      err = esp_wifi_set_mode(scan_mode);
      mode_changed = err == ESP_OK;
    }
  }
  if (err == ESP_OK) err = start_wifi_if_needed();

  /* esp_wifi_set_mode() is asynchronous.  Give the driver time to settle
     before requesting a scan, otherwise AP-only startup can return
     ESP_ERR_WIFI_STATE and the web page sees an empty result. */
  if (err == ESP_OK && mode_changed) vTaskDelay(pdMS_TO_TICKS(500));

  if (err == ESP_OK) {
    wifi_scan_config_t config = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 120, .max = 500},
    };
    esp_err_t stop_err = esp_wifi_scan_stop();
    ESP_LOGI(TAG, "Wi-Fi scan: stop previous scan: %s", esp_err_to_name(stop_err));
    err = esp_wifi_scan_start(&config, true);
    if (err != ESP_OK) {
      ESP_LOGW(TAG, "active Wi-Fi scan failed: %s, retrying passive scan", esp_err_to_name(err));
      wifi_scan_config_t fallback = config;
      fallback.scan_type = WIFI_SCAN_TYPE_PASSIVE;
      fallback.scan_time.passive = 1000;
      err = esp_wifi_scan_start(&fallback, true);
    }
  }

  if (err == ESP_OK) {
    uint16_t found = 0;
    err = esp_wifi_scan_get_ap_num(&found);
    if (err == ESP_OK) {
      uint16_t requested = *record_count;
      if (found < requested) requested = found;
      *record_count = requested;
      if (requested > 0) err = esp_wifi_scan_get_ap_records(record_count, records);
    }
  }

  /* Restoring APSTA back to AP immediately resets the HTTP connection that
     initiated the scan.  Keep APSTA after an AP-only scan so the result can be
     returned to the browser; AP remains active in APSTA mode. */
  if (mode_changed && original_mode != WIFI_MODE_NULL && original_mode != WIFI_MODE_AP) {
    esp_err_t restore_err = esp_wifi_set_mode(original_mode);
    if (err == ESP_OK && restore_err != ESP_OK) err = restore_err;
  }
  if (!was_started && s_wifi_started) {
    esp_err_t stop_err = esp_wifi_stop();
    if (stop_err == ESP_OK || stop_err == ESP_ERR_WIFI_NOT_STARTED) {
      s_wifi_started = false;
    } else if (err == ESP_OK) {
      err = stop_err;
    }
  }

  xSemaphoreGive(s_scan_mutex);
  return err;
}

esp_err_t sx_wifi_init_softap_from_nvs(void) {
  ESP_RETURN_ON_ERROR(ensure_wifi_driver_ready(), TAG, "wifi init failed");

  uint8_t sta_mac[6] = {0};
  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);

  char default_ssid[64] = {0};
  snprintf(default_ssid, sizeof(default_ssid), "%s%02X%02X",
           DEFAULT_AP_SSID_PREFIX, sta_mac[4], sta_mac[5]);

  char ap_ssid[33] = {0};
  char ap_password[65] = {0};

  nvs_handle_t nvs_handle = 0;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err == ESP_OK) {
    read_nvs_string_default(nvs_handle, "ap_name", "", ap_ssid,
                            sizeof(ap_ssid));
    read_nvs_string_default(nvs_handle, "ap_password", "", ap_password,
                            sizeof(ap_password));
    nvs_close(nvs_handle);
  } else {
    ESP_LOGW(TAG, "open nvs failed while loading ap config: %s",
             esp_err_to_name(err));
  }

  bool use_default_ap_pair = (ap_ssid[0] == '\0' && ap_password[0] == '\0');
  if (use_default_ap_pair || ap_ssid[0] == '\0') {
    copy_string_safely(ap_ssid, sizeof(ap_ssid), default_ssid);
  }
  if (use_default_ap_pair) {
    copy_string_safely(ap_password, sizeof(ap_password), DEFAULT_AP_PASSWORD);
  }

  wifi_config_t wifi_config = {0};
  copy_string_safely((char *)wifi_config.ap.ssid, sizeof(wifi_config.ap.ssid),
                     ap_ssid);
  wifi_config.ap.ssid_len = strlen((const char *)wifi_config.ap.ssid);
  copy_string_safely((char *)wifi_config.ap.password,
                     sizeof(wifi_config.ap.password), ap_password);
  wifi_config.ap.channel = DEFAULT_AP_CHANNEL;
  wifi_config.ap.max_connection = DEFAULT_AP_MAX_CONN;
  wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  if (wifi_config.ap.password[0] == '\0') {
    wifi_config.ap.authmode = WIFI_AUTH_OPEN;
  }

  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG,
                      "set WIFI_MODE_APSTA failed");
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG,
                      "set AP config failed");

  err = start_wifi_if_needed();
  ESP_RETURN_ON_ERROR(err, TAG, "esp_wifi_start failed");

  esp_netif_ip_info_t ip_info = {0};
  esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap_netif != NULL && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK) {
    ESP_LOGI(TAG, "Set up softAP with IP: " IPSTR, IP2STR(&ip_info.ip));
  } else {
    ESP_LOGI(TAG, "softAP started");
  }

  ESP_LOGI(TAG, "wifi_init_softap finished. SSID:'%s' security:%s",
           wifi_config.ap.ssid,
           wifi_config.ap.authmode == WIFI_AUTH_OPEN ? "open" : "protected");
  return ESP_OK;
}

esp_err_t wifi_init_sta(const char *w_ssid, const char *w_passwd) {
  if (w_ssid == NULL || w_ssid[0] == '\0') {
    ESP_LOGW(TAG, "wifi_ssid empty, skip STA connect");
    s_sta_connect_enabled = false;
    cancel_sta_retry();
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = ensure_wifi_driver_ready();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "ensure wifi driver failed: %s", esp_err_to_name(err));
    return err;
  }

  wifi_config_t wifi_config = {0};
  copy_string_safely((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid),
                     w_ssid);
  if (w_passwd != NULL) {
    copy_string_safely((char *)wifi_config.sta.password,
                       sizeof(wifi_config.sta.password), w_passwd);
  }
  wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
  if (w_passwd == NULL || w_passwd[0] == '\0') {
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
  }

  err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set WIFI_MODE_APSTA failed: %s", esp_err_to_name(err));
    return err;
  }

  err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set STA config failed: %s", esp_err_to_name(err));
    return err;
  }

  err = start_wifi_if_needed();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_start failed before STA connect: %s",
             esp_err_to_name(err));
    return err;
  }

  s_sta_connect_enabled = true;
  s_sta_retry_count = 0;
  cancel_sta_retry();
  err = esp_wifi_connect();
  if (err == ESP_ERR_WIFI_NOT_STARTED) {
    ESP_LOGW(TAG, "wifi not started during connect, retry after start");
    err = start_wifi_if_needed();
    if (err == ESP_OK) {
      err = esp_wifi_connect();
    }
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
    schedule_sta_retry();
    return err;
  }

  ESP_LOGI(TAG, "wifi_init_sta finished. ssid:%s", w_ssid);
  return ESP_OK;
}

static void wifi_task(void *pvParameters) {
  wifi_credentials_t *wifi_credentials = (wifi_credentials_t *)pvParameters;
  if (wifi_credentials != NULL) {
    (void)wifi_init_sta(wifi_credentials->ssid, wifi_credentials->password);
    free(wifi_credentials);
  }
  delete_self_app_task_with_caps();
}

void start_wifi_task(const char *ssid, const char *password) {
  wifi_credentials_t *wifi_credentials = calloc(1, sizeof(wifi_credentials_t));
  if (wifi_credentials == NULL) {
    ESP_LOGE(TAG, "alloc wifi task credentials failed");
    return;
  }

  if (ssid != NULL) {
    copy_string_safely(wifi_credentials->ssid, sizeof(wifi_credentials->ssid),
                       ssid);
  }
  if (password != NULL) {
    copy_string_safely(wifi_credentials->password,
                       sizeof(wifi_credentials->password), password);
  }

  BaseType_t ret = create_app_task_psram(wifi_task, "wifi_task",
                                         WIFI_TASK_STACK_SIZE,
                                         wifi_credentials, WIFI_TASK_PRIORITY,
                                         NULL, tskNO_AFFINITY);
  if (ret != pdPASS) {
    ESP_LOGE(TAG, "create wifi task failed");
    free(wifi_credentials);
  }
}

esp_netif_t *sx_wifi_get_ap_netif(void) { return s_ap_netif; }
esp_netif_t *sx_wifi_get_sta_netif(void) { return s_sta_netif; }

static esp_err_t configure_ap_ipv4(const char *ip_text, const char *mask_text,
                                   bool dhcp_enabled)
{
  if (s_ap_netif == NULL || ip_text == NULL || mask_text == NULL) return ESP_OK;
  esp_netif_ip_info_t info = {0};
  ip4_addr_t ip = {0};
  ip4_addr_t netmask = {0};
  if (!ip4addr_aton(ip_text, &ip) || !ip4addr_aton(mask_text, &netmask)) {
    return ESP_ERR_INVALID_ARG;
  }
  info.ip.addr = ip.addr;
  info.netmask.addr = netmask.addr;
  info.gw = info.ip;
  esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
  if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
  ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &info), TAG,
                      "set AP IPv4 failed");
  if (!dhcp_enabled) {
    err = esp_netif_dhcps_stop(s_ap_netif);
    return (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) ? ESP_OK : err;
  }
  err = esp_netif_dhcps_start(s_ap_netif);
  return err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ? ESP_OK : err;
}

static esp_err_t configure_sta_ipv4(bool sta_static, const char *ip_text,
                                    const char *mask_text, const char *gw_text,
                                    const char *dns_text)
{
  if (s_sta_netif == NULL) return ESP_ERR_INVALID_STATE;
  if (!sta_static) {
    esp_err_t err = esp_netif_dhcpc_start(s_sta_netif);
    return (err == ESP_OK || err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED)
               ? ESP_OK : err;
  }

  if (ip_text == NULL || mask_text == NULL || gw_text == NULL || dns_text == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  ip4_addr_t ip = {0};
  ip4_addr_t netmask = {0};
  ip4_addr_t gateway = {0};
  ip4_addr_t dns_addr = {0};
  if (!ip4addr_aton(ip_text, &ip) || !ip4addr_aton(mask_text, &netmask) ||
      !ip4addr_aton(gw_text, &gateway) || !ip4addr_aton(dns_text, &dns_addr)) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t err = esp_netif_dhcpc_stop(s_sta_netif);
  if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
  esp_netif_ip_info_t info = {0};
  info.ip.addr = ip.addr;
  info.netmask.addr = netmask.addr;
  info.gw.addr = gateway.addr;
  ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_sta_netif, &info), TAG,
                      "set STA IPv4 failed");
  esp_netif_dns_info_t dns = {0};
  dns.ip.u_addr.ip4.addr = dns_addr.addr;
  dns.ip.type = IPADDR_TYPE_V4;
  return esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns);
}

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
                                   bool ap_dhcp_enabled)
{
  if (!ap_enabled && !sta_enabled) return ESP_OK;
  ESP_RETURN_ON_ERROR(ensure_wifi_driver_ready(), TAG, "wifi driver init failed");

  if (ap_enabled) {
    ESP_RETURN_ON_ERROR(sx_wifi_init_softap_from_nvs(), TAG, "start AP failed");
    ESP_RETURN_ON_ERROR(configure_ap_ipv4(ap_ip, ap_netmask, ap_dhcp_enabled), TAG,
                        "configure AP network failed");
  }
  if (sta_enabled) {
    if (sta_ssid == NULL || sta_ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    ESP_RETURN_ON_ERROR(configure_sta_ipv4(sta_static, sta_ip, sta_netmask,
                                            sta_gateway, sta_dns), TAG,
                        "configure STA network failed");
    ESP_RETURN_ON_ERROR(wifi_init_sta(sta_ssid, sta_password), TAG,
                        "configure STA failed");
  }

  wifi_mode_t configured_mode = ap_enabled && sta_enabled ? WIFI_MODE_APSTA
                              : ap_enabled ? WIFI_MODE_AP
                              : WIFI_MODE_STA;
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(configured_mode), TAG,
                      "apply configured Wi-Fi mode failed");
  /* Disable modem sleep so AP forwarding and STA uplink keep low latency. */
  esp_err_t ps_err = esp_wifi_set_ps(WIFI_PS_NONE);
  if (ps_err != ESP_OK) {
    ESP_LOGW(TAG, "disable Wi-Fi power save failed: %s", esp_err_to_name(ps_err));
  }
  if (sta_enabled) {
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
      schedule_sta_retry();
      return err;
    }
  }
  ESP_LOGI(TAG, "SP603 Wi-Fi configured: AP=%d STA=%d", ap_enabled, sta_enabled);
  return ESP_OK;
}

esp_err_t sx_wifi_stop_ap(bool keep_sta_running)
{
  if (!s_wifi_started) return ESP_OK;

  if (keep_sta_running) {
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG,
                        "disable AP while keeping STA failed");
    ESP_LOGI(TAG, "Wi-Fi AP stopped; STA remains active");
    return ESP_OK;
  }

  esp_err_t err = esp_wifi_stop();
  if (err == ESP_OK || err == ESP_ERR_WIFI_NOT_STARTED) {
    s_wifi_started = false;
    ESP_LOGI(TAG, "Wi-Fi AP stopped");
    return ESP_OK;
  }
  return err;
}
