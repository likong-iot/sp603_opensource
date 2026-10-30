#include "w5500_manager.h"

#include <stdbool.h>
#include <esp_check.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_eth.h>
#include <esp_eth_mac_spi.h>
#include <esp_eth_netif_glue.h>
#include <esp_mac.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_netif_ip_addr.h>
#include <esp_netif_defaults.h>
#include <dhcpserver/dhcpserver.h>
#include <lwip/ip4_addr.h>
#include "s3_gpio.h"
#include "sx_network_manager.h"

#define W5500_SPI_HOST      SPI2_HOST
#define W5500_PIN_MOSI      W5500_MOSI
#define W5500_PIN_MISO      W5500_MISO
#define W5500_PIN_SCLK      W5500_SCLK
#define W5500_PIN_CS        W5500_SCSN
#define W5500_PIN_INT       W5500_INT
#define W5500_PIN_RST       W5500_RST
#define W5500_PHY_ADDR      1
#define W5500_SPI_CLOCK_HZ  (40 * 1000 * 1000)

static const char *TAG = "w5500_mgr";
static bool s_initialized = false;
static bool s_cleanup_failed;
static bool s_w5500_link_up = false;
static bool s_isr_service_installed = false;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_netif_dns_info_t s_w5500_dns_main = {0};
static bool s_w5500_dns_valid = false;
static bool s_static_address = false;
static bool s_dhcp_server_enabled = false;
static spi_device_interface_config_t s_w5500_devcfg = {
    .command_bits = 0,
    .address_bits = 0,
    .dummy_bits = 0,
    .mode = 0,
    .clock_speed_hz = W5500_SPI_CLOCK_HZ,
    .spics_io_num = W5500_PIN_CS,
    .queue_size = 32,
};

static void w5500_set_dhcp_server_running(bool running)
{
    if (!s_dhcp_server_enabled || s_eth_netif == NULL) return;
    esp_err_t err = running ? esp_netif_dhcps_start(s_eth_netif)
                            : esp_netif_dhcps_stop(s_eth_netif);
    esp_err_t already = running ? ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED
                                : ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED;
    if (err != ESP_OK && err != already) {
        ESP_LOGE(TAG, "DHCP server %s failed: %s",
                 running ? "start" : "stop", esp_err_to_name(err));
    }
}

static void w5500_eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    uint8_t mac[6] = { 0 };
    if (event_data == NULL) return;
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;
    if (eth_handle != s_eth_handle) return;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "W5500 Link Up, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        s_w5500_link_up = true;
        /* The Ethernet glue was registered first and has brought the netif up.
         * Starting before CONNECTED only leaves the DHCP server in INIT. */
        w5500_set_dhcp_server_running(true);
        sx_network_manager_w5500_link_up();
        if (s_static_address && s_eth_netif != NULL) {
            esp_netif_ip_info_t info = {0};
            if (esp_netif_get_ip_info(s_eth_netif, &info) == ESP_OK &&
                info.ip.addr != 0) {
                sx_network_manager_w5500_got_ip(&info);
            }
        }
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "W5500 Link Down");
        w5500_set_dhcp_server_running(false);
        s_w5500_link_up = false;
        s_w5500_dns_valid = false;
        sx_network_manager_w5500_link_down();
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "W5500 Started");
        break;
    case ETHERNET_EVENT_STOP:
        w5500_set_dhcp_server_running(false);
        s_w5500_link_up = false;
        s_w5500_dns_valid = false;
        sx_network_manager_w5500_link_down();
        ESP_LOGI(TAG, "W5500 Stopped");
        break;
    default:
        break;
    }
}

static void w5500_on_got_ip(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    if (event == NULL || event->esp_netif != s_eth_netif) return;
    ESP_LOGI(TAG, "W5500 Got IP:" IPSTR " Mask:" IPSTR " GW:" IPSTR,
             IP2STR(&event->ip_info.ip), IP2STR(&event->ip_info.netmask), IP2STR(&event->ip_info.gw));
    esp_err_t dns_err = esp_netif_get_dns_info(event->esp_netif,
                                               ESP_NETIF_DNS_MAIN,
                                               &s_w5500_dns_main);
    s_w5500_dns_valid = dns_err == ESP_OK &&
                         s_w5500_dns_main.ip.type == ESP_IPADDR_TYPE_V4 &&
                         s_w5500_dns_main.ip.u_addr.ip4.addr != 0;
    if (s_w5500_dns_valid) {
        ESP_LOGI(TAG, "W5500 DHCP DNS(Main): " IPSTR,
                 IP2STR(&s_w5500_dns_main.ip.u_addr.ip4));
    } else {
        ESP_LOGW(TAG, "W5500 DHCP did not provide a usable DNS server");
    }
    s_w5500_link_up = true;
    sx_network_manager_w5500_got_ip(&event->ip_info);
}

