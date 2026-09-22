#include "sx_web_server.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_eth.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_memory_utils.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_ap_get_sta_list.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "async_uart.h"
#include "modem_manager.h"
#include "sx_auto_collect.h"
#include "sx_init_wifi.h"
#include "sx_led_manager.h"
#include "sx_time_manager.h"
#include "sx_work_mode.h"
#include "sx_network_manager.h"
#include "sx_serial_port_manager.h"
#include "sx_serial_resource.h"
#include "sx_serial_server.h"
#include "sx_storage_defaults.h"
#include "w5500_manager.h"

#define TAG "WEB"
#define MAX_WS_CLIENTS 8
#define MAX_HTTP_BODY_LEN 8192
#define DEFAULT_REPLY_TIMEOUT_MS 500
#define HW_SERIAL_PORT_COUNT 3
#define HW_AUTO_COLLECT_MASTER_PORT 2
#define HW_FEATURE_MODBUS_FILTER 0
#define HW_FEATURE_SLAVE_MAPPING 0
#define AUTH_SESSION_TOKEN_BYTES 16
#define AUTH_SESSION_TIMEOUT_US (30LL * 60LL * 1000000LL)
#define MAX_PROTECTED_ROUTES 64
#define SYSTEM_LOG_CAPACITY 64

_Static_assert(AC_MAX_ITEMS_PER_PORT <= 100,
               "auto-collect NVS item keys exceed the 15-character limit");

#define NVS_NAMESPACE SX_NVS_NAMESPACE
#define NVS_SCHEMA_KEY SX_NVS_SCHEMA_KEY
#define NVS_SCHEMA_VERSION SX_NVS_SCHEMA_VERSION
#define NVS_HW_PROFILE_KEY SX_NVS_HW_PROFILE_KEY
#define NVS_HW_PROFILE_VALUE SX_NVS_HW_PROFILE_VALUE

static const char *s_supported_work_modes[] = {
    "serial_server",
    "auto_collect",
};

extern const uint8_t web_html_gz_start[] asm("_binary_web_html_gz_start");
extern const uint8_t web_html_gz_end[] asm("_binary_web_html_gz_end");
extern const uint8_t root_html_gz_start[] asm("_binary_root_html_gz_start");
extern const uint8_t root_html_gz_end[] asm("_binary_root_html_gz_end");
extern const uint8_t web_js_gz_start[] asm("_binary_web_js_gz_start");
extern const uint8_t web_js_gz_end[] asm("_binary_web_js_gz_end");
extern const uint8_t web_css_gz_start[] asm("_binary_web_css_gz_start");
extern const uint8_t web_css_gz_end[] asm("_binary_web_css_gz_end");

static httpd_handle_t s_http_server = NULL;
static char s_session_token[AUTH_SESSION_TOKEN_BYTES * 2 + 1];
static int64_t s_session_last_seen_us;
static portMUX_TYPE s_auth_lock = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    esp_err_t (*handler)(httpd_req_t *req);
    void *user_ctx;
    bool is_websocket;
} protected_route_context_t;

static protected_route_context_t s_protected_routes[MAX_PROTECTED_ROUTES];
static size_t s_protected_route_count;
#if CONFIG_HTTPD_WS_SUPPORT
typedef struct {
    bool used;
    int fd;
} ws_client_t;

static ws_client_t s_ws_clients[MAX_WS_CLIENTS] = {0};
static portMUX_TYPE s_ws_spinlock = portMUX_INITIALIZER_UNLOCKED;
#endif

typedef struct {
    int64_t timestamp_us;
    char level[8];
    char source[24];
    char text[192];
} system_log_entry_t;

static system_log_entry_t s_system_logs[SYSTEM_LOG_CAPACITY];
static size_t s_system_log_count;
static size_t s_system_log_next;
static portMUX_TYPE s_system_log_spinlock = portMUX_INITIALIZER_UNLOCKED;

static bool port_supported_for_hw(int port)
{
    return (port >= 1 && port <= HW_SERIAL_PORT_COUNT);
}

static bool parse_port_from_query(httpd_req_t *req, int *out_port)
{
    char query[64] = {0};
    char value[16] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    if (httpd_query_key_value(query, "port", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    int port = atoi(value);
    if (port < 1 || port > HW_SERIAL_PORT_COUNT) {
        return false;
    }
    *out_port = port;
    return true;
}

static esp_err_t ensure_string_default(nvs_handle_t nvs, const char *key, const char *value)
{
    size_t len = 0;
    esp_err_t err = nvs_get_str(nvs, key, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return nvs_set_str(nvs, key, value);
    }
    return (err == ESP_OK) ? ESP_OK : err;
}

static bool parse_valid_serial_mode(const char *mode)
{
    if (mode == NULL) {
        return false;
    }
    return strcmp(mode, "separate") == 0 ||
           strcmp(mode, "unified") == 0 ||
           strcmp(mode, "slave_follow") == 0;
}

static const char *default_work_mode(void)
{
    return s_supported_work_modes[0];
}

static bool is_supported_work_mode(const char *mode)
{
    if (mode == NULL) {
        return false;
    }

    for (size_t i = 0; i < sizeof(s_supported_work_modes) / sizeof(s_supported_work_modes[0]); i++) {
        if (strcmp(mode, s_supported_work_modes[i]) == 0) {
            return sx_work_mode_is_valid(mode);
        }
    }
    return false;
}

static void add_firmware_capabilities_json(cJSON *root)
{
    if (root == NULL) {
        return;
    }

    cJSON *modes = cJSON_CreateArray();
    for (size_t i = 0; i < sizeof(s_supported_work_modes) / sizeof(s_supported_work_modes[0]); i++) {
        if (sx_work_mode_is_valid(s_supported_work_modes[i]))
            cJSON_AddItemToArray(modes, cJSON_CreateString(s_supported_work_modes[i]));
    }
    cJSON_AddItemToObject(root, "supported_work_modes", modes);

    cJSON *ports = cJSON_CreateArray();
    for (int port = 1; port <= HW_SERIAL_PORT_COUNT; port++) {
        cJSON_AddItemToArray(ports, cJSON_CreateNumber(port));
    }
    cJSON_AddItemToObject(root, "supported_serial_ports", ports);

    cJSON *available_ports = cJSON_CreateArray();
    for (int port = 1; port <= HW_SERIAL_PORT_COUNT; port++) {
        if (sx_serial_port_manager_port_available(port))
            cJSON_AddItemToArray(available_ports, cJSON_CreateNumber(port));
    }
    cJSON_AddItemToObject(root, "available_serial_ports", available_ports);

    cJSON *port_info = cJSON_CreateArray();
    sx_serial_port_capability_t capabilities[4];
    size_t count = sx_serial_port_manager_get_capabilities(capabilities, 4);
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "id", capabilities[i].id);
        cJSON_AddStringToObject(item, "label", capabilities[i].label);
        cJSON_AddNumberToObject(item, "port", capabilities[i].port);
        cJSON_AddBoolToObject(item, "present", capabilities[i].present);
        cJSON_AddBoolToObject(item, "available", capabilities[i].available);
        cJSON_AddStringToObject(item, "reserved_by", capabilities[i].reserved_by);
        cJSON_AddItemToArray(port_info, item);
    }
    cJSON_AddItemToObject(root, "serial_ports", port_info);

    cJSON_AddBoolToObject(root, "feature_modbus_filter", HW_FEATURE_MODBUS_FILTER != 0);
    cJSON_AddBoolToObject(root, "feature_slave_mapping", HW_FEATURE_SLAVE_MAPPING != 0);
}

static esp_err_t ensure_storage_schema(nvs_handle_t nvs)
{
    char serial_mode[32] = {0};
    char serial_sync_mode[32] = {0};
    size_t len = sizeof(serial_mode);

    esp_err_t err = nvs_get_str(nvs, "serial_mode", serial_mode, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        size_t sync_len = sizeof(serial_sync_mode);
        err = nvs_get_str(nvs, "serialSyncMode", serial_sync_mode, &sync_len);
        if (err == ESP_OK && parse_valid_serial_mode(serial_sync_mode)) {
            ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "serial_mode", serial_sync_mode), TAG,
                                "copy serial_mode from serialSyncMode failed");
            snprintf(serial_mode, sizeof(serial_mode), "%s", serial_sync_mode);
        } else {
            ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "serial_mode", "separate"), TAG,
                                "set serial_mode default failed");
            snprintf(serial_mode, sizeof(serial_mode), "separate");
        }
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "read serial_mode failed");
    }

    if (!parse_valid_serial_mode(serial_mode)) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "serial_mode", "separate"), TAG,
                            "normalize serial_mode failed");
        snprintf(serial_mode, sizeof(serial_mode), "separate");
    }

    len = sizeof(serial_sync_mode);
    err = nvs_get_str(nvs, "serialSyncMode", serial_sync_mode, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND || !parse_valid_serial_mode(serial_sync_mode)) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "serialSyncMode", serial_mode), TAG,
                            "sync serialSyncMode failed");
    } else {
        ESP_RETURN_ON_ERROR(err, TAG, "read serialSyncMode failed");
    }

    char work_mode[32] = {0};
    len = sizeof(work_mode);
    err = nvs_get_str(nvs, "w_mode", work_mode, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "w_mode", default_work_mode()), TAG,
                            "set default w_mode failed");
    } else if (err != ESP_OK) {
        ESP_RETURN_ON_ERROR(err, TAG, "read w_mode failed");
    } else if (!is_supported_work_mode(work_mode)) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, "w_mode", default_work_mode()), TAG,
                            "normalize w_mode failed");
    }

    ESP_RETURN_ON_ERROR(ensure_string_default(nvs, NVS_SCHEMA_KEY, NVS_SCHEMA_VERSION), TAG,
                        "set schema version failed");
    ESP_RETURN_ON_ERROR(ensure_string_default(nvs, NVS_HW_PROFILE_KEY, NVS_HW_PROFILE_VALUE), TAG,
                        "set hw profile failed");

    return ESP_OK;
}

static const char *nvs_get_string_or_default(nvs_handle_t nvs,
                                             const char *key,
                                             const char *def,
                                             char *buf,
                                             size_t buf_size)
{
    if (!buf || buf_size == 0) {
        return def;
    }

    size_t len = buf_size;
    esp_err_t err = nvs_get_str(nvs, key, buf, &len);
    if (err == ESP_OK) {
        return buf;
    }

    if (def) {
        snprintf(buf, buf_size, "%s", def);
    } else {
        buf[0] = '\0';
    }
    return buf;
}

static esp_err_t read_http_body(httpd_req_t *req, char **out_body, size_t *out_len)
{
    if (req == NULL || out_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *out_body = NULL;
    if (out_len != NULL) {
        *out_len = 0;
    }

    if (req->content_len <= 0 || req->content_len > MAX_HTTP_BODY_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t total = (size_t)req->content_len;
    char *body = (char *)calloc(1, total + 1);
    if (body == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < total) {
        int ret = httpd_req_recv(req, body + received, total - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    body[total] = '\0';
    *out_body = body;
    if (out_len != NULL) {
        *out_len = total;
    }
    return ESP_OK;
}

static void http_json_reply(httpd_req_t *req, cJSON *json)
{
    char *resp = cJSON_PrintUnformatted(json);
    httpd_resp_set_type(req, "application/json");
    if (resp) {
        httpd_resp_sendstr(req, resp);
        free(resp);
    } else {
        httpd_resp_sendstr(req, "{}");
    }
}

static void http_reply_code_msg(httpd_req_t *req, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", code);
    cJSON_AddStringToObject(root, "msg", msg ? msg : "");
    http_json_reply(req, root);
    cJSON_Delete(root);
}

static bool request_has_valid_session(httpd_req_t *req)
{
    size_t cookie_len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (cookie_len == 0 || cookie_len > 1024) return false;
    char *cookie = calloc(1, cookie_len + 1);
    if (cookie == NULL) return false;
    bool valid = false;
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie, cookie_len + 1) == ESP_OK) {
        const char *key = "SP603_SESSION=";
        char *value = strstr(cookie, key);
        if (value != NULL) {
            value += strlen(key);
            size_t token_len = strcspn(value, "; ");
            int64_t now = esp_timer_get_time();
            portENTER_CRITICAL(&s_auth_lock);
            valid = s_session_token[0] != '\0' &&
                    token_len == strlen(s_session_token) &&
                    memcmp(value, s_session_token, token_len) == 0 &&
                    now - s_session_last_seen_us <= AUTH_SESSION_TIMEOUT_US;
            if (valid) s_session_last_seen_us = now;
            portEXIT_CRITICAL(&s_auth_lock);
        }
    }
    free(cookie);
    return valid;
}

static esp_err_t redirect_to(httpd_req_t *req, const char *location)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t protected_route_handler(httpd_req_t *req)
{
    protected_route_context_t *context = req->user_ctx;
    /* WebSocket data frames have no HTTP headers. Reaching this path means the
     * connection already passed the authenticated upgrade handler. */
    bool authenticated_ws_frame = context != NULL && context->is_websocket && req->uri[0] == '\0';
    if (!authenticated_ws_frame && !request_has_valid_session(req)) {
        if (context != NULL && context->is_websocket) {
            ESP_LOGW(TAG, "rejecting unauthenticated WebSocket connection");
            return ESP_FAIL;
        }
        if (strcmp(req->uri, "/status") == 0 || strcmp(req->uri, "/web.html") == 0) {
            return redirect_to(req, "/");
        }
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"code\":401,\"msg\":\"authentication required\"}");
    }
    if (context == NULL || context->handler == NULL) return ESP_FAIL;
    req->user_ctx = context->user_ctx;
    return context->handler(req);
}

static esp_err_t register_protected_route(httpd_handle_t server, const httpd_uri_t *uri)
{
    if (s_protected_route_count >= MAX_PROTECTED_ROUTES) {
        ESP_LOGE(TAG, "protected route table full: %s", uri->uri);
        return ESP_ERR_NO_MEM;
    }
    protected_route_context_t *context = &s_protected_routes[s_protected_route_count];
    context->handler = uri->handler;
    context->user_ctx = uri->user_ctx;
#if CONFIG_HTTPD_WS_SUPPORT
    context->is_websocket = uri->is_websocket;
#else
    context->is_websocket = false;
#endif
    httpd_uri_t protected_uri = *uri;
    protected_uri.handler = protected_route_handler;
    protected_uri.user_ctx = context;
    esp_err_t err = httpd_register_uri_handler(server, &protected_uri);
    if (err != ESP_OK) {
        memset(context, 0, sizeof(*context));
        ESP_LOGE(TAG, "failed to register protected route %s: %s",
                 uri->uri, esp_err_to_name(err));
        return err;
    }
    s_protected_route_count++;
    return ESP_OK;
}

static void http_reply_feature_not_supported(httpd_req_t *req, const char *feature)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", 410);
    cJSON_AddStringToObject(root, "msg", "feature not supported on current hardware profile");
    cJSON_AddStringToObject(root, "feature", feature ? feature : "");
    http_json_reply(req, root);
    cJSON_Delete(root);
}

#if CONFIG_HTTPD_WS_SUPPORT
static void ws_add_client(int fd)
{
    portENTER_CRITICAL(&s_ws_spinlock);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i].used && s_ws_clients[i].fd == fd) {
            portEXIT_CRITICAL(&s_ws_spinlock);
            return;
        }
    }

    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (!s_ws_clients[i].used) {
            s_ws_clients[i].used = true;
            s_ws_clients[i].fd = fd;
            break;
        }
    }
    portEXIT_CRITICAL(&s_ws_spinlock);
}

