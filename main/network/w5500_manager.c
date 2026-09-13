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
static bool s_w5500_link_up = false;
static bool s_isr_service_installed = false;
static esp_eth_handle_t s_eth_handle = NULL;
static esp_eth_netif_glue_handle_t s_eth_glue = NULL;
static esp_netif_t *s_eth_netif = NULL;
static esp_netif_dns_info_t s_w5500_dns_main = {0};
static bool s_w5500_dns_valid = false;
static spi_device_interface_config_t s_w5500_devcfg = {
    .command_bits = 0,
    .address_bits = 0,
    .dummy_bits = 0,
    .mode = 0,
    .clock_speed_hz = W5500_SPI_CLOCK_HZ,
    .spics_io_num = W5500_PIN_CS,
    .queue_size = 32,
};

static void w5500_eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    uint8_t mac[6] = { 0 };
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "W5500 Link Up, MAC=%02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        s_w5500_link_up = true;
        sx_network_manager_w5500_link_up();
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "W5500 Link Down");
        s_w5500_link_up = false;
        sx_network_manager_w5500_link_down();
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "W5500 Started");
        break;
    case ETHERNET_EVENT_STOP:
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

    spi_bus_config_t buscfg = {
        .mosi_io_num = W5500_PIN_MOSI,
        .miso_io_num = W5500_PIN_MISO,
        .sclk_io_num = W5500_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(W5500_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI bus init failed");

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(W5500_SPI_HOST, &s_w5500_devcfg);
    w5500_config.int_gpio_num = W5500_PIN_INT;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = W5500_PHY_ADDR;
    phy_config.reset_gpio_num = W5500_PIN_RST;

    if (!s_isr_service_installed && W5500_PIN_INT >= 0) {
        // 使用较高等级的中断，避免与 USB Host 争抢 Level-1 资源
        esp_err_t isr_ret = gpio_install_isr_service(ESP_INTR_FLAG_LEVEL2);
        if (isr_ret != ESP_OK && isr_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "gpio_install_isr_service failed (%s)", esp_err_to_name(isr_ret));
            return isr_ret;
        }
        s_isr_service_installed = true;
    }

    if (W5500_PIN_RST >= 0) {
        gpio_reset_pin(W5500_PIN_RST);
        gpio_set_direction(W5500_PIN_RST, GPIO_MODE_OUTPUT);
        gpio_set_level(W5500_PIN_RST, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
    ESP_RETURN_ON_FALSE(mac, ESP_ERR_NO_MEM, TAG, "create MAC failed");
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);
    if (!phy) {
        mac->del(mac);
        ESP_LOGE(TAG, "create PHY failed");
        return ESP_ERR_NO_MEM;
    }

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &s_eth_handle), TAG, "driver install failed");
    ESP_RETURN_ON_ERROR(w5500_apply_mac(s_eth_handle), TAG, "set MAC failed");

    s_eth_glue = esp_eth_new_netif_glue(s_eth_handle);
    ESP_RETURN_ON_FALSE(s_eth_glue != NULL, ESP_ERR_NO_MEM, TAG, "create glue failed");

    esp_netif_config_t netif_config = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_config);
    ESP_RETURN_ON_FALSE(s_eth_netif != NULL, ESP_ERR_NO_MEM, TAG, "create netif failed");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_eth_netif, s_eth_glue), TAG, "attach netif failed");
    if (lan_mode || static_enabled) {
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
        if (gateway != NULL && ip4addr_aton(gateway, &gateway_addr)) info.gw.addr = gateway_addr.addr; else info.gw = info.ip;
        esp_err_t dhcp_err = esp_netif_dhcpc_stop(s_eth_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            return dhcp_err;
        }
        ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_eth_netif, &info), TAG,
                            "set Ethernet LAN IPv4 failed");
        if (dns != NULL) { ip4_addr_t dns_addr = {0}; esp_netif_dns_info_t dns_info = {0}; if (ip4addr_aton(dns, &dns_addr)) { dns_info.ip.type = ESP_IPADDR_TYPE_V4; dns_info.ip.u_addr.ip4.addr = dns_addr.addr; esp_netif_set_dns_info(s_eth_netif, ESP_NETIF_DNS_MAIN, &dns_info); } }
        if (dhcp_server_enabled) {
            dhcp_err = esp_netif_dhcps_start(s_eth_netif);
            if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                return dhcp_err;
            }
        } else {
            dhcp_err = esp_netif_dhcps_stop(s_eth_netif);
            if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                return dhcp_err;
            }
        }
    } else {
        esp_err_t dhcp_err = esp_netif_dhcpc_start(s_eth_netif);
        if (dhcp_err != ESP_OK && dhcp_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            return dhcp_err;
        }
    }

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &w5500_eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &w5500_on_got_ip, NULL));

    ESP_RETURN_ON_ERROR(esp_eth_start(s_eth_handle), TAG, "eth start failed");

    s_initialized = true;
    ESP_LOGI(TAG, "W5500 初始化完成 role=%s spi=%dMHz (CS=%d MOSI=%d MISO=%d CLK=%d INT=%d)",
             lan_mode ? "static-lan" : "uplink-dhcp",
             W5500_SPI_CLOCK_HZ / 1000000,
             W5500_PIN_CS, W5500_PIN_MOSI, W5500_PIN_MISO, W5500_PIN_SCLK, W5500_PIN_INT);
    return ESP_OK;
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
