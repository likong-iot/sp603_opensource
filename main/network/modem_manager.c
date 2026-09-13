#include "modem_manager.h"

#include <stdbool.h>
#include <inttypes.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include "usbh_modem_board.h"
#include "app_task_utils.h"
#include "s3_gpio.h"
#include "sx_network_manager.h"

static const char *TAG = "modem_mgr";
static const char *TAG_EVENT = "4g_event";
static bool s_modem_ready = false;
// 记录 PPP 拨号后是否已经获取有效 IP，用于对外判定 4G 网络可用
static volatile bool s_modem_connected = false;
// 记录 USB CDC 是否已经枚举并连接（DTE）
static volatile bool s_modem_usb_connected = false;
static TaskHandle_t s_modem_task_handle = NULL;
static bool s_ip_event_registered = false;

#define MODEM_MANAGER_TASK_STACK_SIZE    (8192)
#define MODEM_MANAGER_TASK_PRIORITY      (5)
#define MODEM_POWER_PULSE_MS              (2000)
#define MODEM_POWER_BOOT_WAIT_MS          (5000)

static void modem_manager_task(void *arg);
static void modem_ip_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data);

static void modem_manager_power_on_pulse(void)
{
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << AIR780E_PWRKEY,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&config));

    gpio_set_level(AIR780E_PWRKEY, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "AIR780E PWRKEY pulse: GPIO%d high for %d ms",
             AIR780E_PWRKEY, MODEM_POWER_PULSE_MS);
    gpio_set_level(AIR780E_PWRKEY, 1);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWER_PULSE_MS));
    gpio_set_level(AIR780E_PWRKEY, 0);

    ESP_LOGI(TAG, "waiting %d ms for AIR780E power-up",
             MODEM_POWER_BOOT_WAIT_MS);
    vTaskDelay(pdMS_TO_TICKS(MODEM_POWER_BOOT_WAIT_MS));
}

static bool modem_manager_try_get_ppp_ip(esp_netif_ip_info_t *ip_info)
{
    bool already_connected = s_modem_connected;
    if (ip_info != NULL) {
        ip_info->ip.addr = 0;
        ip_info->gw.addr = 0;
        ip_info->netmask.addr = 0;
    }

    esp_netif_t *ppp_netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
    if (!ppp_netif) {
        return already_connected;
    }

    esp_netif_ip_info_t tmp_ip_info = {0};
    if (esp_netif_get_ip_info(ppp_netif, &tmp_ip_info) != ESP_OK || tmp_ip_info.ip.addr == 0) {
        return already_connected;
    }

    if (ip_info != NULL) {
        *ip_info = tmp_ip_info;
    }

    s_modem_connected = true;
    return true;
}