static void ws_remove_client(int fd)
{
    portENTER_CRITICAL(&s_ws_spinlock);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i].used && s_ws_clients[i].fd == fd) {
            s_ws_clients[i].used = false;
            s_ws_clients[i].fd = -1;
        }
    }
    portEXIT_CRITICAL(&s_ws_spinlock);
}

static void ws_broadcast_text(const char *text)
{
    if (s_http_server == NULL || text == NULL) {
        return;
    }

    int fds[MAX_WS_CLIENTS];
    int fd_count = 0;

    portENTER_CRITICAL(&s_ws_spinlock);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_clients[i].used) {
            fds[fd_count++] = s_ws_clients[i].fd;
        }
    }
    portEXIT_CRITICAL(&s_ws_spinlock);

    httpd_ws_frame_t pkt = {
        .final = true,
        .fragmented = false,
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)text,
        .len = strlen(text),
    };

    for (int i = 0; i < fd_count; i++) {
        esp_err_t err = httpd_ws_send_frame_async(s_http_server, fds[i], &pkt);
        if (err != ESP_OK) {
            ws_remove_client(fds[i]);
        }
    }
}
#else
static void ws_broadcast_text(const char *text)
{
    (void)text;
}
#endif

void send_system_log(const char *level, const char *source, const char *text)
{
    system_log_entry_t entry = {0};
    entry.timestamp_us = time_manager_get_current_us();
    snprintf(entry.level, sizeof(entry.level), "%s", level ? level : "INFO");
    snprintf(entry.source, sizeof(entry.source), "%s", source ? source : "system");
    snprintf(entry.text, sizeof(entry.text), "%s", text ? text : "");

    portENTER_CRITICAL(&s_system_log_spinlock);
    s_system_logs[s_system_log_next] = entry;
    s_system_log_next = (s_system_log_next + 1U) % SYSTEM_LOG_CAPACITY;
    if (s_system_log_count < SYSTEM_LOG_CAPACITY) ++s_system_log_count;
    portEXIT_CRITICAL(&s_system_log_spinlock);

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) return;
    cJSON_AddStringToObject(root, "type", "system_log");
    cJSON_AddNumberToObject(root, "timestamp", (double)entry.timestamp_us);
    cJSON_AddStringToObject(root, "level", entry.level);
    cJSON_AddStringToObject(root, "source", entry.source);
    cJSON_AddStringToObject(root, "text", entry.text);
    char *payload = cJSON_PrintUnformatted(root);
    if (payload != NULL) {
        ws_broadcast_text(payload);
        free(payload);
    }
    cJSON_Delete(root);
}

static int runtime_port_from_port(int port)
{
    if (port < 1 || port > HW_SERIAL_PORT_COUNT) {
        return -1;
    }
    return port;
}

static void serial_port_nvs_key(char *key, size_t key_size, int port,
                                const char *field)
{
    snprintf(key, key_size, "%s_%s", sx_serial_port_manager_port_key(port), field);
}

static uart_word_length_t parse_data_bits(const char *data_bits)
{
    int value = atoi(data_bits ? data_bits : "8");
    switch (value) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_parity_t parse_parity(const char *parity)
{
    int value = atoi(parity ? parity : "0");
    switch (value) {
    case 1:
        return UART_PARITY_ODD;
    case 2:
        return UART_PARITY_EVEN;
    default:
        return UART_PARITY_DISABLE;
    }
}

static uart_stop_bits_t parse_stop_bits(const char *stop_bits)
{
    if (stop_bits == NULL) {
        return UART_STOP_BITS_1;
    }
    if (strcmp(stop_bits, "2") == 0) {
        return UART_STOP_BITS_2;
    }
    if (strcmp(stop_bits, "1.5") == 0) {
        return UART_STOP_BITS_1_5;
    }
    return UART_STOP_BITS_1;
}

static esp_err_t ensure_serial_port_defaults(nvs_handle_t nvs, int port)
{
    char key[32];
    size_t len = 0;

    serial_port_nvs_key(key, sizeof(key), port, "baud");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "9600"), TAG, "set %s failed", key);
    }

    serial_port_nvs_key(key, sizeof(key), port, "data");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "8"), TAG, "set %s failed", key);
    }

    serial_port_nvs_key(key, sizeof(key), port, "parity");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "0"), TAG, "set %s failed", key);
    }

    serial_port_nvs_key(key, sizeof(key), port, "stop");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "1"), TAG, "set %s failed", key);
    }

    serial_port_nvs_key(key, sizeof(key), port, "ftime");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "50"), TAG, "set %s failed", key);
    }

    serial_port_nvs_key(key, sizeof(key), port, "flen");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "512"), TAG, "set %s failed", key);
    }

    serial_port_nvs_key(key, sizeof(key), port, "timeout");
    if (nvs_get_str(nvs, key, NULL, &len) == ESP_ERR_NVS_NOT_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_set_str(nvs, key, "500"), TAG, "set %s failed", key);
    }

    return ESP_OK;
}

static esp_err_t ensure_auto_collect_defaults(nvs_handle_t nvs)
{
    for (int i = 1; i <= AC_COLLECT_PORT_COUNT; i++) {
        char key[32];
        uint8_t maddr = 1;
        int32_t count = 1;

        snprintf(key, sizeof(key), "ac%d_maddr", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 1), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_count", i);
        if (nvs_get_i32(nvs, key, &count) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_i32(nvs, key, 1), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_en", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 1), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_rs", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 1), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_fc", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 3), TAG, "set %s failed", key);
        }

        uint16_t u16 = 0;
        uint32_t u32 = 0;

        snprintf(key, sizeof(key), "ac%d_item0_ra", i);
        if (nvs_get_u16(nvs, key, &u16) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u16(nvs, key, 0), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_mra", i);
        if (nvs_get_u16(nvs, key, &u16) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u16(nvs, key, 0), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_rn", i);
        if (nvs_get_u16(nvs, key, &u16) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u16(nvs, key, 1), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_int", i);
        if (nvs_get_u32(nvs, key, &u32) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u32(nvs, key, 100), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_to", i);
        if (nvs_get_u32(nvs, key, &u32) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u32(nvs, key, 1000), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_baud", i);
        if (nvs_get_u32(nvs, key, &u32) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u32(nvs, key, 9600), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_db", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 8), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_par", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 0), TAG, "set %s failed", key);
        }

        snprintf(key, sizeof(key), "ac%d_item0_sb", i);
        if (nvs_get_u8(nvs, key, &maddr) == ESP_ERR_NVS_NOT_FOUND) {
            ESP_RETURN_ON_ERROR(nvs_set_u8(nvs, key, 1), TAG, "set %s failed", key);
        }
    }

    return ESP_OK;
}

esp_err_t sx_web_server_init_storage_defaults(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    for (int port = 1; port <= HW_SERIAL_PORT_COUNT && err == ESP_OK; port++) {
        err = ensure_serial_port_defaults(nvs, port);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ensure serial defaults failed for port %d: %s",
                     port, esp_err_to_name(err));
        }
    }
    if (err == ESP_OK) err = ensure_storage_schema(nvs);
    if (err == ESP_OK) err = ensure_auto_collect_defaults(nvs);

    // Remove the valid legacy filter key when present. The former
    // mapping_cfg_json key was 16 characters and could never exist in NVS.
    if (err == ESP_OK) {
        esp_err_t erase_err = nvs_erase_key(nvs, "filter_cfg_json");
        if (erase_err != ESP_OK && erase_err != ESP_ERR_NVS_NOT_FOUND) err = erase_err;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static esp_err_t send_gzip_asset(httpd_req_t *req,
                                 const uint8_t *start,
                                 const uint8_t *end,
                                 const char *content_type,
                                 const char *cache_control)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    httpd_resp_set_hdr(req, "Cache-Control", cache_control);
    httpd_resp_set_hdr(req, "Connection", "close");
    return httpd_resp_send(req, (const char *)start, (ssize_t)(end - start));
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (request_has_valid_session(req)) return redirect_to(req, "/status");
    return send_gzip_asset(req, root_html_gz_start, root_html_gz_end,
                           "text/html; charset=utf-8", "no-store");
}

static esp_err_t web_html_get_handler(httpd_req_t *req)
{
    return send_gzip_asset(req, web_html_gz_start, web_html_gz_end,
                           "text/html; charset=utf-8", "no-store");
}

static esp_err_t root_html_get_handler(httpd_req_t *req)
{
    return send_gzip_asset(req, root_html_gz_start, root_html_gz_end,
                           "text/html; charset=utf-8", "no-store");
}

static esp_err_t js_get_handler(httpd_req_t *req)
{
    return send_gzip_asset(req, web_js_gz_start, web_js_gz_end,
                           "application/javascript; charset=utf-8",
                           "no-store");
}

static esp_err_t css_get_handler(httpd_req_t *req)
{
    return send_gzip_asset(req, web_css_gz_start, web_css_gz_end,
                           "text/css; charset=utf-8",
                           "no-store");
}

static esp_err_t get_devinfo_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char host_name[64];
        char dhcp_mode[8];
        cJSON_AddStringToObject(root, "host_names",
                                nvs_get_string_or_default(nvs, "host_names", "SP603-多串口物联网网关", host_name,
                                                          sizeof(host_name)));
        cJSON_AddStringToObject(root, "is_dhcp",
                                nvs_get_string_or_default(nvs, "is_dhcp", "1", dhcp_mode,
                                                          sizeof(dhcp_mode)));
        nvs_close(nvs);
    } else {
        cJSON_AddStringToObject(root, "host_names", "SP603-多串口物联网网关");
        cJSON_AddStringToObject(root, "is_dhcp", "1");
    }

    cJSON_AddNumberToObject(root, "uptime", (double)esp_timer_get_time());

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[24];
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    cJSON_AddStringToObject(root, "device_sta_mac", mac_str);

    esp_netif_ip_info_t ip = {0};
    bool ip_found = false;

    esp_netif_t *eth = sx_wifi_get_sta_netif();
    if (eth && esp_netif_get_ip_info(eth, &ip) == ESP_OK && ip.ip.addr != 0) {
        ip_found = true;
    }

    if (!ip_found) {
        esp_netif_t *ppp = esp_netif_get_handle_from_ifkey("PPP_DEF");
        if (ppp && esp_netif_get_ip_info(ppp, &ip) == ESP_OK && ip.ip.addr != 0) {
            ip_found = true;
        }
    }

    char ip_str[24] = "0.0.0.0";
    if (ip_found) {
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip.ip));
    }
    cJSON_AddStringToObject(root, "sta_ip", ip_str);

    /* Expose each configured interface separately.  The old page showed a
     * single "STA IP" field, which was misleading on a multi-network unit. */
    sx_network_status_t net_status = {0};
    sx_network_config_t net_config = {0};
    sx_network_manager_get_status(&net_status);
    sx_network_manager_get_config(&net_config);
    cJSON_AddStringToObject(root, "wifi_sta_ip",
                            net_status.wifi_ip[0] ? net_status.wifi_ip
                                                  : (net_config.wifi_sta_enabled && net_config.wifi_sta_static
                                                         ? net_config.wifi_sta_ip : "0.0.0.0"));
    cJSON_AddStringToObject(root, "wifi_ap_ip",
                            net_config.wifi_ap_enabled ? net_config.ap_ip : "0.0.0.0");
    cJSON_AddStringToObject(root, "ethernet_ip",
                            net_status.ethernet_ip[0] ? net_status.ethernet_ip
                                                       : (net_config.ethernet_enabled && net_config.ethernet_static
                                                              ? net_config.ethernet_lan_ip : "0.0.0.0"));
    cJSON_AddStringToObject(root, "modem_ip",
                            net_status.modem_ip[0] ? net_status.modem_ip : "0.0.0.0");
    cJSON_AddStringToObject(root, "active_interface",
                            sx_network_interface_name(net_status.active_interface));
    cJSON_AddStringToObject(root, "gateway", net_status.gateway);
    cJSON_AddStringToObject(root, "netmask", net_status.netmask);
    cJSON_AddStringToObject(root, "active_dns",
                            net_status.dns_ready ? net_status.active_dns : "0.0.0.0");
    cJSON_AddBoolToObject(root, "dns_ready", net_status.dns_ready);
    cJSON_AddNumberToObject(root, "uplink_generation", net_status.uplink_generation);

    const char *address_mode = "--";
    bool active_link = false;
    esp_netif_t *active_netif = NULL;
    char active_identifier[24] = "--";
    if (net_status.active_interface == SX_NETWORK_IF_W5500) {
        address_mode = net_config.ethernet_static ? "static" : "dhcp";
        active_link = net_status.w5500_got_ip && net_status.dns_ready;
        active_netif = w5500_manager_get_netif();
        uint8_t eth_mac[6] = {0};
        esp_eth_handle_t eth_handle = w5500_manager_get_handle();
        if (eth_handle != NULL &&
            esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, eth_mac) == ESP_OK) {
            snprintf(active_identifier, sizeof(active_identifier),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     eth_mac[0], eth_mac[1], eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
        }
    } else if (net_status.active_interface == SX_NETWORK_IF_WIFI) {
        address_mode = net_config.wifi_sta_static ? "static" : "dhcp";
        active_link = net_status.wifi_got_ip && net_status.dns_ready;
        active_netif = sx_wifi_get_sta_netif();
        snprintf(active_identifier, sizeof(active_identifier), "%s", mac_str);
    } else if (net_status.active_interface == SX_NETWORK_IF_4G) {
        address_mode = "ppp";
        active_link = net_status.modem_got_ip && net_status.dns_ready;
        active_netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
    }
    char dns_text[16] = "0.0.0.0";
    esp_netif_dns_info_t dns_info = {0};
    if (active_netif != NULL &&
        esp_netif_get_dns_info(active_netif, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        dns_info.ip.type == ESP_IPADDR_TYPE_V4) {
        snprintf(dns_text, sizeof(dns_text), IPSTR,
                 IP2STR(&dns_info.ip.u_addr.ip4));
    }
    cJSON_AddBoolToObject(root, "active_link", active_link);
    cJSON_AddStringToObject(root, "active_ip", net_status.ip);
    cJSON_AddStringToObject(root, "active_dns", dns_text);
    cJSON_AddStringToObject(root, "active_address_mode", address_mode);
    cJSON_AddStringToObject(root, "active_identifier", active_identifier);

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_sys_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    cJSON_AddStringToObject(root, "version", VERSION);
    cJSON_AddNumberToObject(root, "free_heap_internal_kb", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
    cJSON_AddNumberToObject(root, "min_heap_internal_kb", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024.0);
    cJSON_AddNumberToObject(root, "free_heap_psram_kb", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024.0);
    cJSON_AddNumberToObject(root, "min_heap_psram_kb", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024.0);
    cJSON_AddNumberToObject(root, "time_since_boot", (double)esp_timer_get_time());

    static configRUN_TIME_COUNTER_TYPE previous_time = 0;
    static configRUN_TIME_COUNTER_TYPE previous_idle[2] = {0};
    configRUN_TIME_COUNTER_TYPE now = (configRUN_TIME_COUNTER_TYPE)esp_timer_get_time();
    configRUN_TIME_COUNTER_TYPE elapsed = now - previous_time;
    for (BaseType_t core = 0; core < 2; ++core) {
        configRUN_TIME_COUNTER_TYPE idle = ulTaskGetIdleRunTimeCounterForCore(core);
        configRUN_TIME_COUNTER_TYPE idle_elapsed = idle - previous_idle[core];
        double usage = 0.0;
        if (elapsed > 0) {
            double idle_percent = ((double)idle_elapsed * 100.0) / (double)elapsed;
            if (idle_percent > 100.0) idle_percent = 100.0;
            usage = 100.0 - idle_percent;
        }
        cJSON_AddNumberToObject(root,
                                core == 0 ? "cpu0_usage_percent" : "cpu1_usage_percent",
                                usage);
        previous_idle[core] = idle;
    }
    previous_time = now;

    esp_reset_reason_t reason = esp_reset_reason();
    cJSON_AddNumberToObject(root, "res_reason", reason);

    int socket_count = 0;
    const struct tcp_pcb *pcb = tcp_active_pcbs;
    while (pcb != NULL) {
        socket_count++;
        pcb = pcb->next;
    }
    cJSON_AddNumberToObject(root, "active_sockets", socket_count);

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t read_json_request(httpd_req_t *req, cJSON **out_json)
{
    char *body = NULL;
    size_t body_len = 0;
    esp_err_t err = read_http_body(req, &body, &body_len);
    if (err != ESP_OK) {
        return err;
    }

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        return ESP_FAIL;
    }

    *out_json = json;
    return ESP_OK;
}

static void create_session_token(char token[AUTH_SESSION_TOKEN_BYTES * 2 + 1])
{
    uint8_t random_bytes[AUTH_SESSION_TOKEN_BYTES];
    esp_fill_random(random_bytes, sizeof(random_bytes));
    for (size_t i = 0; i < sizeof(random_bytes); ++i) {
        snprintf(token + i * 2, 3, "%02x", random_bytes[i]);
    }
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "invalid request");
    }
    cJSON *username = cJSON_GetObjectItemCaseSensitive(json, "username");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "password");
    bool valid = false;
    if (cJSON_IsString(username) && cJSON_IsString(password) &&
        username->valuestring != NULL && password->valuestring != NULL) {
        char stored_username[32] = "admin";
        char stored_password[64] = "12345678";
        nvs_handle_t nvs = 0;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
            nvs_get_string_or_default(nvs, "lgname", "admin", stored_username,
                                      sizeof(stored_username));
            nvs_get_string_or_default(nvs, "lgpwd", "12345678", stored_password,
                                      sizeof(stored_password));
            nvs_close(nvs);
        }
        valid = strcmp(username->valuestring, stored_username) == 0 &&
                strcmp(password->valuestring, stored_password) == 0;
        memset(stored_password, 0, sizeof(stored_password));
    }
    cJSON_Delete(json);
    if (!valid) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_sendstr(req, "invalid credentials");
    }

    char token[AUTH_SESSION_TOKEN_BYTES * 2 + 1];
    create_session_token(token);
    portENTER_CRITICAL(&s_auth_lock);
    snprintf(s_session_token, sizeof(s_session_token), "%s", token);
    s_session_last_seen_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_auth_lock);

    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "SP603_SESSION=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800",
             token);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "success");
}