static void w5500_on_lost_ip(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_id;
    const ip_event_got_ip_t *event = event_data;
    if (event == NULL || event->esp_netif != s_eth_netif || s_static_address) return;
    ESP_LOGW(TAG, "W5500 lost IP address");
    s_w5500_dns_valid = false;
    sx_network_manager_w5500_lost_ip();
}

static esp_err_t w5500_apply_mac(esp_eth_handle_t handle)
{
    uint8_t mac_addr[6] = { 0 };
    esp_err_t err = esp_efuse_mac_get_default(mac_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "读取默认 MAC 失败: %s", esp_err_to_name(err));
        return err;
    }
    mac_addr[5] ^= 0x02; // 与 Wi-Fi 默认地址错开
    err = esp_eth_ioctl(handle, ETH_CMD_S_MAC_ADDR, mac_addr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置 W5500 MAC 失败: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "W5500 MAC 设置为 %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2],
             mac_addr[3], mac_addr[4], mac_addr[5]);
    return ESP_OK;
}

static esp_err_t w5500_configure_netif(bool lan_mode,
                                       const char *lan_ip,
                                       const char *lan_netmask,
                                       bool static_enabled,
                                       const char *gateway,
                                       const char *dns,
                                       bool dhcp_server_enabled)
{
    s_static_address = lan_mode || static_enabled;
    s_dhcp_server_enabled = lan_mode && dhcp_server_enabled;
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    if (lan_mode) {
        /* DHCP server state must be allocated by esp_netif_new(). */
        base.flags &= ~ESP_NETIF_DHCP_CLIENT;
        if (s_dhcp_server_enabled) base.flags |= ESP_NETIF_DHCP_SERVER;
    }
    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    netif_config.base = &base;
    s_eth_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_ERR_NO_MEM, TAG, "create netif failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue), TAG, "attach netif failed");
    if (s_static_address) {
        esp_netif_ip_info_t info = {0};
        ip4_addr_t ip = {0};
        ip4_addr_t netmask = {0};
        ESP_RETURN_ON_FALSE(lan_ip != NULL && lan_netmask != NULL &&
                            ip4addr_aton(lan_ip, &ip) &&
                            ip4addr_aton(lan_netmask, &netmask),
                            ESP_ERR_INVALID_ARG, TAG, "invalid Ethernet LAN IPv4 config");
        info.ip.addr = ip.addr;
        info.netmask.addr = netmask.addr;
        ip4_addr_t gateway_addr = {0};
        if (lan_mode) {
            /* Downstream DHCP clients route through this gateway, not the
             * gateway address configured for Ethernet uplink mode. */
            info.gw = info.ip;
        } else if (gateway != NULL && ip4addr_aton(gateway, &gateway_addr)) {
            info.gw.addr = gateway_addr.addr;
        } else {
            info.gw = info.ip;
        }
        if (s_dhcp_server_enabled) {
            ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(s_eth_netif), TAG,
                                "stop DHCP server before setting IP failed");
        } else if (!lan_mode) {
            esp_err_t err = esp_netif_dhcpc_stop(s_eth_netif);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) return err;
        }
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_eth_netif, &info), TAG,
                            "set Ethernet static IPv4 failed");
        if (dns != NULL) {
            ip4_addr_t dns_addr = {0};
            if (ip4addr_aton(dns, &dns_addr) && dns_addr.addr != 0) {
                esp_netif_dns_info_t dns_info = {0};
                dns_info.ip.type = ESP_IPADDR_TYPE_V4;
                dns_info.ip.u_addr.ip4.addr = dns_addr.addr;
                ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN,
                                                         &dns_info), TAG, "set DNS failed");
                if (s_dhcp_server_enabled) {
                    dhcps_offer_t offer_dns = OFFER_DNS;
                    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_eth_netif, ESP_NETIF_OP_SET,
                                        ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)),
                                        TAG, "enable DHCP DNS offer failed");
                }
            }
        }
    } else {
        esp_err_t err = esp_netif_dhcpc_start(s_eth_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) return err;
    }
    /* DHCP server starts on CONNECTED, after the netif is up. */
    return ESP_OK;
}