static void modem_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    if (base != MODEM_BOARD_EVENT) {
        return;
    }
    switch (id) {
    case MODEM_EVENT_DTE_CONN:
        ESP_LOGI(TAG_EVENT, "USB 已连接 4G 模组");
        s_modem_usb_connected = true;
        sx_network_manager_modem_connected();
        break;
    case MODEM_EVENT_DTE_DISCONN:
        ESP_LOGW(TAG_EVENT, "USB 与 4G 模组断开");
        s_modem_usb_connected = false;
        s_modem_connected = false;
        sx_network_manager_modem_disconnected();
        break;
    case MODEM_EVENT_SIMCARD_CONN:
        ESP_LOGI(TAG_EVENT, "SIM 卡检测正常");
        break;
    case MODEM_EVENT_SIMCARD_DISCONN:
        ESP_LOGW(TAG_EVENT, "SIM 卡缺失或 PIN 错误");
        break;
    case MODEM_EVENT_NET_CONN: {
        ESP_LOGI(TAG_EVENT, "收到 PPP 已连接事件(MODEM_EVENT_NET_CONN)");
        esp_netif_ip_info_t ip_info = {0};
        if (modem_manager_try_get_ppp_ip(&ip_info)) {
            if (ip_info.ip.addr != 0) {
                ESP_LOGI(TAG_EVENT, "PPP IP:" IPSTR " GW:" IPSTR " MASK:" IPSTR,
                         IP2STR(&ip_info.ip), IP2STR(&ip_info.gw), IP2STR(&ip_info.netmask));
            } else {
                ESP_LOGI(TAG_EVENT, "PPP 已连接，IP 信息稍后由 IP_EVENT/轮询日志补充");
            }
        } else {
            ESP_LOGW(TAG_EVENT, "NET_CONN 已到达，但 PPP IP 暂未就绪，等待 IP_EVENT_PPP_GOT_IP 或轮询确认");
        }
        esp_netif_dns_info_t dns;
        if (modem_board_get_dns_info(ESP_NETIF_DNS_MAIN, &dns) == ESP_OK) {
            ESP_LOGI(TAG_EVENT, "PPP DNS(Main): " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        if (modem_board_get_dns_info(ESP_NETIF_DNS_BACKUP, &dns) == ESP_OK) {
            ESP_LOGI(TAG_EVENT, "PPP DNS(Backup): " IPSTR, IP2STR(&dns.ip.u_addr.ip4));
        }
        break;
    }
    case MODEM_EVENT_NET_DISCONN:
        ESP_LOGW(TAG_EVENT, "PPP 拨号断开，等待重试");
        s_modem_connected = false;
        sx_network_manager_modem_disconnected();
        break;
    case MODEM_EVENT_WIFI_STA_CONN: {
        intptr_t cnt = (intptr_t)event_data;
        ESP_LOGI(TAG_EVENT, "热点有新设备接入，总数=%"PRIiPTR, cnt);
        break;
    }
    case MODEM_EVENT_WIFI_STA_DISCONN: {
        intptr_t cnt = (intptr_t)event_data;
        ESP_LOGI(TAG_EVENT, "热点有设备离线，剩余=%"PRIiPTR, cnt);
        break;
    }
    default:
        ESP_LOGI(TAG_EVENT, "Modem 事件 id=%"PRIi32, id);
        break;
    }
}

static void modem_ip_event_handler(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    (void)handler_args;
    if (base != IP_EVENT) {
        return;
    }

    if (id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        if (event != NULL) {
            esp_netif_t *ppp_netif = esp_netif_get_handle_from_ifkey("PPP_DEF");
            if (ppp_netif != NULL && event->esp_netif != ppp_netif) {
                return;
            }
            s_modem_connected = true;
            sx_network_manager_modem_got_ip(&event->ip_info);
            ESP_LOGI(TAG_EVENT, "收到 PPP GOT IP 事件: IP:" IPSTR " GW:" IPSTR " MASK:" IPSTR,
                     IP2STR(&event->ip_info.ip),
                     IP2STR(&event->ip_info.gw),
                     IP2STR(&event->ip_info.netmask));
        } else {
            s_modem_connected = true;
            ESP_LOGI(TAG_EVENT, "收到 PPP GOT IP 事件");
        }
        return;
    }

    if (id == IP_EVENT_PPP_LOST_IP) {
        s_modem_connected = false;
        sx_network_manager_modem_disconnected();
        ESP_LOGW(TAG_EVENT, "收到 PPP LOST IP 事件");
    }
}

// 该任务独立完成调制解调器、PPP 初始化，避免阻塞 app_main
static void modem_manager_task(void *arg)
{
    (void)arg;
    esp_err_t err = ESP_OK;

    ESP_LOGI(TAG, "开始初始化 4G 模组并拨号");
    modem_manager_power_on_pulse();
    modem_config_t modem_cfg = MODEM_DEFAULT_CONFIG();
    // 非阻塞返回，避免在 PPP 未建链时卡住整个 modem_manager_task
    modem_cfg.flags |= MODEM_FLAGS_INIT_NOT_BLOCK | MODEM_FLAGS_INIT_NOT_FORCE_RESET;
    modem_cfg.handler = modem_event_handler;
    modem_cfg.handler_arg = NULL;
    err = modem_board_init(&modem_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "modem board init failed (%s)", esp_err_to_name(err));
        goto exit;
    }

    s_modem_ready = true;
    ESP_LOGI(TAG, "4G 模组初始化完成（已关闭 4G->Wi-Fi AP 共享逻辑）");

exit:
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "4G 模组初始化任务失败: %s", esp_err_to_name(err));
    }
    s_modem_task_handle = NULL;
    delete_self_app_task_with_caps();
}

esp_err_t modem_manager_init(void)
{
    if (s_modem_ready) {
        return ESP_OK;
    }
    if (s_modem_task_handle) {
        return ESP_OK;
    }

    if (!s_ip_event_registered) {
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, modem_ip_event_handler, NULL),
                            TAG, "register IP_EVENT_PPP_GOT_IP failed");
        ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, modem_ip_event_handler, NULL),
                            TAG, "register IP_EVENT_PPP_LOST_IP failed");
        s_ip_event_registered = true;
    }

    BaseType_t ret = create_app_task_psram(modem_manager_task,
                                           "modem_mgr",
                                           MODEM_MANAGER_TASK_STACK_SIZE,
                                           NULL,
                                           MODEM_MANAGER_TASK_PRIORITY,
                                           &s_modem_task_handle,
                                           tskNO_AFFINITY);
    if (ret != pdPASS) {
        s_modem_task_handle = NULL;
        ESP_LOGE(TAG, "创建 4G 模组任务失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "4G 模组任务已启动");
    return ESP_OK;
}

bool modem_manager_is_connected(void)
{
    // 仅当 PPP 连接事件中拿到 IP 后才会返回 true
    return s_modem_connected;
}