static esp_err_t logout_post_handler(httpd_req_t *req)
{
    portENTER_CRITICAL(&s_auth_lock);
    memset(s_session_token, 0, sizeof(s_session_token));
    s_session_last_seen_us = 0;
    portEXIT_CRITICAL(&s_auth_lock);
    httpd_resp_set_hdr(req, "Set-Cookie",
                       "SP603_SESSION=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, "success");
}

static void json_string_or_number_to_buf(cJSON *obj,
                                         const char *key,
                                         char *out,
                                         size_t out_size,
                                         const char *def)
{
    if (!out || out_size == 0) {
        return;
    }

    if (def != NULL) {
        snprintf(out, out_size, "%s", def);
    } else {
        out[0] = '\0';
    }

    if (!obj || !key) {
        return;
    }

    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        snprintf(out, out_size, "%s", item->valuestring);
    } else if (cJSON_IsNumber(item)) {
        snprintf(out, out_size, "%.0f", item->valuedouble);
    } else if (cJSON_IsBool(item)) {
        snprintf(out, out_size, "%d", cJSON_IsTrue(item) ? 1 : 0);
    }
}

static bool json_contains_key(cJSON *obj, const char *key)
{
  if (obj == NULL || key == NULL) {
    return false;
  }
  return cJSON_GetObjectItemCaseSensitive(obj, key) != NULL;
}

static esp_err_t save_flat_json_object_to_nvs(cJSON *json)
{
  if (json == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  nvs_handle_t nvs_handle = 0;
  esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
  if (err != ESP_OK) {
    return err;
  }

  cJSON *item = NULL;
  cJSON_ArrayForEach(item, json) {
    if (!item->string) {
      continue;
    }
    if (strlen(item->string) >= NVS_KEY_NAME_MAX_SIZE) {
      ESP_LOGE(TAG, "reject overlength NVS key '%s' (%u > %u)", item->string,
               (unsigned)strlen(item->string), (unsigned)(NVS_KEY_NAME_MAX_SIZE - 1));
      nvs_close(nvs_handle);
      return ESP_ERR_NVS_KEY_TOO_LONG;
    }

    if (cJSON_IsString(item) && item->valuestring) {
      err = nvs_set_str(nvs_handle, item->string, item->valuestring);
    } else if (cJSON_IsNumber(item)) {
      char num_buf[32];
      snprintf(num_buf, sizeof(num_buf), "%.0f", item->valuedouble);
      err = nvs_set_str(nvs_handle, item->string, num_buf);
    } else if (cJSON_IsBool(item)) {
      err = nvs_set_str(nvs_handle, item->string, cJSON_IsTrue(item) ? "1" : "0");
    } else {
      continue;
    }

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "save key '%s' failed: %s", item->string,
               esp_err_to_name(err));
      nvs_close(nvs_handle);
      return err;
    }
  }

  err = nvs_commit(nvs_handle);
  nvs_close(nvs_handle);
  return err;
}

static esp_err_t save_flat_json_to_nvs(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    esp_err_t err = save_flat_json_object_to_nvs(json);
    cJSON_Delete(json);

    if (err != ESP_OK) {
        char message[80];
        snprintf(message, sizeof(message), "nvs save failed: %s", esp_err_to_name(err));
        http_reply_code_msg(req, 500, message);
        return ESP_OK;
    }

    http_reply_code_msg(req, 200, "success");
    return ESP_OK;
}

static esp_err_t get_module_set_post_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }
    cJSON *host = cJSON_GetObjectItemCaseSensitive(json, "host_names");
    cJSON *username = cJSON_GetObjectItemCaseSensitive(json, "lgname");
    cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "lgpwd");
    if (!cJSON_IsString(host) || !cJSON_IsString(username) ||
        host->valuestring == NULL || username->valuestring == NULL ||
        host->valuestring[0] == '\0' || username->valuestring[0] == '\0' ||
        strlen(host->valuestring) >= 64 || strlen(username->valuestring) >= 32 ||
        (cJSON_IsString(password) && password->valuestring != NULL &&
         password->valuestring[0] != '\0' && strlen(password->valuestring) < 8)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "invalid device name, username or password");
        return ESP_OK;
    }
    nvs_handle_t nvs = 0;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_str(nvs, "host_names", host->valuestring);
    if (err == ESP_OK) err = nvs_set_str(nvs, "lgname", username->valuestring);
    if (err == ESP_OK && cJSON_IsString(password) && password->valuestring != NULL &&
        password->valuestring[0] != '\0') {
        err = nvs_set_str(nvs, "lgpwd", password->valuestring);
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs != 0) nvs_close(nvs);
    cJSON_Delete(json);
    if (err != ESP_OK) {
        http_reply_code_msg(req, 500, "NVS save failed");
        return ESP_OK;
    }
    http_reply_code_msg(req, 200, "success");
    return ESP_OK;
}

static esp_err_t get_net_set_post_handler(httpd_req_t *req)
{
  cJSON *json = NULL;
  if (read_json_request(req, &json) != ESP_OK) {
    http_reply_code_msg(req, 400, "invalid json body");
    return ESP_OK;
  }

  bool update_sta = json_contains_key(json, "wifi_ssid") ||
                    json_contains_key(json, "wifi_password");
  esp_err_t err = save_flat_json_object_to_nvs(json);
  cJSON_Delete(json);
  if (err != ESP_OK) {
    char message[80];
    snprintf(message, sizeof(message), "nvs save failed: %s", esp_err_to_name(err));
    http_reply_code_msg(req, 500, message);
    return ESP_OK;
  }

  if (update_sta) {
    ESP_LOGI(TAG, "legacy net_set updated STA credentials; network manager applies them after reboot");
  }

  http_reply_code_msg(req, 200, "success");
  return ESP_OK;
}

static esp_err_t get_ap_set_post_handler(httpd_req_t *req)
{
  cJSON *json = NULL;
  if (read_json_request(req, &json) != ESP_OK) {
    http_reply_code_msg(req, 400, "invalid json body");
    return ESP_OK;
  }

  bool update_ap = json_contains_key(json, "ap_name") ||
                   json_contains_key(json, "ap_password") ||
                   json_contains_key(json, "ap_wait_time") ||
                   json_contains_key(json, "ap_timeout");

  esp_err_t err = save_flat_json_object_to_nvs(json);
  cJSON_Delete(json);
  if (err != ESP_OK) {
    http_reply_code_msg(req, 500, "nvs save failed");
    return ESP_OK;
  }

  if (update_ap) {
    ESP_LOGI(TAG, "legacy ap_set updated AP config; network manager applies it after reboot");
  }

  http_reply_code_msg(req, 200, "success");
  return ESP_OK;
}

static esp_err_t get_module_set_info_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char host_names[64];
        char lgname[32];
        cJSON_AddStringToObject(root, "host_names",
                                nvs_get_string_or_default(nvs, "host_names", "SP603-多串口物联网网关", host_names,
                                                          sizeof(host_names)));
        cJSON_AddStringToObject(root, "lgname",
                                nvs_get_string_or_default(nvs, "lgname", "admin", lgname,
                                                          sizeof(lgname)));
        cJSON_AddBoolToObject(root, "password_configured", true);
        nvs_close(nvs);
    }

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_net_set_info_get_handler(httpd_req_t *req)
{
  cJSON *root = cJSON_CreateObject();
  nvs_handle_t nvs_handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
    char value[64];
    cJSON_AddStringToObject(root, "netconn",
                            nvs_get_string_or_default(nvs_handle, "netconn",
                                                      "2", value, sizeof(value)));
    cJSON_AddStringToObject(root, "is_dhcp",
                            nvs_get_string_or_default(nvs_handle, "is_dhcp",
                                                      "1", value, sizeof(value)));
    cJSON_AddStringToObject(root, "static_ip",
                            nvs_get_string_or_default(nvs_handle, "static_ip",
                                                      "", value, sizeof(value)));
    cJSON_AddStringToObject(root, "static_netmask",
                            nvs_get_string_or_default(nvs_handle, "static_netmask",
                                                      "", value, sizeof(value)));
    cJSON_AddStringToObject(root, "static_gateway",
                            nvs_get_string_or_default(nvs_handle, "static_gateway",
                                                      "", value, sizeof(value)));
    cJSON_AddStringToObject(root, "static_dns1",
                            nvs_get_string_or_default(nvs_handle, "static_dns1",
                                                      "8.8.8.8", value, sizeof(value)));
    cJSON_AddStringToObject(root, "static_dns2",
                            nvs_get_string_or_default(nvs_handle, "static_dns2",
                                                      "114.114.114.114", value, sizeof(value)));
    cJSON_AddStringToObject(root, "wifi_ssid",
                            nvs_get_string_or_default(nvs_handle, "wifi_ssid",
                                                      "", value, sizeof(value)));
    cJSON_AddStringToObject(root, "wifi_password",
                            nvs_get_string_or_default(nvs_handle, "wifi_password",
                                                      "", value, sizeof(value)));
    nvs_close(nvs_handle);
  }

  http_json_reply(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t get_ap_set_info_get_handler(httpd_req_t *req)
{
  cJSON *root = cJSON_CreateObject();
  nvs_handle_t nvs_handle;
  if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle) == ESP_OK) {
    char value[64];
    cJSON_AddStringToObject(root, "ap_name",
                            nvs_get_string_or_default(nvs_handle, "ap_name",
                                                      "", value, sizeof(value)));
    cJSON_AddStringToObject(root, "ap_password",
                            nvs_get_string_or_default(nvs_handle, "ap_password",
                                                      "", value, sizeof(value)));
    cJSON_AddStringToObject(root, "ap_wait_time",
                            nvs_get_string_or_default(nvs_handle, "ap_wait_time",
                                                      "10", value, sizeof(value)));
    cJSON_AddStringToObject(root, "ap_timeout",
                            nvs_get_string_or_default(nvs_handle, "ap_timeout",
                                                      "30", value, sizeof(value)));
    nvs_close(nvs_handle);
  } else {
    cJSON_AddStringToObject(root, "ap_name", "");
    cJSON_AddStringToObject(root, "ap_password", "");
    cJSON_AddStringToObject(root, "ap_wait_time", "10");
    cJSON_AddStringToObject(root, "ap_timeout", "30");
  }

  http_json_reply(req, root);
  cJSON_Delete(root);
  return ESP_OK;
}

static esp_err_t get_serial_config_mode_info_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char mode[32];
        cJSON_AddStringToObject(root, "config_mode",
                                nvs_get_string_or_default(nvs, "serial_mode", "separate", mode, sizeof(mode)));
        nvs_close(nvs);
    } else {
        cJSON_AddStringToObject(root, "config_mode", "separate");
    }

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_serial_config_mode_set_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    cJSON *mode = cJSON_GetObjectItem(json, "config_mode");
    if (!cJSON_IsString(mode) || mode->valuestring == NULL) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "invalid config_mode");
        return ESP_OK;
    }

    const char *mode_str = mode->valuestring;
    if (strcmp(mode_str, "unified") != 0 &&
        strcmp(mode_str, "separate") != 0 &&
        strcmp(mode_str, "slave_follow") != 0) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "unsupported config mode");
        return ESP_OK;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, "nvs open failed");
        return ESP_OK;
    }

    esp_err_t save_err = nvs_set_str(nvs, "serial_mode", mode_str);
    if (save_err == ESP_OK) save_err = nvs_set_str(nvs, "serialSyncMode", mode_str);
    if (save_err == ESP_OK) save_err = nvs_commit(nvs);
    nvs_close(nvs);
    if (save_err != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), "serial config mode save failed: %s",
                 esp_err_to_name(save_err));
        ESP_LOGE(TAG, "%s", message);
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, message);
        return ESP_OK;
    }

    if (strcmp(mode_str, "slave_follow") == 0) {
        set_uart_config_mode(UART_CONFIG_MODE_SLAVE_FOLLOW);
    } else {
        set_uart_config_mode(UART_CONFIG_MODE_NORMAL);
    }

    cJSON_Delete(json);
    http_reply_code_msg(req, 200, "success");
    return ESP_OK;
}