esp_err_t w5500_manager_init_with_role(bool lan_mode,
                                       const char *lan_ip,
                                       const char *lan_netmask,
                                       bool static_enabled,
                                       const char *gateway,
                                       const char *dns,
                                       bool dhcp_server_enabled)
{
    if (s_initialized) {
        return ESP_OK;
    }
    if (s_cleanup_failed) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_FALSE(!dhcp_server_enabled || lan_mode, ESP_ERR_INVALID_ARG, TAG,
                        "DHCP server requires Ethernet downlink mode");

    esp_err_t err;
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    bool bus_initialized = false;
    bool eth_event_registered = false;
    bool got_ip_registered = false;
    bool lost_ip_registered = false;
    spi_bus_config_t buscfg = {
        .mosi_io_num = W5500_PIN_MOSI,
        .miso_io_num = W5500_PIN_MISO,
        .sclk_io_num = W5500_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };
    err = spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) goto fail;
    bus_initialized = true;

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &s_w5500_devcfg);
    w5500_config.int_gpio_num = W5500_PIN_INT;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = W5500_PHY_ADDR;
    phy_config.reset_gpio_num = W5500_PIN_RST;

    if (!s_isr_service_installed && W5500_PIN_INT >= 0) {
        err = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL2);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
        /* The GPIO ISR service is shared with other peripherals; retain it. */
        s_isr_service_installed = true;
    }
    if (W5500_PIN_RST >= 0) {
        err = gpio_reset_pin(W5500_PIN_RST);
        if (err != ESP_OK) goto fail;
        err = gpio_set_direction(W5500_PIN_RST, GPIO_MODE_OUTPUT);
        if (err != ESP_OK) goto fail;
        err = gpio_set_level(W5500_PIN_RST, 1);
        if (err != ESP_OK) goto fail;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    if (mac == NULL) { err = ESP_ERR_NO_MEM; goto fail; }
    phy = esp_eth_phy_new_w5500(&phy_config);
    if (phy == NULL) { err = ESP_ERR_NO_MEM; goto fail; }
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    err = esp_eth_driver_install(&eth_config, &s_eth_handle);
    if (err != ESP_OK) goto fail;
    err = w5500_apply_mac(s_eth_handle);
    if (err != ESP_OK) goto fail;

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    if (s_eth_glue == NULL) { err = ESP_ERR_NO_MEM; goto fail; }
    err = w5500_configure_netif(lan_mode, lan_ip, lan_netmask, static_enabled,
                                gateway, dns, dhcp_server_enabled);
    if (err != ESP_OK) goto fail;
    err = esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_eth_event_handler, NULL);
    if (err != ESP_OK) goto fail;
    eth_event_registered = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &w5500_on_got_ip, NULL);
    if (err != ESP_OK) goto fail;
    got_ip_registered = true;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, &w5500_on_lost_ip, NULL);
    if (err != ESP_OK) goto fail;
    lost_ip_registered = true;
    err = esp_eth_start(s_eth_handle);
    if (err != ESP_OK) goto fail;

    s_initialized = true;
    ESP_LOGI(TAG, "W5500 初始化完成 role=%s spi=%dMHz (CS=%d MOSI=%d MISO=%d CLK=%d INT=%d)",
             lan_mode ? "static-lan" : (static_enabled ? "uplink-static" : "uplink-dhcp"),
             W5500_SPI_CLOCK_HZ / 1000000,
             W5500_PIN_CS, W5500_PIN_MOSI, W5500_PIN_MISO, W5500_PIN_SCLK, W5500_PIN_INT);
    return ESP_OK;

fail:
    ESP_LOGE(TAG, "W5500 init failed: %s; releasing partial initialization", esp_err_to_name(err));
    if (lost_ip_registered) esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_LOST_IP, &w5500_on_lost_ip);
    if (got_ip_registered) esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, &w5500_on_got_ip);
    if (eth_event_registered) esp_event_handler_unregister(ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_eth_event_handler);
    /* IDF can leave the FSM in START after esp_eth_start() fails midway. */
    if (s_eth_handle != NULL) (void)esp_eth_stop(s_eth_handle);
    if (s_eth_glue != NULL) {
        esp_eth_del_netif_glue(s_eth_glue);
        s_eth_glue = NULL;
    }
    if (s_eth_handle != NULL) {
        esp_err_t cleanup_err = esp_eth_driver_uninstall(s_eth_handle);
        if (cleanup_err != ESP_OK) {
            /* A live driver may still reference MAC/PHY, netif and SPI.
             * Retain those resources rather than freeing beneath it. */
            s_cleanup_failed = true;
            ESP_LOGE(TAG, "W5500 driver cleanup failed: %s; reboot required",
                     esp_err_to_name(cleanup_err));
            return err;
        }
        s_eth_handle = NULL;
    }
    if (s_eth_netif != NULL) {
        esp_netif_destroy(s_eth_netif);
        s_eth_netif = NULL;
    }
    if (phy != NULL) phy->del(phy);
    if (mac != NULL) mac->del(mac);
    if (bus_initialized) spi_bus_free(W5500_SPI_HOST);
    s_dhcp_server_enabled = false;
    s_static_address = false;
    s_w5500_dns_valid = false;
    s_w5500_link_up = false;
    return err;
}

esp_err_t w5500_manager_init(void)
{
    return w5500_manager_init_with_role(false, NULL, NULL, false, NULL, NULL, false);
}

esp_eth_handle_t w5500_manager_get_handle(void)
{
    return s_eth_handle;
}

esp_netif_t *w5500_manager_get_netif(void)
{
    return s_eth_netif;
}

bool w5500_manager_is_connected(void)
{
    return s_w5500_link_up;
}

esp_err_t w5500_manager_get_dns_info(esp_netif_dns_info_t *dns)
{
    if (dns == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_w5500_dns_valid) {
        return ESP_ERR_INVALID_STATE;
    }
    *dns = s_w5500_dns_main;
    return ESP_OK;
}
