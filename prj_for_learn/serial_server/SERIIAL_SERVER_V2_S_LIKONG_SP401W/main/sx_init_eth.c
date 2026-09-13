/*
 * @Author: Orion
 * @Date: 2024-01-23 15:57:22
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 13:53:51
 * @FilePath: \ETH_TH\main\sx_init_eth.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sx_udp_server.h"
#include "sx_utils.h"
#include "sx_web_server.h"
#include "driver/gpio.h"
#include "sx_gpio.h"
#include <string.h>
#include "esp_timer.h"
static const char *TAG = "ETH-TH";

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

/** Event handler for Ethernet events */
void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                       void *event_data) {
  uint8_t mac_addr[6] = {0};
  /* we can get the ethernet driver handle from event data */
  esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

  switch (event_id) {
  case ETHERNET_EVENT_CONNECTED:
    esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
    ESP_LOGI(TAG, "Ethernet Link Up");
    ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4],
             mac_addr[5]);
    sprintf(device_mac, "%02X%02X%02X%02X%02X%02X", mac_addr[0], mac_addr[1],
            mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    sprintf(device_mac_end, "%02X%02X", mac_addr[4], mac_addr[5]);
    break;
  case ETHERNET_EVENT_DISCONNECTED:
    ESP_LOGI(TAG, "Ethernet Link Down");
    // 停止并删除LED闪烁定时器，确保LED常亮
    extern esp_timer_handle_t time_sync_led_timer;
    if (time_sync_led_timer != NULL) {
        ESP_LOGI(TAG, "以太网断开：停止并删除LED闪烁定时器");
        esp_err_t stop_err = esp_timer_stop(time_sync_led_timer);
        if (stop_err != ESP_OK && stop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "停止LED闪烁定时器失败: %s", esp_err_to_name(stop_err));
        }

        // 删除定时器并设置为NULL，防止重复创建导致闪烁异常
        esp_err_t delete_err = esp_timer_delete(time_sync_led_timer);
        if (delete_err != ESP_OK) {
            ESP_LOGW(TAG, "删除LED闪烁定时器失败: %s", esp_err_to_name(delete_err));
        }
        time_sync_led_timer = NULL;

        // 确保LED长灭
        LED_LAN_OFF(); // 以太网断开时LED应该长灭
    } else {
        // 即使定时器为NULL，也确保LED长灭
        LED_LAN_OFF(); // 以太网断开时LED应该长灭
    }

    // 标记网络已断开
    extern bool is_network_connected;
    extern bool is_intranet_mode;
    extern bool is_wifi_active;
    is_network_connected = false;
    is_intranet_mode = false;
    is_wifi_active = false;

    // 在网络断开时将IP地址相关信息设置为占位符
    strcpy(got_ip_addrs, "---");
    strcpy(got_ip_netmask, "---");
    strcpy(got_ip_gw, "---");
    ESP_LOGI(TAG, "以太网断开，IP显示重置为占位符");

    eth_disconnect();
    kill_udp_server();
    break;
  case ETHERNET_EVENT_START:
    ESP_LOGI(TAG, "Ethernet Started");
    break;
  case ETHERNET_EVENT_STOP:
    ESP_LOGI(TAG, "Ethernet Stopped");
    break;
  default:
    ESP_LOGI(TAG, "Unknown Ethernet Event");
    break;
  }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