static esp_err_t get_serial_set_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    cJSON *serial_port = cJSON_GetObjectItem(json, "serial_port");
    int port = 1;
    bool has_port_field = (serial_port != NULL);
    if (cJSON_IsString(serial_port) && serial_port->valuestring) {
        char *end_ptr = NULL;
        long parsed = strtol(serial_port->valuestring, &end_ptr, 10);
        if (end_ptr == serial_port->valuestring || (end_ptr && *end_ptr != '\0')) {
            cJSON_Delete(json);
            http_reply_code_msg(req, 400, "invalid serial_port");
            return ESP_OK;
        }
        port = (int)parsed;
    } else if (cJSON_IsNumber(serial_port)) {
        port = (int)serial_port->valuedouble;
    } else if (has_port_field) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "invalid serial_port");
        return ESP_OK;
    }

    if (!port_supported_for_hw(port)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "serial_port not supported by current hardware");
        return ESP_OK;
    }
    if (!sx_serial_port_manager_port_available(port)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 409, "serial port is reserved by the active SP603 hardware layout");
        return ESP_OK;
    }

    char baud_str[16] = "9600";
    char data_bit_str[8] = "8";
    char check_bit_str[8] = "0";
    char stop_bit_str[8] = "1";
    char frame_time_str[16] = "50";
    char frame_len_str[16] = "512";
    char timeout_str[16] = "500";

    json_string_or_number_to_buf(json, "baud_rate", baud_str, sizeof(baud_str), "9600");
    json_string_or_number_to_buf(json, "data_bit", data_bit_str, sizeof(data_bit_str), "8");
    json_string_or_number_to_buf(json, "check_bit", check_bit_str, sizeof(check_bit_str), "0");
    json_string_or_number_to_buf(json, "stop_bit", stop_bit_str, sizeof(stop_bit_str), "1");
    json_string_or_number_to_buf(json, "frame_time", frame_time_str, sizeof(frame_time_str), "50");
    json_string_or_number_to_buf(json, "frame_len", frame_len_str, sizeof(frame_len_str), "512");
    json_string_or_number_to_buf(json, "reply_timeout", timeout_str, sizeof(timeout_str), "500");

    sx_serial_port_config_t tcp_cfg;
    sx_serial_server_get_config(port, &tcp_cfg);
    const int previous_baud_rate = tcp_cfg.baud_rate;
    const int previous_data_bit = tcp_cfg.data_bit;
    const int previous_check_bit = tcp_cfg.check_bit;
    const int previous_stop_bit = tcp_cfg.stop_bit;
    const int previous_frame_time = tcp_cfg.frame_time;
    const int previous_frame_len = tcp_cfg.frame_len;
    const int previous_timeout = tcp_cfg.timeout;
    cJSON *protocol = cJSON_GetObjectItemCaseSensitive(json, "protocol");
    if (protocol == NULL) protocol = cJSON_GetObjectItemCaseSensitive(json, "tcp_mode");
    if (cJSON_IsString(protocol) && protocol->valuestring &&
        !sx_serial_protocol_from_name(protocol->valuestring, &tcp_cfg.tcp_mode)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "invalid serial network protocol");
        return ESP_OK;
    }
    char remote_ip[64] = {0};
    cJSON *remote_ip_item = cJSON_GetObjectItem(json, "remote_ip");
    if (cJSON_IsString(remote_ip_item) && remote_ip_item->valuestring) snprintf(remote_ip, sizeof(remote_ip), "%s", remote_ip_item->valuestring);
    if (remote_ip[0]) snprintf(tcp_cfg.remote_ip, sizeof(tcp_cfg.remote_ip), "%s", remote_ip);
    cJSON *local_port_item = cJSON_GetObjectItem(json, "local_port");
    cJSON *remote_port_item = cJSON_GetObjectItem(json, "remote_port");
    if (cJSON_IsNumber(local_port_item)) tcp_cfg.local_port = (uint16_t)local_port_item->valuedouble;
    if (cJSON_IsNumber(remote_port_item)) tcp_cfg.remote_port = (uint16_t)remote_port_item->valuedouble;
#define UPDATE_SERIAL_TEXT(json_key, field, keep_empty) do { \
        cJSON *item = cJSON_GetObjectItemCaseSensitive(json, json_key); \
        if (cJSON_IsString(item) && item->valuestring != NULL && \
            ((keep_empty) || item->valuestring[0] != '\0')) \
            snprintf(tcp_cfg.field, sizeof(tcp_cfg.field), "%s", item->valuestring); \
    } while (0)
    UPDATE_SERIAL_TEXT("mqtt_uri", mqtt_uri, true);
    UPDATE_SERIAL_TEXT("mqtt_username", mqtt_username, true);
    UPDATE_SERIAL_TEXT("mqtt_password", mqtt_password, false);
    UPDATE_SERIAL_TEXT("mqtt_client_id", mqtt_client_id, true);
    UPDATE_SERIAL_TEXT("mqtt_publish_topic", mqtt_publish_topic, true);
    UPDATE_SERIAL_TEXT("mqtt_subscribe_topic", mqtt_subscribe_topic, true);
#undef UPDATE_SERIAL_TEXT
    cJSON *mqtt_qos = cJSON_GetObjectItemCaseSensitive(json, "mqtt_qos");
    cJSON *mqtt_retain = cJSON_GetObjectItemCaseSensitive(json, "mqtt_retain");
    if (cJSON_IsNumber(mqtt_qos)) tcp_cfg.mqtt_qos = (uint8_t)mqtt_qos->valueint;
    if (cJSON_IsBool(mqtt_retain)) tcp_cfg.mqtt_retain = cJSON_IsTrue(mqtt_retain);
    tcp_cfg.port = port; tcp_cfg.baud_rate = atoi(baud_str); tcp_cfg.data_bit = atoi(data_bit_str);
    tcp_cfg.check_bit = atoi(check_bit_str);
    tcp_cfg.stop_bit = strcmp(stop_bit_str, "1.5") == 0 ? 15 : atoi(stop_bit_str);
    tcp_cfg.frame_time = atoi(frame_time_str); tcp_cfg.frame_len = atoi(frame_len_str); tcp_cfg.timeout = atoi(timeout_str);
    esp_err_t save_err = sx_serial_server_save_config(&tcp_cfg);
    if (save_err != ESP_OK) {
        cJSON_Delete(json);
        http_reply_code_msg(req, save_err == ESP_ERR_INVALID_ARG ? 400 : 500,
                            save_err == ESP_ERR_INVALID_ARG
                                ? "invalid serial configuration"
                                : "serial configuration save failed");
        return ESP_OK;
    }

    int runtime_port = runtime_port_from_port(port);
    bool uart_changed = previous_baud_rate != tcp_cfg.baud_rate ||
                        previous_data_bit != tcp_cfg.data_bit ||
                        previous_check_bit != tcp_cfg.check_bit ||
                        previous_stop_bit != tcp_cfg.stop_bit ||
                        previous_frame_time != tcp_cfg.frame_time ||
                        previous_frame_len != tcp_cfg.frame_len ||
                        previous_timeout != tcp_cfg.timeout;
    if (uart_changed && port_supported_for_hw(runtime_port)) {
        serial_port_config_t cfg = {
            .port = runtime_port,
            .baudrate = atoi(baud_str),
            .data_bits = parse_data_bits(data_bit_str),
            .parity = parse_parity(check_bit_str),
            .stop_bits = parse_stop_bits(stop_bit_str),
            .frame_time = atoi(frame_time_str),
            .frame_len = atoi(frame_len_str),
            .timeout = atoi(timeout_str),
        };
        if (cfg.timeout <= 0) {
            cfg.timeout = DEFAULT_REPLY_TIMEOUT_MS;
        }
        esp_err_t runtime_err = quick_reconfigure_port(runtime_port, &cfg);
        if (runtime_err != ESP_OK) {
            cJSON_Delete(json);
            http_reply_code_msg(req, 500, "serial runtime reconfigure failed");
            return ESP_OK;
        }
        set_current_runtime_config(runtime_port, &cfg);
    }

    cJSON_Delete(json);
    http_reply_code_msg(req, 200, "success");
    return ESP_OK;
}

static esp_err_t get_serial_set_info_get_handler(httpd_req_t *req)
{
    int port = 1;
    char query[64] = {0};
    bool has_query = (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK);
    if (has_query) {
        if (!parse_port_from_query(req, &port)) {
            http_reply_code_msg(req, 400, "invalid or unsupported port");
            return ESP_OK;
        }
    }

    cJSON *root = cJSON_CreateObject();
    sx_serial_port_config_t tcp_cfg;
    sx_serial_server_get_config(port, &tcp_cfg);
    cJSON_AddStringToObject(root, "tcp_mode", tcp_cfg.tcp_mode == SX_SERIAL_TCP_CLIENT ? "client" : "server");
    cJSON_AddStringToObject(root, "protocol", sx_serial_protocol_name(tcp_cfg.tcp_mode));
    cJSON_AddNumberToObject(root, "local_port", tcp_cfg.local_port);
    cJSON_AddStringToObject(root, "remote_ip", tcp_cfg.remote_ip);
    cJSON_AddNumberToObject(root, "remote_port", tcp_cfg.remote_port);
    cJSON_AddStringToObject(root, "mqtt_uri", tcp_cfg.mqtt_uri);
    cJSON_AddStringToObject(root, "mqtt_username", tcp_cfg.mqtt_username);
    cJSON_AddStringToObject(root, "mqtt_password", "");
    cJSON_AddBoolToObject(root, "mqtt_password_configured", tcp_cfg.mqtt_password[0] != '\0');
    cJSON_AddStringToObject(root, "mqtt_client_id", tcp_cfg.mqtt_client_id);
    cJSON_AddStringToObject(root, "mqtt_publish_topic", tcp_cfg.mqtt_publish_topic);
    cJSON_AddStringToObject(root, "mqtt_subscribe_topic", tcp_cfg.mqtt_subscribe_topic);
    cJSON_AddNumberToObject(root, "mqtt_qos", tcp_cfg.mqtt_qos);
    cJSON_AddBoolToObject(root, "mqtt_retain", tcp_cfg.mqtt_retain);

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char key[32];
        char value[24];

        serial_port_nvs_key(key, sizeof(key), port, "baud");
        cJSON_AddStringToObject(root, "baud_rate",
                                nvs_get_string_or_default(nvs, key, "9600", value, sizeof(value)));

        serial_port_nvs_key(key, sizeof(key), port, "data");
        cJSON_AddStringToObject(root, "data_bit",
                                nvs_get_string_or_default(nvs, key, "8", value, sizeof(value)));

        serial_port_nvs_key(key, sizeof(key), port, "parity");
        cJSON_AddStringToObject(root, "check_bit",
                                nvs_get_string_or_default(nvs, key, "0", value, sizeof(value)));

        serial_port_nvs_key(key, sizeof(key), port, "stop");
        cJSON_AddStringToObject(root, "stop_bit",
                                nvs_get_string_or_default(nvs, key, "1", value, sizeof(value)));

        serial_port_nvs_key(key, sizeof(key), port, "ftime");
        cJSON_AddStringToObject(root, "frame_time",
                                nvs_get_string_or_default(nvs, key, "50", value, sizeof(value)));

        serial_port_nvs_key(key, sizeof(key), port, "flen");
        cJSON_AddStringToObject(root, "frame_len",
                                nvs_get_string_or_default(nvs, key, "512", value, sizeof(value)));

        serial_port_nvs_key(key, sizeof(key), port, "timeout");
        cJSON_AddStringToObject(root, "reply_timeout",
                                nvs_get_string_or_default(nvs, key, "500", value, sizeof(value)));

        nvs_close(nvs);
    }

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static const char *serial_runtime_state_text(sx_serial_runtime_state_t state)
{
    switch (state) {
    case SX_SERIAL_RUNTIME_CONNECTING: return "连接中";
    case SX_SERIAL_RUNTIME_LISTENING: return "监听中";
    case SX_SERIAL_RUNTIME_CONNECTED: return "已连接";
    case SX_SERIAL_RUNTIME_ERROR: return "错误";
    default: return "未启动";
    }
}

static const char *serial_uplink_text(const char *uplink)
{
    if (strcmp(uplink, "ethernet") == 0) return "Ethernet";
    if (strcmp(uplink, "wifi_sta") == 0) return "Wi-Fi 客户端";
    if (strcmp(uplink, "4g") == 0) return "4G 蜂窝网络";
    return "暂无可用上联";
}

static esp_err_t get_serial_runtime_status_get_handler(httpd_req_t *req)
{
    sx_network_status_t network = {0};
    sx_network_manager_get_status(&network);
    const char *uplink = sx_network_interface_name(network.active_interface);
    const char *uplink_text = serial_uplink_text(uplink);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active_uplink", uplink);
    cJSON *ports = cJSON_CreateArray();
    sx_serial_port_capability_t capabilities[4] = {0};
    size_t count = sx_serial_port_manager_get_capabilities(capabilities, 4);
    for (size_t i = 0; i < count; ++i) {
        int port = capabilities[i].port;
        sx_serial_runtime_status_t runtime = {0};
        sx_serial_server_get_runtime_status(port, &runtime);
        cJSON *item = cJSON_CreateObject();
        const char *label = sx_serial_port_manager_port_label(port);
        const char *protocol = sx_serial_protocol_name(runtime.protocol);
        char route[256];
        if (runtime.protocol == SX_SERIAL_TCP_SERVER) {
            snprintf(route, sizeof(route), "%s ↔ TCP Server :%u（监听全部网络接口）",
                     label, runtime.local_port);
        } else if (runtime.protocol == SX_SERIAL_TCP_CLIENT) {
            snprintf(route, sizeof(route), "%s ↔ TCP Client %s:%u，经 %s",
                     label, runtime.remote_ip[0] ? runtime.remote_ip : "--",
                     runtime.remote_port, uplink_text);
        } else if (runtime.protocol == SX_SERIAL_MQTT) {
            snprintf(route, sizeof(route), "%s ↔ MQTT %s，经 %s",
                     label, runtime.mqtt_uri[0] ? runtime.mqtt_uri : "--", uplink_text);
        } else {
            snprintf(route, sizeof(route), "%s ↔ Modbus TCP :%u ↔ Modbus RTU",
                     label, runtime.local_port);
        }
        cJSON_AddNumberToObject(item, "port", port);
        cJSON_AddStringToObject(item, "label", label);
        cJSON_AddStringToObject(item, "protocol", protocol);
        cJSON_AddNumberToObject(item, "state", runtime.state);
        cJSON_AddStringToObject(item, "state_text", serial_runtime_state_text(runtime.state));
        cJSON_AddStringToObject(item, "route", route);
        cJSON_AddStringToObject(item, "uplink", uplink_text);
        cJSON_AddBoolToObject(item, "listening", runtime.listening);
        cJSON_AddBoolToObject(item, "connected", runtime.connected || runtime.mqtt_connected);
        cJSON_AddItemToArray(ports, item);
    }
    cJSON_AddItemToObject(root, "ports", ports);
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_system_logs_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *logs = cJSON_CreateArray();
    if (root == NULL || logs == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(logs);
        return ESP_ERR_NO_MEM;
    }

    size_t count;
    size_t next;
    portENTER_CRITICAL(&s_system_log_spinlock);
    count = s_system_log_count;
    next = s_system_log_next;
    portEXIT_CRITICAL(&s_system_log_spinlock);

    for (size_t i = 0; i < count; ++i) {
        system_log_entry_t entry;
        size_t index = (next + SYSTEM_LOG_CAPACITY - count + i) % SYSTEM_LOG_CAPACITY;
        portENTER_CRITICAL(&s_system_log_spinlock);
        entry = s_system_logs[index];
        portEXIT_CRITICAL(&s_system_log_spinlock);

        cJSON *item = cJSON_CreateObject();
        if (item == NULL) break;
        cJSON_AddNumberToObject(item, "timestamp", (double)entry.timestamp_us);
        cJSON_AddStringToObject(item, "level", entry.level);
        cJSON_AddStringToObject(item, "source", entry.source);
        cJSON_AddStringToObject(item, "text", entry.text);
        cJSON_AddItemToArray(logs, item);
    }
    cJSON_AddItemToObject(root, "logs", logs);
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static bool parse_hex_text(const char *text, uint8_t *out, size_t out_size, size_t *out_len)
{
    if (!text || !out || !out_len) {
        return false;
    }

    size_t n = 0;
    size_t i = 0;
    size_t text_len = strlen(text);
    while (i < text_len) {
        while (i < text_len && isspace((unsigned char)text[i])) {
            i++;
        }
        if (i >= text_len) {
            break;
        }

        if (i + 1 >= text_len || !isxdigit((unsigned char)text[i]) || !isxdigit((unsigned char)text[i + 1])) {
            return false;
        }

        if (n >= out_size) {
            return false;
        }

        char tmp[3] = {text[i], text[i + 1], '\0'};
        out[n++] = (uint8_t)strtoul(tmp, NULL, 16);
        i += 2;
    }

    *out_len = n;
    return true;
}

static esp_err_t get_serial_ctl_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    const cJSON *instruction = cJSON_GetObjectItem(json, "instruction");
    const cJSON *send_type = cJSON_GetObjectItem(json, "sendType");
    const cJSON *port = cJSON_GetObjectItem(json, "port");

    if (!cJSON_IsString(instruction) || instruction->valuestring == NULL) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "invalid instruction");
        return ESP_OK;
    }

    int target_port = 1;
    if (cJSON_IsNumber(port)) {
        target_port = (int)port->valuedouble;
    }

    if (!port_supported_for_hw(target_port)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "port not supported by current hardware");
        return ESP_OK;
    }

    bool is_hex = cJSON_IsString(send_type) && send_type->valuestring && strcmp(send_type->valuestring, "hex") == 0;

    if (is_hex) {
        uint8_t bytes[512];
        size_t byte_len = 0;
        if (!parse_hex_text(instruction->valuestring, bytes, sizeof(bytes), &byte_len) || byte_len == 0) {
            cJSON_Delete(json);
            http_reply_code_msg(req, 400, "invalid hex payload");
            return ESP_OK;
        }
        tx_tasks_to_port(bytes, byte_len, target_port);
    } else {
        tx_tasks_to_port((uint8_t *)instruction->valuestring, strlen(instruction->valuestring), target_port);
    }

    cJSON_Delete(json);
    http_reply_code_msg(req, 200, "success");
    return ESP_OK;
}

static esp_err_t sync_time_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    cJSON *timestamp = cJSON_GetObjectItem(json, "timestamp");
    if (!cJSON_IsNumber(timestamp)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "missing timestamp");
        return ESP_OK;
    }

    uint64_t browser_ms = (uint64_t)timestamp->valuedouble;
    esp_err_t err = time_manager_sync_from_browser(browser_ms);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "success", err == ESP_OK);
    cJSON_AddNumberToObject(resp, "current_time", (double)time_manager_get_current_ms());
    cJSON_AddBoolToObject(resp, "synced", time_manager_is_synced());

    cJSON_Delete(json);
    http_json_reply(req, resp);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t ac_save_port_to_nvs(nvs_handle_t nvs, cJSON *port_obj, const char *prefix)
{
    if (!port_obj || !prefix) {
        return ESP_ERR_INVALID_ARG;
    }

    char key[32];
    esp_err_t write_err = ESP_OK;
#define AC_NVS_WRITE(call) do { \
        if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE) { \
            ESP_LOGE(TAG, "auto-collect NVS key too long: %s", key); \
            return ESP_ERR_NVS_KEY_TOO_LONG; \
        } \
        write_err = (call); \
        if (write_err != ESP_OK) { \
            ESP_LOGE(TAG, "auto-collect NVS write %s failed: %s", key, \
                     esp_err_to_name(write_err)); \
            return write_err; \
        } \
    } while (0)

    cJSON *mapped_slave_addr = cJSON_GetObjectItem(port_obj, "mapped_slave_addr");
    uint8_t maddr = (uint8_t)((cJSON_IsNumber(mapped_slave_addr) ? mapped_slave_addr->valuedouble : 1));
    if (maddr < 1 || maddr > 247) {
        maddr = 1;
    }

    snprintf(key, sizeof(key), "%s_maddr", prefix);
    AC_NVS_WRITE(nvs_set_u8(nvs, key, maddr));

    cJSON *items = cJSON_GetObjectItem(port_obj, "items");
    int count = (cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0);
    if (count < 0) {
        count = 0;
    }
    if (count > AC_MAX_ITEMS_PER_PORT) {
        count = AC_MAX_ITEMS_PER_PORT;
    }

    snprintf(key, sizeof(key), "%s_count", prefix);
    AC_NVS_WRITE(nvs_set_i32(nvs, key, count));

    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(items, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }

        cJSON *v = NULL;

        snprintf(key, sizeof(key), "%s_item%d_en", prefix, i);
        v = cJSON_GetObjectItem(item, "enabled");
        AC_NVS_WRITE(nvs_set_u8(nvs, key, cJSON_IsBool(v) ? (cJSON_IsTrue(v) ? 1 : 0) : 1));

        snprintf(key, sizeof(key), "%s_item%d_rs", prefix, i);
        v = cJSON_GetObjectItem(item, "real_slave_addr");
        AC_NVS_WRITE(nvs_set_u8(nvs, key, (uint8_t)(cJSON_IsNumber(v) ? v->valuedouble : 1)));

        snprintf(key, sizeof(key), "%s_item%d_fc", prefix, i);
        v = cJSON_GetObjectItem(item, "function_code");
        AC_NVS_WRITE(nvs_set_u8(nvs, key, (uint8_t)(cJSON_IsNumber(v) ? v->valuedouble : 3)));

        snprintf(key, sizeof(key), "%s_item%d_ra", prefix, i);
        v = cJSON_GetObjectItem(item, "register_addr");
        AC_NVS_WRITE(nvs_set_u16(nvs, key, (uint16_t)(cJSON_IsNumber(v) ? v->valuedouble : 0)));

        snprintf(key, sizeof(key), "%s_item%d_mra", prefix, i);
        v = cJSON_GetObjectItem(item, "mapped_register_addr");
        AC_NVS_WRITE(nvs_set_u16(nvs, key, (uint16_t)(cJSON_IsNumber(v) ? v->valuedouble : 0)));

        snprintf(key, sizeof(key), "%s_item%d_rn", prefix, i);
        v = cJSON_GetObjectItem(item, "register_num");
        AC_NVS_WRITE(nvs_set_u16(nvs, key, (uint16_t)(cJSON_IsNumber(v) ? v->valuedouble : 1)));

        snprintf(key, sizeof(key), "%s_item%d_int", prefix, i);
        v = cJSON_GetObjectItem(item, "interval_ms");
        AC_NVS_WRITE(nvs_set_u32(nvs, key, (uint32_t)(cJSON_IsNumber(v) ? v->valuedouble : 100)));

        snprintf(key, sizeof(key), "%s_item%d_to", prefix, i);
        v = cJSON_GetObjectItem(item, "timeout_ms");
        AC_NVS_WRITE(nvs_set_u32(nvs, key, (uint32_t)(cJSON_IsNumber(v) ? v->valuedouble : 1000)));

        snprintf(key, sizeof(key), "%s_item%d_baud", prefix, i);
        v = cJSON_GetObjectItem(item, "baudrate");
        AC_NVS_WRITE(nvs_set_u32(nvs, key, (uint32_t)(cJSON_IsNumber(v) ? v->valuedouble : 9600)));

        snprintf(key, sizeof(key), "%s_item%d_db", prefix, i);
        v = cJSON_GetObjectItem(item, "data_bits");
        AC_NVS_WRITE(nvs_set_u8(nvs, key, (uint8_t)(cJSON_IsNumber(v) ? v->valuedouble : 8)));

        snprintf(key, sizeof(key), "%s_item%d_par", prefix, i);
        v = cJSON_GetObjectItem(item, "parity");
        AC_NVS_WRITE(nvs_set_u8(nvs, key, (uint8_t)(cJSON_IsNumber(v) ? v->valuedouble : 0)));

        snprintf(key, sizeof(key), "%s_item%d_sb", prefix, i);
        v = cJSON_GetObjectItem(item, "stop_bits");
        AC_NVS_WRITE(nvs_set_u8(nvs, key, (uint8_t)(cJSON_IsNumber(v) ? v->valuedouble : 1)));
    }

#undef AC_NVS_WRITE
    return ESP_OK;
}

static void ac_add_port_from_nvs(cJSON *response, nvs_handle_t nvs, const char *port_key, const char *prefix)
{
    cJSON *port = cJSON_CreateObject();
    uint8_t maddr = 1;
    int32_t count = 0;
    char key[32];

    snprintf(key, sizeof(key), "%s_maddr", prefix);
    if (nvs_get_u8(nvs, key, &maddr) != ESP_OK) {
        maddr = 1;
    }
    cJSON_AddNumberToObject(port, "mapped_slave_addr", maddr);

    snprintf(key, sizeof(key), "%s_count", prefix);
    if (nvs_get_i32(nvs, key, &count) != ESP_OK || count < 0) {
        count = 0;
    }
    if (count > AC_MAX_ITEMS_PER_PORT) {
        count = AC_MAX_ITEMS_PER_PORT;
    }

    cJSON *items = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *it = cJSON_CreateObject();
        uint8_t u8 = 0;
        uint16_t u16 = 0;
        uint32_t u32 = 0;

        snprintf(key, sizeof(key), "%s_item%d_en", prefix, i);
        nvs_get_u8(nvs, key, &u8);
        cJSON_AddBoolToObject(it, "enabled", u8 != 0);

        snprintf(key, sizeof(key), "%s_item%d_rs", prefix, i);
        nvs_get_u8(nvs, key, &u8);
        cJSON_AddNumberToObject(it, "real_slave_addr", u8);

        snprintf(key, sizeof(key), "%s_item%d_fc", prefix, i);
        nvs_get_u8(nvs, key, &u8);
        cJSON_AddNumberToObject(it, "function_code", u8);

        snprintf(key, sizeof(key), "%s_item%d_ra", prefix, i);
        nvs_get_u16(nvs, key, &u16);
        cJSON_AddNumberToObject(it, "register_addr", u16);

        snprintf(key, sizeof(key), "%s_item%d_mra", prefix, i);
        nvs_get_u16(nvs, key, &u16);
        cJSON_AddNumberToObject(it, "mapped_register_addr", u16);

        snprintf(key, sizeof(key), "%s_item%d_rn", prefix, i);
        nvs_get_u16(nvs, key, &u16);
        cJSON_AddNumberToObject(it, "register_num", u16);

        snprintf(key, sizeof(key), "%s_item%d_int", prefix, i);
        nvs_get_u32(nvs, key, &u32);
        cJSON_AddNumberToObject(it, "interval_ms", u32);

        snprintf(key, sizeof(key), "%s_item%d_to", prefix, i);
        nvs_get_u32(nvs, key, &u32);
        cJSON_AddNumberToObject(it, "timeout_ms", u32);

        snprintf(key, sizeof(key), "%s_item%d_baud", prefix, i);
        nvs_get_u32(nvs, key, &u32);
        cJSON_AddNumberToObject(it, "baudrate", u32);

        snprintf(key, sizeof(key), "%s_item%d_db", prefix, i);
        nvs_get_u8(nvs, key, &u8);
        cJSON_AddNumberToObject(it, "data_bits", u8);

        snprintf(key, sizeof(key), "%s_item%d_par", prefix, i);
        nvs_get_u8(nvs, key, &u8);
        cJSON_AddNumberToObject(it, "parity", u8);

        snprintf(key, sizeof(key), "%s_item%d_sb", prefix, i);
        nvs_get_u8(nvs, key, &u8);
        cJSON_AddNumberToObject(it, "stop_bits", u8);

        cJSON_AddItemToArray(items, it);
    }

    cJSON_AddItemToObject(port, "items", items);
    cJSON_AddItemToObject(response, port_key, port);
}

static const char *network_interface_state(bool enabled,
                                           bool started,
                                           bool link_up,
                                           bool got_ip,
                                           bool address_required)
{
    if (!enabled) return "disabled";
    if (!started) return "error";
    if (got_ip) return "online";
    if (link_up) return address_required ? "acquiring_ip" : "connected";
    return "waiting";
}

static cJSON *add_network_interface(cJSON *interfaces,
                                    const char *name,
                                    bool enabled,
                                    bool started,
                                    sx_network_role_t role,
                                    bool link_up,
                                    bool got_ip,
                                    bool address_required,
                                    const char *ip)
{
    cJSON *interface = cJSON_CreateObject();
    cJSON_AddBoolToObject(interface, "enabled", enabled);
    cJSON_AddBoolToObject(interface, "started", started);
    cJSON_AddStringToObject(interface, "role", sx_network_role_name(role));
    cJSON_AddStringToObject(interface, "state",
                            network_interface_state(enabled, started, link_up,
                                                    got_ip, address_required));
    cJSON_AddBoolToObject(interface, "link_up", link_up);
    cJSON_AddBoolToObject(interface, "got_ip", got_ip);
    cJSON_AddStringToObject(interface, "ip", ip != NULL ? ip : "");
    cJSON_AddItemToObject(interfaces, name, interface);
    return interface;
}

#define NETWORK_CLIENT_LIMIT 8

typedef struct {
    ip4_addr_t ip;
    uint8_t mac[6];
} ethernet_client_t;

typedef struct {
    int netif_index;
    size_t count;
    ethernet_client_t clients[NETWORK_CLIENT_LIMIT];
} ethernet_client_snapshot_t;

static void collect_ethernet_clients_on_tcpip(void *arg)
{
    ethernet_client_snapshot_t *snapshot = (ethernet_client_snapshot_t *)arg;
    for (size_t i = 0; i < ARP_TABLE_SIZE && snapshot->count < NETWORK_CLIENT_LIMIT; ++i) {
        ip4_addr_t *ip = NULL;
        struct netif *netif = NULL;
        struct eth_addr *mac = NULL;
        if (!etharp_get_entry(i, &ip, &netif, &mac) || ip == NULL || netif == NULL || mac == NULL ||
            netif_get_index(netif) != snapshot->netif_index) {
            continue;
        }
        ethernet_client_t *client = &snapshot->clients[snapshot->count++];
        client->ip = *ip;
        memcpy(client->mac, mac->addr, sizeof(client->mac));
    }
}

static void add_client_json(cJSON *clients, const char *interface_name,
                            const uint8_t mac[6], const ip4_addr_t *ip,
                            const char *state, int rssi)
{
    char mac_text[18];
    char ip_text[16] = "--";
    snprintf(mac_text, sizeof(mac_text), MACSTR, MAC2STR(mac));
    if (ip != NULL && ip->addr != 0) ip4addr_ntoa_r(ip, ip_text, sizeof(ip_text));

    cJSON *client = cJSON_CreateObject();
    cJSON_AddStringToObject(client, "interface", interface_name);
    cJSON_AddStringToObject(client, "ip", ip_text);
    cJSON_AddStringToObject(client, "mac", mac_text);
    cJSON_AddStringToObject(client, "state", state);
    if (rssi != INT_MIN) cJSON_AddNumberToObject(client, "rssi", rssi);
    cJSON_AddItemToArray(clients, client);
}

static size_t add_downstream_clients(cJSON *clients, const sx_network_config_t *config)
{
    if (config->wifi_ap_enabled && config->wifi_ap_role == SX_NETWORK_ROLE_DOWNLINK) {
        wifi_sta_list_t stations = {0};
        wifi_sta_mac_ip_list_t addresses = {0};
        if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK) return 0;
        bool have_addresses = esp_wifi_ap_get_sta_list_with_ip(&stations, &addresses) == ESP_OK;
        size_t count = (size_t)stations.num;
        if (count > NETWORK_CLIENT_LIMIT) count = NETWORK_CLIENT_LIMIT;
        for (size_t i = 0; i < count; ++i) {
            ip4_addr_t ip = {0};
            const ip4_addr_t *ip_ptr = NULL;
            if (have_addresses) {
                ip.addr = addresses.sta[i].ip.addr;
                ip_ptr = &ip;
            }
            add_client_json(clients, "wifi_ap", stations.sta[i].mac, ip_ptr,
                            "active", stations.sta[i].rssi);
        }
        return count;
    }

    if (config->ethernet_enabled && config->ethernet_role == SX_NETWORK_ROLE_DOWNLINK) {
        esp_netif_t *ethernet = w5500_manager_get_netif();
        if (ethernet == NULL) return 0;
        ethernet_client_snapshot_t snapshot = {
            .netif_index = esp_netif_get_netif_impl_index(ethernet),
        };
        if (snapshot.netif_index <= 0 ||
            tcpip_callback_wait(collect_ethernet_clients_on_tcpip, &snapshot) != ERR_OK) {
            return 0;
        }
        for (size_t i = 0; i < snapshot.count; ++i) {
            add_client_json(clients, "ethernet", snapshot.clients[i].mac,
                            &snapshot.clients[i].ip, "recent", INT_MIN);
        }
        return snapshot.count;
    }
    return 0;
}

static void add_route(cJSON *routes, const char *destination, const char *netmask,
                      const char *gateway, const char *interface_name, const char *type)
{
    cJSON *route = cJSON_CreateObject();
    cJSON_AddStringToObject(route, "destination", destination);
    cJSON_AddStringToObject(route, "netmask", netmask);
    cJSON_AddStringToObject(route, "gateway", gateway);
    cJSON_AddStringToObject(route, "interface", interface_name);
    cJSON_AddStringToObject(route, "type", type);
    cJSON_AddItemToArray(routes, route);
}

static void add_effective_routes(cJSON *root, const sx_network_status_t *status,
                                 const sx_network_config_t *config)
{
    cJSON *routes = cJSON_CreateArray();
    const char *active = sx_network_interface_name(status->active_interface);
    if (status->active_interface != SX_NETWORK_IF_NONE) {
        add_route(routes, "0.0.0.0", "0.0.0.0",
                  status->gateway[0] ? status->gateway : "--", active, "default");
    }

    const char *downlink = "none";
    const char *ip_text = NULL;
    const char *mask_text = NULL;
    if (config->wifi_ap_enabled && config->wifi_ap_role == SX_NETWORK_ROLE_DOWNLINK) {
        downlink = "wifi_ap";
        ip_text = config->ap_ip;
        mask_text = config->ap_netmask;
    } else if (config->ethernet_enabled && config->ethernet_role == SX_NETWORK_ROLE_DOWNLINK) {
        downlink = "ethernet";
        ip_text = config->ethernet_lan_ip;
        mask_text = config->ethernet_lan_netmask;
    }
    if (ip_text != NULL && mask_text != NULL) {
        ip4_addr_t ip = {0}, mask = {0}, network = {0};
        char network_text[16] = "--";
        if (ip4addr_aton(ip_text, &ip) && ip4addr_aton(mask_text, &mask)) {
            network.addr = ip.addr & mask.addr;
            ip4addr_ntoa_r(&network, network_text, sizeof(network_text));
        }
        add_route(routes, network_text, mask_text, "--", downlink, "connected");
    }
    cJSON_AddStringToObject(root, "downlink_interface", downlink);
    cJSON_AddItemToObject(root, "routes", routes);
}

static esp_err_t get_network_status_handler(httpd_req_t *req)
{
    sx_network_status_t status = {0};
    sx_network_config_t config = {0};
    sx_network_manager_get_status(&status);
    sx_network_manager_get_config(&config);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active_interface", sx_network_interface_name(status.active_interface));
    cJSON_AddBoolToObject(root, "wifi_connected", status.wifi_connected);
    cJSON_AddBoolToObject(root, "wifi_got_ip", status.wifi_got_ip);
    cJSON_AddBoolToObject(root, "w5500_link_up", status.w5500_link_up);
    cJSON_AddBoolToObject(root, "w5500_got_ip", status.w5500_got_ip);
    cJSON_AddBoolToObject(root, "modem_usb_connected", status.modem_usb_connected);
    cJSON_AddBoolToObject(root, "modem_got_ip", status.modem_got_ip);
    cJSON_AddStringToObject(root, "ip", status.ip);
    cJSON_AddStringToObject(root, "gateway", status.gateway);
    cJSON_AddStringToObject(root, "netmask", status.netmask);
    cJSON_AddBoolToObject(root, "routing_enabled", status.routing_enabled);
    cJSON_AddBoolToObject(root, "napt_active", status.napt_active);
    cJSON_AddStringToObject(root, "active_dns",
                            status.dns_ready ? status.active_dns : "0.0.0.0");
    cJSON_AddBoolToObject(root, "dns_ready", status.dns_ready);
    cJSON_AddNumberToObject(root, "uplink_generation", status.uplink_generation);
    cJSON_AddBoolToObject(root, "reboot_required", status.reboot_required);
    cJSON_AddNumberToObject(root, "client_limit", NETWORK_CLIENT_LIMIT);
    add_effective_routes(root, &status, &config);
    cJSON *clients = cJSON_CreateArray();
    size_t client_count = add_downstream_clients(clients, &config);
    cJSON_AddNumberToObject(root, "downstream_client_count", client_count);
    cJSON_AddItemToObject(root, "downstream_clients", clients);

    cJSON *interfaces = cJSON_CreateObject();
    const bool ethernet_static = config.ethernet_role == SX_NETWORK_ROLE_DOWNLINK;
    const bool ethernet_got_ip = status.w5500_got_ip ||
                                  (config.ethernet_enabled && status.w5500_started && ethernet_static);
    const char *ethernet_ip = status.ethernet_ip[0] ? status.ethernet_ip :
                              (ethernet_static ? config.ethernet_lan_ip : "");
    cJSON *ethernet = add_network_interface(interfaces, "ethernet", config.ethernet_enabled,
                          status.w5500_started, config.ethernet_role,
                          status.w5500_link_up,
                          ethernet_got_ip,
                          config.ethernet_role == SX_NETWORK_ROLE_UPLINK ||
                              config.ethernet_role == SX_NETWORK_ROLE_BACKUP ||
                              config.ethernet_role == SX_NETWORK_ROLE_LAST,
                          ethernet_ip);
    cJSON_AddBoolToObject(ethernet, "dhcp_server",
                          config.ethernet_enabled &&
                              config.ethernet_role == SX_NETWORK_ROLE_DOWNLINK &&
                              config.ethernet_dhcp_enabled);
    add_network_interface(interfaces, "wifi_sta", config.wifi_sta_enabled,
                          status.wifi_sta_started, config.wifi_sta_role,
                          status.wifi_connected,
                          status.wifi_got_ip, true, status.wifi_ip);
    cJSON *wifi_ap = add_network_interface(
        interfaces, "wifi_ap", config.wifi_ap_enabled,
        status.wifi_ap_started, config.wifi_ap_role,
        status.wifi_ap_client_count > 0, status.wifi_ap_client_count > 0,
        false, config.ap_ip);
    cJSON_AddNumberToObject(wifi_ap, "client_count", status.wifi_ap_client_count);
    cJSON_AddBoolToObject(wifi_ap, "dhcp_server",
                          config.wifi_ap_enabled &&
                              (config.wifi_ap_role == SX_NETWORK_ROLE_LOCAL ||
                               (config.wifi_ap_role == SX_NETWORK_ROLE_DOWNLINK &&
                                config.wifi_ap_dhcp_enabled)));
    add_network_interface(interfaces, "4g", config.modem_enabled,
                          status.modem_started, config.modem_role,
                          status.modem_usb_connected,
                          status.modem_got_ip, true, status.modem_ip);
    cJSON_AddItemToObject(root, "interfaces", interfaces);
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static void add_network_config_json(cJSON *root, const sx_network_config_t *cfg)
{
    cJSON_AddBoolToObject(root, "ethernet_enabled", cfg->ethernet_enabled);
    cJSON_AddStringToObject(root, "ethernet_role", sx_network_role_name(cfg->ethernet_role));
    cJSON_AddStringToObject(root, "ethernet_lan_ip", cfg->ethernet_lan_ip);
    cJSON_AddStringToObject(root, "ethernet_lan_netmask", cfg->ethernet_lan_netmask);
    cJSON_AddBoolToObject(root, "ethernet_static", cfg->ethernet_static);
    cJSON_AddStringToObject(root, "ethernet_gateway", cfg->ethernet_gateway);
    cJSON_AddStringToObject(root, "ethernet_dns", cfg->ethernet_dns);
    cJSON_AddBoolToObject(root, "ethernet_dhcp_enabled", cfg->ethernet_dhcp_enabled);
    cJSON_AddBoolToObject(root, "wifi_sta_enabled", cfg->wifi_sta_enabled);
    cJSON_AddStringToObject(root, "wifi_sta_role", sx_network_role_name(cfg->wifi_sta_role));
    cJSON_AddStringToObject(root, "wifi_ssid", cfg->wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_password", "");
    cJSON_AddBoolToObject(root, "wifi_password_configured", cfg->wifi_password[0] != '\0');
    cJSON_AddBoolToObject(root, "wifi_sta_static", cfg->wifi_sta_static);
    cJSON_AddStringToObject(root, "wifi_sta_ip", cfg->wifi_sta_ip);
    cJSON_AddStringToObject(root, "wifi_sta_netmask", cfg->wifi_sta_netmask);
    cJSON_AddStringToObject(root, "wifi_sta_gateway", cfg->wifi_sta_gateway);
    cJSON_AddStringToObject(root, "wifi_sta_dns", cfg->wifi_sta_dns);
    cJSON_AddBoolToObject(root, "wifi_ap_enabled", cfg->wifi_ap_enabled);
    cJSON_AddStringToObject(root, "wifi_ap_role", sx_network_role_name(cfg->wifi_ap_role));
    cJSON_AddStringToObject(root, "ap_ssid", cfg->ap_ssid);
    cJSON_AddStringToObject(root, "ap_password", "");
    cJSON_AddBoolToObject(root, "ap_password_configured", cfg->ap_password[0] != '\0');
    cJSON_AddStringToObject(root, "ap_ip", cfg->ap_ip);
    cJSON_AddStringToObject(root, "ap_netmask", cfg->ap_netmask);
    cJSON_AddNumberToObject(root, "ap_timeout_minutes", cfg->ap_timeout_minutes);
    cJSON_AddBoolToObject(root, "wifi_ap_dhcp_enabled", cfg->wifi_ap_dhcp_enabled);
    cJSON_AddBoolToObject(root, "modem_enabled", cfg->modem_enabled);
    cJSON_AddStringToObject(root, "modem_role", sx_network_role_name(cfg->modem_role));
    cJSON_AddBoolToObject(root, "routing_enabled", cfg->routing_enabled);
    cJSON_AddStringToObject(root, "nat_downlink", sx_nat_downlink_name(cfg->nat_downlink));
}

static esp_err_t get_network_config_handler(httpd_req_t *req)
{
    sx_network_config_t cfg;
    sx_network_status_t status = {0};
    sx_network_manager_get_config(&cfg);
    sx_network_manager_get_status(&status);
    cJSON *root = cJSON_CreateObject();
    add_network_config_json(root, &cfg);
    cJSON_AddBoolToObject(root, "reboot_required", status.reboot_required);
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static void update_json_bool(cJSON *json, const char *key, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsBool(item)) *value = cJSON_IsTrue(item);
    else if (cJSON_IsNumber(item)) *value = item->valueint != 0;
}

static void update_json_u8(cJSON *json, const char *key, uint8_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint <= 255)
        *value = (uint8_t)item->valueint;
}

static void update_json_u16(cJSON *json, const char *key, uint16_t *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsNumber(item) && item->valueint >= 0 && item->valueint <= 1440)
        *value = (uint16_t)item->valueint;
}

static void update_json_text(cJSON *json, const char *key, char *value, size_t size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    if (cJSON_IsString(item) && item->valuestring != NULL)
        snprintf(value, size, "%s", item->valuestring);
}

static bool update_json_role(cJSON *json, const char *key, sx_network_role_t *role)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(json, key);
    return item == NULL || (cJSON_IsString(item) &&
                            sx_network_role_from_name(item->valuestring, role));
}

static esp_err_t set_network_config_handler(httpd_req_t *req)
{
    const int64_t started = esp_timer_get_time();
    uint32_t stack_probe = 0;
    char trace[48] = "no-id";
    if (httpd_req_get_hdr_value_len(req, "X-Net-Save-Id") < sizeof(trace)) {
        httpd_req_get_hdr_value_str(req, "X-Net-Save-Id", trace, sizeof(trace));
    }
    ESP_LOGI(TAG, "[NETSAVE] %s begin bytes=%u stack_internal=%d stack_dram=%d",
             trace, (unsigned)req->content_len,
             esp_ptr_internal(&stack_probe), esp_ptr_in_dram(&stack_probe));
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        ESP_LOGW(TAG, "[NETSAVE] %s body/JSON failed", trace);
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }
    sx_network_config_t cfg;
    ESP_LOGI(TAG, "[NETSAVE] %s JSON OK; get config begin", trace);
    sx_network_manager_get_config(&cfg);
    ESP_LOGI(TAG, "[NETSAVE] %s get config OK", trace);
    update_json_bool(json, "ethernet_enabled", &cfg.ethernet_enabled);
    update_json_text(json, "ethernet_lan_ip", cfg.ethernet_lan_ip, sizeof(cfg.ethernet_lan_ip));
    update_json_text(json, "ethernet_lan_netmask", cfg.ethernet_lan_netmask, sizeof(cfg.ethernet_lan_netmask));
    update_json_bool(json, "ethernet_static", &cfg.ethernet_static);
    update_json_text(json, "ethernet_gateway", cfg.ethernet_gateway, sizeof(cfg.ethernet_gateway));
    update_json_text(json, "ethernet_dns", cfg.ethernet_dns, sizeof(cfg.ethernet_dns));
    update_json_bool(json, "ethernet_dhcp_enabled", &cfg.ethernet_dhcp_enabled);
    update_json_bool(json, "wifi_sta_enabled", &cfg.wifi_sta_enabled);
    update_json_text(json, "wifi_ssid", cfg.wifi_ssid, sizeof(cfg.wifi_ssid));
    cJSON *wifi_password = cJSON_GetObjectItemCaseSensitive(json, "wifi_password");
    if (cJSON_IsString(wifi_password) && wifi_password->valuestring != NULL &&
        wifi_password->valuestring[0] != '\0') {
        snprintf(cfg.wifi_password, sizeof(cfg.wifi_password), "%s", wifi_password->valuestring);
    }
    update_json_bool(json, "wifi_sta_static", &cfg.wifi_sta_static);
    update_json_text(json, "wifi_sta_ip", cfg.wifi_sta_ip, sizeof(cfg.wifi_sta_ip));
    update_json_text(json, "wifi_sta_netmask", cfg.wifi_sta_netmask, sizeof(cfg.wifi_sta_netmask));
    update_json_text(json, "wifi_sta_gateway", cfg.wifi_sta_gateway, sizeof(cfg.wifi_sta_gateway));
    update_json_text(json, "wifi_sta_dns", cfg.wifi_sta_dns, sizeof(cfg.wifi_sta_dns));
    update_json_bool(json, "wifi_ap_enabled", &cfg.wifi_ap_enabled);
    update_json_text(json, "ap_ssid", cfg.ap_ssid, sizeof(cfg.ap_ssid));
    cJSON *ap_password = cJSON_GetObjectItemCaseSensitive(json, "ap_password");
    if (cJSON_IsString(ap_password) && ap_password->valuestring != NULL &&
        ap_password->valuestring[0] != '\0') {
        snprintf(cfg.ap_password, sizeof(cfg.ap_password), "%s", ap_password->valuestring);
    }
    update_json_text(json, "ap_ip", cfg.ap_ip, sizeof(cfg.ap_ip));
    update_json_text(json, "ap_netmask", cfg.ap_netmask, sizeof(cfg.ap_netmask));
    update_json_u16(json, "ap_timeout_minutes", &cfg.ap_timeout_minutes);
    update_json_bool(json, "wifi_ap_dhcp_enabled", &cfg.wifi_ap_dhcp_enabled);
    update_json_bool(json, "modem_enabled", &cfg.modem_enabled);

    bool valid = update_json_role(json, "ethernet_role", &cfg.ethernet_role) &&
                 update_json_role(json, "wifi_sta_role", &cfg.wifi_sta_role) &&
                 update_json_role(json, "wifi_ap_role", &cfg.wifi_ap_role) &&
                 update_json_role(json, "modem_role", &cfg.modem_role);
    cJSON_Delete(json);
    if (!valid) {
        ESP_LOGW(TAG, "[NETSAVE] %s invalid role", trace);
        http_reply_code_msg(req, 400, "invalid network role");
        return ESP_OK;
    }
    sx_network_config_normalize(&cfg);
    ESP_LOGI(TAG, "[NETSAVE] %s normalized eth=%d/%d sta=%d/%d ap=%d/%d modem=%d/%d route=%d",
             trace, cfg.ethernet_enabled, cfg.ethernet_role, cfg.wifi_sta_enabled, cfg.wifi_sta_role,
             cfg.wifi_ap_enabled, cfg.wifi_ap_role, cfg.modem_enabled, cfg.modem_role, cfg.routing_enabled);
    char reason[128];
    esp_err_t err = sx_network_config_validate(&cfg, reason, sizeof(reason));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[NETSAVE] %s validation failed: %s", trace, reason);
        http_reply_code_msg(req, 400, reason);
        return ESP_OK;
    }
    ESP_LOGI(TAG, "[NETSAVE] %s validation OK; save begin", trace);
    err = sx_network_manager_save_config(&cfg);
    ESP_LOGI(TAG, "[NETSAVE] %s save result=%s", trace, esp_err_to_name(err));
    if (err != ESP_OK) {
        http_reply_code_msg(req, 500, "save network config failed");
        return ESP_OK;
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "code", 200);
    cJSON_AddStringToObject(response, "msg", "saved; reboot required to apply interface changes");
    cJSON_AddBoolToObject(response, "reboot_required", true);
    http_json_reply(req, response);
    ESP_LOGI(TAG, "[NETSAVE] %s reply attempted elapsed_ms=%lld", trace,
             (long long)((esp_timer_get_time() - started) / 1000));
    cJSON_Delete(response);
    return ESP_OK;
}

static void add_serial_layout_json(cJSON *root)
{
    cJSON_AddStringToObject(root, "layout",
                            sx_serial_layout_name(sx_serial_port_manager_get_layout()));
    cJSON_AddBoolToObject(root, "uart0_debug_console",
                          sx_serial_port_manager_uart0_reserved());
    cJSON *ports = cJSON_CreateArray();
    sx_serial_port_capability_t capabilities[4];
    size_t count = sx_serial_port_manager_get_capabilities(capabilities, 4);
    for (size_t i = 0; i < count; ++i) {
        cJSON *port = cJSON_CreateObject();
        cJSON_AddStringToObject(port, "id", capabilities[i].id);
        cJSON_AddStringToObject(port, "label", capabilities[i].label);
        cJSON_AddNumberToObject(port, "port", capabilities[i].port);
        cJSON_AddBoolToObject(port, "present", capabilities[i].present);
        cJSON_AddBoolToObject(port, "available", capabilities[i].available);
        cJSON_AddStringToObject(port, "reserved_by", capabilities[i].reserved_by);
        cJSON_AddItemToArray(ports, port);
    }
    cJSON_AddItemToObject(root, "ports", ports);
    cJSON *resources = cJSON_CreateArray();
    sx_serial_resource_info_t info[4];
    size_t resource_count = sx_serial_resource_get_all(info, 4);
    for (size_t i = 0; i < resource_count && i < 4; ++i) {
        cJSON *resource = cJSON_CreateObject();
        cJSON_AddStringToObject(resource, "id", info[i].id);
        cJSON_AddStringToObject(resource, "label", info[i].label);
        cJSON_AddNumberToObject(resource, "port", info[i].logical_port);
        cJSON_AddNumberToObject(resource, "tx_port", info[i].tx_port);
        cJSON_AddNumberToObject(resource, "rx_port", info[i].rx_port);
        cJSON_AddBoolToObject(resource, "available", info[i].available);
        cJSON_AddItemToArray(resources, resource);
    }
    cJSON_AddItemToObject(root, "resources", resources);
}

static esp_err_t get_serial_layout_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    add_serial_layout_json(root);
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t set_serial_layout_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }
    cJSON *value = cJSON_GetObjectItemCaseSensitive(json, "layout");
    sx_serial_layout_t layout;
    bool valid = cJSON_IsString(value) &&
                 sx_serial_layout_from_name(value->valuestring, &layout);
    if (!valid) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "layout must be dual_rs485 or rs422");
        return ESP_OK;
    }
    cJSON_Delete(json);
    ESP_LOGI(TAG, "[SERIAL_LAYOUT] save request: layout=%s", sx_serial_layout_name(layout));
    char reason[128];
    esp_err_t err = sx_serial_port_manager_save_layout(layout, reason, sizeof(reason));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[SERIAL_LAYOUT] save failed: layout=%s err=%s reason=%s",
                 sx_serial_layout_name(layout), esp_err_to_name(err),
                 reason[0] ? reason : "unknown");
        http_reply_code_msg(req, err == ESP_ERR_INVALID_STATE ? 409 : 500,
                            reason[0] ? reason : "save serial layout failed");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "[SERIAL_LAYOUT] saved: layout=%s reboot_required=true",
             sx_serial_layout_name(layout));
    cJSON *response = cJSON_CreateObject();
    cJSON_AddNumberToObject(response, "code", 200);
    cJSON_AddStringToObject(response, "msg", "saved; reboot required to apply serial layout");
    cJSON_AddBoolToObject(response, "reboot_required", true);
    http_json_reply(req, response);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t get_work_mode_set_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    cJSON *work_mode = cJSON_GetObjectItem(json, "work_mode");
    if (!cJSON_IsString(work_mode) || work_mode->valuestring == NULL) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "missing work_mode");
        return ESP_OK;
    }

    if (!is_supported_work_mode(work_mode->valuestring)) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 400, "current firmware profile does not support this work mode");
        return ESP_OK;
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) {
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, "nvs open failed");
        return ESP_OK;
    }

    esp_err_t mode_err = sx_work_mode_start_by_name(work_mode->valuestring);
    if (mode_err != ESP_OK) {
        nvs_close(nvs);
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, "work mode start failed");
        return ESP_OK;
    }

    esp_err_t save_err = nvs_set_str(nvs, "w_mode", work_mode->valuestring);
    if (save_err == ESP_OK) save_err = nvs_commit(nvs);
    nvs_close(nvs);
    if (save_err != ESP_OK) {
        char message[80];
        snprintf(message, sizeof(message), "work mode save failed: %s",
                 esp_err_to_name(save_err));
        ESP_LOGE(TAG, "%s", message);
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, message);
        return ESP_OK;
    }

    cJSON_Delete(json);
    http_reply_code_msg(req, 200, "work mode saved");
    return ESP_OK;
}

static esp_err_t auto_collect_set_handler(httpd_req_t *req)
{
    cJSON *json = NULL;
    if (read_json_request(req, &json) != ESP_OK) {
        http_reply_code_msg(req, 400, "invalid json body");
        return ESP_OK;
    }

    nvs_handle_t nvs = 0;
    esp_err_t save_err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (save_err != ESP_OK) {
        char message[80];
        snprintf(message, sizeof(message), "auto-collect NVS open failed: %s",
                 esp_err_to_name(save_err));
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, message);
        return ESP_OK;
    }

    for (int i = 1; i <= AC_COLLECT_PORT_COUNT && save_err == ESP_OK; i++) {
        const char *port_key = i == 1 ? "com2" : "com1";
        char prefix[8];
        snprintf(prefix, sizeof(prefix), "ac%d", i);

        cJSON *port_obj = cJSON_GetObjectItem(json, port_key);
        if (cJSON_IsObject(port_obj)) {
            save_err = ac_save_port_to_nvs(nvs, port_obj, prefix);
        }
    }
    if (save_err == ESP_OK) save_err = nvs_commit(nvs);
    nvs_close(nvs);

    if (save_err != ESP_OK) {
        char message[96];
        snprintf(message, sizeof(message), "auto-collect save failed: %s",
                 esp_err_to_name(save_err));
        ESP_LOGE(TAG, "%s", message);
        cJSON_Delete(json);
        http_reply_code_msg(req, 500, message);
        return ESP_OK;
    }

    sx_auto_collect_init();

    cJSON_Delete(json);
    http_reply_code_msg(req, 200, "auto_collect 保存成功");
    return ESP_OK;
}

static esp_err_t get_work_mode_info_handler(httpd_req_t *req)
{
    cJSON *response = cJSON_CreateObject();
    add_firmware_capabilities_json(response);
    nvs_handle_t nvs;

    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        char schema_ver[16];
        char hw_profile[64];
        cJSON_AddStringToObject(response, "cfg_schema_ver",
                                nvs_get_string_or_default(nvs, NVS_SCHEMA_KEY, NVS_SCHEMA_VERSION,
                                                          schema_ver, sizeof(schema_ver)));
        cJSON_AddStringToObject(response, "hw_profile",
                                nvs_get_string_or_default(nvs, NVS_HW_PROFILE_KEY, NVS_HW_PROFILE_VALUE,
                                                          hw_profile, sizeof(hw_profile)));

        char work_mode[32];
        const char *mode = nvs_get_string_or_default(nvs, "w_mode", default_work_mode(), work_mode, sizeof(work_mode));
        if (!is_supported_work_mode(mode)) {
            mode = default_work_mode();
        }
        cJSON_AddStringToObject(response, "work_mode", mode);

        if (strcmp(mode, "auto_collect") == 0) {
            cJSON_AddNumberToObject(response, "master_port", HW_AUTO_COLLECT_MASTER_PORT);
            for (int i = 1; i <= AC_COLLECT_PORT_COUNT; i++) {
                const char *port_key = i == 1 ? "com2" : "com1";
                char prefix[8];
                snprintf(prefix, sizeof(prefix), "ac%d", i);
                ac_add_port_from_nvs(response, nvs, port_key, prefix);
            }
        }

        nvs_close(nvs);
    } else {
        cJSON_AddStringToObject(response, "cfg_schema_ver", NVS_SCHEMA_VERSION);
        cJSON_AddStringToObject(response, "hw_profile", NVS_HW_PROFILE_VALUE);
        cJSON_AddStringToObject(response, "work_mode", default_work_mode());
        cJSON_AddNumberToObject(response, "master_port", HW_AUTO_COLLECT_MASTER_PORT);
    }

    http_json_reply(req, response);
    cJSON_Delete(response);
    return ESP_OK;
}

static esp_err_t get_modbus_filter_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "supported", false);
    cJSON_AddBoolToObject(root, "enabled", false);
    cJSON_AddNumberToObject(root, "mode", 0);
    cJSON_AddItemToObject(root, "ranges", cJSON_CreateArray());
    cJSON_AddStringToObject(root, "msg", "modbus filter is not enabled in current firmware profile");

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t set_modbus_filter_config_handler(httpd_req_t *req)
{
    (void)req;
    http_reply_feature_not_supported(req, "modbus_filter");
    return ESP_OK;
}

static esp_err_t get_slave_mapping_config_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "supported", false);
    cJSON_AddBoolToObject(root, "enabled", false);
    cJSON_AddItemToObject(root, "mappings", cJSON_CreateArray());
    cJSON_AddStringToObject(root, "msg", "slave mapping is not enabled in current firmware profile");

    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t set_slave_mapping_config_handler(httpd_req_t *req)
{
    (void)req;
    http_reply_feature_not_supported(req, "slave_mapping");
    return ESP_OK;
}

static esp_err_t get_find_wifi_get_handler(httpd_req_t *req)
{
    enum { MAX_SCAN_RESULTS = 32 };
    wifi_ap_record_t records[MAX_SCAN_RESULTS] = {0};
    uint16_t count = MAX_SCAN_RESULTS;
    esp_err_t err = sx_wifi_scan_access_points(records, &count);
    cJSON *root = cJSON_CreateObject();
    if (err != ESP_OK) {
        const char *message = "Wi-Fi 扫描失败，请稍后重试";
        if (err == ESP_ERR_INVALID_STATE) {
            message = "Wi-Fi 正在连接或扫描中，请稍后重试";
        } else if (err == ESP_ERR_WIFI_NOT_STARTED || err == ESP_ERR_WIFI_NOT_INIT) {
            message = "Wi-Fi 当前未启动，请先启用 Wi-Fi 客户端或热点";
        }
        cJSON_AddStringToObject(root, "error", message);
        cJSON_AddStringToObject(root, "error_code", esp_err_to_name(err));
        cJSON_AddNumberToObject(root, "wait_time", 3000);
        http_json_reply(req, root);
        cJSON_Delete(root);
        return ESP_OK;
    }

    cJSON_AddNumberToObject(root, "count", count);
    cJSON *networks = cJSON_CreateArray();
    for (uint16_t i = 0; i < count; ++i) {
        cJSON *network = cJSON_CreateObject();
        char ssid[33] = {0};
        memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
        ssid[sizeof(ssid) - 1] = '\0';
        cJSON_AddStringToObject(network, "ssid", ssid);
        cJSON_AddNumberToObject(network, "rssi", records[i].rssi);
        cJSON_AddNumberToObject(network, "port", records[i].primary);
        const char *security = records[i].authmode == WIFI_AUTH_OPEN ? "开放" : "加密";
        cJSON_AddStringToObject(network, "security", security);
        cJSON_AddItemToArray(networks, network);
    }
    cJSON_AddItemToObject(root, "networks", networks);
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_ota_post_handler(httpd_req_t *req)
{
    (void)req;
    http_reply_code_msg(req, 500, "OTA功能未启用");
    return ESP_OK;
}

static esp_err_t get_ota_progress_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "progress", 0);
    cJSON_AddStringToObject(root, "status", "OTA未开始");
    http_json_reply(req, root);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t get_operate_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", 200);
    cJSON_AddStringToObject(root, "msg", "restart");
    http_json_reply(req, root);
    cJSON_Delete(root);

    /* Allow the HTTP response to leave the socket before resetting the chip. */
    sx_led_manager_indicate_restart();
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
    return ESP_OK;
}

static esp_err_t get_restore_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "code", 200);
    cJSON_AddStringToObject(root, "msg", "restore");
    http_json_reply(req, root);
    cJSON_Delete(root);

    /* Keep reset behavior consistent with restart and let the response flush. */
    sx_led_manager_indicate_factory_reset();
    vTaskDelay(pdMS_TO_TICKS(1600));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
#if CONFIG_HTTPD_WS_SUPPORT
    if (req->method == HTTP_GET) {
        int fd = httpd_req_to_sockfd(req);
        ws_add_client(fd);
        ESP_LOGI(TAG, "ws client connected fd=%d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t err = httpd_ws_recv_frame(req, &frame, 0);
    if (err != ESP_OK) {
        return err;
    }

    if (frame.len > 0) {
        frame.payload = (uint8_t *)calloc(1, frame.len + 1);
        if (!frame.payload) {
            return ESP_ERR_NO_MEM;
        }
        err = httpd_ws_recv_frame(req, &frame, frame.len);
        free(frame.payload);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        ws_remove_client(fd);
    }

    return ESP_OK;
#else
    (void)req;
    return ESP_OK;
#endif
}

void send_uart_to_websocket(const uint8_t *data, size_t len, bool is_tx, int port)
{
    if (data == NULL || len == 0) {
        return;
    }

    size_t hex_len = len * 3 + 1;
    char *hex = (char *)calloc(1, hex_len);
    char *ascii = (char *)calloc(1, len + 1);
    if (!hex || !ascii) {
        free(hex);
        free(ascii);
        return;
    }

    char *hex_ptr = hex;
    for (size_t i = 0; i < len; i++) {
        snprintf(hex_ptr, 4, "%02X ", data[i]);
        hex_ptr += 3;

        uint8_t c = data[i];
        ascii[i] = (c >= 32 && c <= 126) ? (char)c : '.';
    }
    if (len > 0) {
        hex[len * 3 - 1] = '\0';
    }
    ascii[len] = '\0';

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "uart_data");
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddBoolToObject(root, "is_tx", is_tx);
    cJSON_AddStringToObject(root, "hex", hex);
    cJSON_AddStringToObject(root, "ascii", ascii);
    cJSON_AddNumberToObject(root, "timestamp", (double)time_manager_get_current_us());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        ws_broadcast_text(payload);
        free(payload);
    }

    cJSON_Delete(root);
    free(hex);
    free(ascii);
}

void send_uart_event_to_websocket(int port, const char *level, const char *source, const char *text)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "uart_event");
    cJSON_AddNumberToObject(root, "port", port);
    cJSON_AddStringToObject(root, "level", level ? level : "INFO");
    cJSON_AddStringToObject(root, "source", source ? source : "system");
    cJSON_AddStringToObject(root, "text", text ? text : "");
    cJSON_AddNumberToObject(root, "timestamp", (double)time_manager_get_current_us());

    char *payload = cJSON_PrintUnformatted(root);
    if (payload) {
        ws_broadcast_text(payload);
        free(payload);
    }
    cJSON_Delete(root);
}

void send_uart_to_websocket_async(const uint8_t *data, size_t len, bool is_tx, int port)
{
    send_uart_to_websocket(data, len, is_tx, port);
}

void send_uart_to_websocket_from_port(const uint8_t *data, size_t len, bool is_tx, uart_port_t uart_num)
{
    int physical_port = 1;
    if (uart_num == UART_NUM_1) {
        physical_port = 1;
    } else if (uart_num == UART_NUM_2) {
        physical_port = 2;
    } else if (uart_num == UART_NUM_0) {
        physical_port = 3;
    }
    int logical_port = physical_port;
    if (is_tx) {
        /* In RS422 the logical port 1 TX is physically UART0/COM1. */
        logical_port = (sx_serial_port_manager_get_layout() == SX_SERIAL_LAYOUT_RS422 &&
                           physical_port == 3) ? 1 : physical_port;
    } else {
        logical_port = sx_serial_resource_logical_from_physical_rx(physical_port);
    }
    send_uart_to_websocket(data, len, is_tx, logical_port);
}

static const httpd_uri_t root_uri = {
    .uri = "/",
    .method = HTTP_GET,
    .handler = root_get_handler,
};

static const httpd_uri_t web_html_uri = {
    .uri = "/web.html",
    .method = HTTP_GET,
    .handler = web_html_get_handler,
};

static const httpd_uri_t status_uri = {
    .uri = "/status",
    .method = HTTP_GET,
    .handler = web_html_get_handler,
};

static const httpd_uri_t login_uri = {
    .uri = "/login",
    .method = HTTP_POST,
    .handler = login_post_handler,
};

static const httpd_uri_t logout_uri = {
    .uri = "/logout",
    .method = HTTP_POST,
    .handler = logout_post_handler,
};

static const httpd_uri_t root_html_uri = {
    .uri = "/root.html",
    .method = HTTP_GET,
    .handler = root_html_get_handler,
};

static const httpd_uri_t js_uri = {
    .uri = "/web.js",
    .method = HTTP_GET,
    .handler = js_get_handler,
};

static const httpd_uri_t css_uri = {
    .uri = "/web.css",
    .method = HTTP_GET,
    .handler = css_get_handler,
};

static const httpd_uri_t devinfo_uri = {
    .uri = "/devinfo",
    .method = HTTP_GET,
    .handler = get_devinfo_get_handler,
};

static const httpd_uri_t sys_uri = {
    .uri = "/sys",
    .method = HTTP_GET,
    .handler = get_sys_get_handler,
};

static const httpd_uri_t module_set_uri = {
    .uri = "/module_set",
    .method = HTTP_POST,
    .handler = get_module_set_post_handler,
};

static const httpd_uri_t module_set_info_uri = {
    .uri = "/module_set_info",
    .method = HTTP_GET,
    .handler = get_module_set_info_get_handler,
};

static const httpd_uri_t net_set_uri = {
    .uri = "/net_set",
    .method = HTTP_POST,
    .handler = get_net_set_post_handler,
};

static const httpd_uri_t net_set_info_uri = {
    .uri = "/net_set_info",
    .method = HTTP_GET,
    .handler = get_net_set_info_get_handler,
};

static const httpd_uri_t ap_set_uri = {
    .uri = "/ap_set",
    .method = HTTP_POST,
    .handler = get_ap_set_post_handler,
};

static const httpd_uri_t ap_set_info_uri = {
    .uri = "/ap_set_info",
    .method = HTTP_GET,
    .handler = get_ap_set_info_get_handler,
};

static const httpd_uri_t serial_mode_info_uri = {
    .uri = "/serial_config_mode_info",
    .method = HTTP_GET,
    .handler = get_serial_config_mode_info_handler,
};

static const httpd_uri_t serial_mode_set_uri = {
    .uri = "/serial_config_mode_set",
    .method = HTTP_POST,
    .handler = get_serial_config_mode_set_handler,
};

static const httpd_uri_t serial_set_uri = {
    .uri = "/serial_set",
    .method = HTTP_POST,
    .handler = get_serial_set_handler,
};

static const httpd_uri_t serial_set_info_uri = {
    .uri = "/serial_set_info",
    .method = HTTP_GET,
    .handler = get_serial_set_info_get_handler,
};

static const httpd_uri_t serial_runtime_status_uri = {
    .uri = "/serial_runtime_status",
    .method = HTTP_GET,
    .handler = get_serial_runtime_status_get_handler,
};

static const httpd_uri_t system_logs_uri = {
    .uri = "/system_logs",
    .method = HTTP_GET,
    .handler = get_system_logs_get_handler,
};

static const httpd_uri_t serial_ctl_uri = {
    .uri = "/serial_ctl",
    .method = HTTP_POST,
    .handler = get_serial_ctl_handler,
};

static const httpd_uri_t mode_set_uri = {
    .uri = "/mode_set",
    .method = HTTP_POST,
    .handler = get_work_mode_set_handler,
};

static const httpd_uri_t network_status_uri = {
    .uri = "/network_status",
    .method = HTTP_GET,
    .handler = get_network_status_handler,
};

static const httpd_uri_t network_config_get_uri = {
    .uri = "/network_config",
    .method = HTTP_GET,
    .handler = get_network_config_handler,
};

static const httpd_uri_t network_config_post_uri = {
    .uri = "/network_config",
    .method = HTTP_POST,
    .handler = set_network_config_handler,
};

static const httpd_uri_t serial_layout_get_uri = {
    .uri = "/serial_layout",
    .method = HTTP_GET,
    .handler = get_serial_layout_handler,
};

static const httpd_uri_t serial_layout_post_uri = {
    .uri = "/serial_layout",
    .method = HTTP_POST,
    .handler = set_serial_layout_handler,
};

static const httpd_uri_t mode_info_uri = {
    .uri = "/mode_info",
    .method = HTTP_GET,
    .handler = get_work_mode_info_handler,
};

static const httpd_uri_t auto_collect_set_uri = {
    .uri = "/auto_collect_set",
    .method = HTTP_POST,
    .handler = auto_collect_set_handler,
};

static const httpd_uri_t filter_config_uri = {
    .uri = "/filter_config",
    .method = HTTP_GET,
    .handler = get_modbus_filter_config_handler,
};

static const httpd_uri_t filter_set_uri = {
    .uri = "/filter_set",
    .method = HTTP_POST,
    .handler = set_modbus_filter_config_handler,
};

static const httpd_uri_t slave_mapping_config_uri = {
    .uri = "/slave_mapping_config",
    .method = HTTP_GET,
    .handler = get_slave_mapping_config_handler,
};

static const httpd_uri_t slave_mapping_set_uri = {
    .uri = "/slave_mapping_set",
    .method = HTTP_POST,
    .handler = set_slave_mapping_config_handler,
};

static const httpd_uri_t find_wifi_uri = {
    .uri = "/find_wifi",
    .method = HTTP_GET,
    .handler = get_find_wifi_get_handler,
};

static const httpd_uri_t ota_uri = {
    .uri = "/ota",
    .method = HTTP_POST,
    .handler = get_ota_post_handler,
};

static const httpd_uri_t ota_progress_uri = {
    .uri = "/ota_progress",
    .method = HTTP_GET,
    .handler = get_ota_progress_handler,
};

static const httpd_uri_t operate_uri = {
    .uri = "/operate",
    .method = HTTP_GET,
    .handler = get_operate_get_handler,
};

static const httpd_uri_t restore_uri = {
    .uri = "/restore",
    .method = HTTP_GET,
    .handler = get_restore_get_handler,
};

static const httpd_uri_t sync_time_uri = {
    .uri = "/api/sync_time",
    .method = HTTP_POST,
    .handler = sync_time_handler,
};

static const httpd_uri_t ws_uri = {
    .uri = "/ws/log",
    .method = HTTP_GET,
    .handler = ws_handler,
#if CONFIG_HTTPD_WS_SUPPORT
    .is_websocket = true,
#endif
};

void http_server_init(void)
{
    if (s_http_server != NULL) {
      return;
    }

    s_protected_route_count = 0;
    memset(s_protected_routes, 0, sizeof(s_protected_routes));

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 10240;
    /* NVS/Flash writes can disable cache; the HTTP task stack must stay in internal RAM. */
    config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    config.max_uri_handlers = 64;
    const int desired_open_sockets = 16;
    const int httpd_internal_sockets = 3;
    int max_open_sockets = CONFIG_LWIP_MAX_SOCKETS - httpd_internal_sockets;
    if (max_open_sockets < 1) {
        max_open_sockets = 1;
    }
    config.max_open_sockets = (desired_open_sockets <= max_open_sockets)
                                  ? desired_open_sockets
                                  : max_open_sockets;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.keep_alive_enable = false;
    config.enable_so_linger = true;
    config.linger_timeout = 1;

    ESP_LOGI(TAG, "http max_open_sockets=%d (desired=%d, lwip_max=%d)",
             config.max_open_sockets, desired_open_sockets, CONFIG_LWIP_MAX_SOCKETS);

    if (httpd_start(&s_http_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "http server start failed");
        s_http_server = NULL;
        return;
    }

    httpd_register_uri_handler(s_http_server, &root_uri);
    httpd_register_uri_handler(s_http_server, &root_html_uri);
    httpd_register_uri_handler(s_http_server, &css_uri);
    httpd_register_uri_handler(s_http_server, &login_uri);
    register_protected_route(s_http_server, &status_uri);
    register_protected_route(s_http_server, &web_html_uri);
    register_protected_route(s_http_server, &js_uri);
    register_protected_route(s_http_server, &logout_uri);

    register_protected_route(s_http_server, &devinfo_uri);
    register_protected_route(s_http_server, &sys_uri);

    register_protected_route(s_http_server, &module_set_uri);
    register_protected_route(s_http_server, &module_set_info_uri);
    register_protected_route(s_http_server, &net_set_uri);
    register_protected_route(s_http_server, &net_set_info_uri);
    register_protected_route(s_http_server, &ap_set_uri);
    register_protected_route(s_http_server, &ap_set_info_uri);

    register_protected_route(s_http_server, &serial_mode_info_uri);
    register_protected_route(s_http_server, &serial_mode_set_uri);
    register_protected_route(s_http_server, &serial_set_uri);
    register_protected_route(s_http_server, &serial_set_info_uri);
    register_protected_route(s_http_server, &serial_runtime_status_uri);
    register_protected_route(s_http_server, &system_logs_uri);
    register_protected_route(s_http_server, &serial_ctl_uri);

    register_protected_route(s_http_server, &mode_set_uri);
    register_protected_route(s_http_server, &mode_info_uri);
    register_protected_route(s_http_server, &network_status_uri);
    register_protected_route(s_http_server, &network_config_get_uri);
    register_protected_route(s_http_server, &network_config_post_uri);
    register_protected_route(s_http_server, &serial_layout_get_uri);
    register_protected_route(s_http_server, &serial_layout_post_uri);
    register_protected_route(s_http_server, &auto_collect_set_uri);

    register_protected_route(s_http_server, &filter_config_uri);
    register_protected_route(s_http_server, &filter_set_uri);
    register_protected_route(s_http_server, &slave_mapping_config_uri);
    register_protected_route(s_http_server, &slave_mapping_set_uri);

    register_protected_route(s_http_server, &find_wifi_uri);

    register_protected_route(s_http_server, &ota_uri);
    register_protected_route(s_http_server, &ota_progress_uri);

    register_protected_route(s_http_server, &operate_uri);
    register_protected_route(s_http_server, &restore_uri);
    register_protected_route(s_http_server, &sync_time_uri);

    register_protected_route(s_http_server, &ws_uri);

    ESP_LOGI(TAG, "http server started on port %d", config.server_port);
}
