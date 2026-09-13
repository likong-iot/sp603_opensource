#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_async_uart.h"

#include "sx_gpio.h"

#include "sx_init_wifi.h"
#include "sx_auto_collect.h"
#include "sx_task.h"
#include "sx_timer_tasks.h"
#include "sx_time_manager.h"

#include "sx_utils.h"
#include "sx_web_server.h"
#include <inttypes.h> // 添加inttypes.h头文件用于PRIu32宏
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "MAIN";

// 全局变量存储当前UART配置模式
static uart_config_mode_t g_uart_config_mode = UART_CONFIG_MODE_NORMAL;

// 从NVS读取UART配置模式
static void load_uart_config_mode_from_nvs(void) {
    ESP_LOGI(TAG, "开始从NVS读取UART配置模式");
    
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "无法打开NVS存储读取UART配置模式: %s", esp_err_to_name(err));
        g_uart_config_mode = UART_CONFIG_MODE_NORMAL; // 默认为正常模式
        return;
    }
    
    // 读取串口同步模式配置
    char serial_sync_mode[32] = {0};
    size_t len = sizeof(serial_sync_mode);
    err = nvs_get_str(nvs_handle, "serialSyncMode", serial_sync_mode, &len);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "从NVS读取到串口同步模式: %s", serial_sync_mode);
        
        if (strcmp(serial_sync_mode, "slave_follow") == 0) {
            g_uart_config_mode = UART_CONFIG_MODE_SLAVE_FOLLOW;
            ESP_LOGI(TAG, "设置UART配置模式为：从机跟随模式");
        } else {
            g_uart_config_mode = UART_CONFIG_MODE_NORMAL;
            ESP_LOGI(TAG, "设置UART配置模式为：正常模式");
        }
    } else {
        ESP_LOGW(TAG, "从NVS读取串口同步模式失败: %s，默认使用正常模式", esp_err_to_name(err));
        g_uart_config_mode = UART_CONFIG_MODE_NORMAL;
    }
    
    nvs_close(nvs_handle);
    ESP_LOGI(TAG, "UART配置模式读取完成");
}

// 获取当前UART配置模式
uart_config_mode_t get_uart_config_mode(void) {
    return g_uart_config_mode;
}

// 设置UART配置模式
void set_uart_config_mode(uart_config_mode_t mode) {
    g_uart_config_mode = mode;
    ESP_LOGI(TAG, "UART配置模式已更新为: %s", 
             (mode == UART_CONFIG_MODE_SLAVE_FOLLOW) ? "从机跟随" : "正常模式");
}

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

// 工作模式初始化函数
void init_work_mode(void) {
  ESP_LOGI(TAG, "开始初始化工作模式");

  // 打开NVS存储
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("storage", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "无法打开NVS存储: %s", esp_err_to_name(err));
    return;
  }

  // 获取工作模式
  char work_mode[32] = {0};
  size_t len = sizeof(work_mode);
  bool nvs_handle_closed = false; // 标志NVS句柄是否已关闭

  err = nvs_get_str(nvs_handle, "w_mode", work_mode, &len);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "获取工作模式失败: %s，设置默认自动采集模式",
             esp_err_to_name(err));

    // 关闭只读句柄
    nvs_close(nvs_handle);
    nvs_handle_closed = true;

    // 重新以读写模式打开NVS进行配置保存
    nvs_handle_t nvs_write_handle;
    esp_err_t write_err = nvs_open("storage", NVS_READWRITE, &nvs_write_handle);
    if (write_err == ESP_OK) {
      // 保存默认工作模式为自动采集模式
      write_err = nvs_set_str(nvs_write_handle, "w_mode", "auto_collect");
      if (write_err == ESP_OK) {
        write_err = nvs_commit(nvs_write_handle);
        if (write_err == ESP_OK) {
          ESP_LOGI(TAG, "已保存默认工作模式为自动采集模式");
        } else {
          ESP_LOGE(TAG, "保存工作模式失败: %s", esp_err_to_name(write_err));
        }
      } else {
        ESP_LOGE(TAG, "设置工作模式失败: %s", esp_err_to_name(write_err));
      }
      nvs_close(nvs_write_handle);
    } else {
      ESP_LOGE(TAG, "打开NVS写入句柄失败: %s", esp_err_to_name(write_err));
    }

    // 设置为默认工作模式并继续执行
    strcpy(work_mode, "auto_collect");
  } else if (strcmp(work_mode, "auto_collect") != 0) {
    ESP_LOGW(TAG, "当前工作模式非自动采集，强制写回自动采集模式");
    nvs_close(nvs_handle);
    nvs_handle_closed = true;

    nvs_handle_t nvs_write_handle;
    esp_err_t write_err = nvs_open("storage", NVS_READWRITE, &nvs_write_handle);
    if (write_err == ESP_OK) {
      write_err = nvs_set_str(nvs_write_handle, "w_mode", "auto_collect");
      if (write_err == ESP_OK) {
        write_err = nvs_commit(nvs_write_handle);
      }
      if (write_err != ESP_OK) {
        ESP_LOGE(TAG, "覆盖工作模式失败: %s", esp_err_to_name(write_err));
      }
      nvs_close(nvs_write_handle);
    } else {
      ESP_LOGE(TAG, "打开NVS写入句柄失败: %s", esp_err_to_name(write_err));
    }
    strcpy(work_mode, "auto_collect");
  }

  ESP_LOGI(TAG, "当前工作模式: %s", work_mode);

  ESP_LOGI(TAG, "工作模式：自动采集模式");
  sx_auto_collect_init();

  // 关闭NVS句柄（如果还没有关闭的话）
  if (!nvs_handle_closed) {
    nvs_close(nvs_handle);
  }
  ESP_LOGI(TAG, "工作模式初始化完成");
}

void print_sockets(void) {
  int socket_count = 0;
  struct tcp_pcb *pcb;
  pcb = tcp_active_pcbs;

  while (pcb != NULL) {
    socket_count++;
    pcb = pcb->next;
  }

  ESP_LOGI(TAG, "Total active sockets: %d", socket_count);
}

// 内存监控定时器回调函数
static void memory_monitor_timer_callback(void *arg) {
  // 检查系统内存状态并尝试回收
  if (esp_get_free_heap_size() < 2097152) {   // 如果剩余内存小于2MB
    if (esp_get_free_heap_size() < 1048576) { // 如果剩余内存小于1MB
      ESP_LOGW(TAG, "Low memory detected,  free_heap_size<1MB:%d",
               esp_get_free_heap_size());
      return;
    }
    ESP_LOGW(TAG, "Low memory detected,  free_heap_size<2MB:%d",
             esp_get_free_heap_size());
  }
}

void app_main(void) {
  init_gpio();
  init_key_timer();

  // 初始化UART收发指示灯
  uart_led_init();
  // 启动，接收指示灯闪烁
  //  亮起所有接收指示灯（按通道：CH1/CH2/CH3）
  uart_rx_led_on(1);
  uart_rx_led_on(2);
  uart_rx_led_on(3);
  vTaskDelay(pdMS_TO_TICKS(1000));
  // 关闭所有接收指示灯
  uart_rx_led_off(1);
  uart_rx_led_off(2);
  uart_rx_led_off(3);

  // 打印启动信息和固件版本
  ESP_LOGI(TAG, "======================================");
  ESP_LOGI(TAG, "SX-IOT Serial Server v%s", VERSION);
  ESP_LOGI(TAG, "Build time: %s %s", __DATE__, __TIME__);
  ESP_LOGI(TAG, "======================================");

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());

  uint8_t sta_mac[6] = {0};
  esp_read_mac(sta_mac, ESP_MAC_WIFI_STA);
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 格式化MAC地址
  sprintf(device_sta_mac, "%02X:%02X:%02X:%02X:%02X:%02X", sta_mac[0],
          sta_mac[1], sta_mac[2], sta_mac[3], sta_mac[4], sta_mac[5]);

  ESP_LOGI(TAG, "Device STA MAC: %s", device_sta_mac);

  nvs_handle_t nvs_handle;
  ESP_ERROR_CHECK(nvs_open("storage", NVS_READWRITE, &nvs_handle));

  size_t netconn, is_dhcp, static_ip, static_netmask, static_gateway,
      static_dns1, static_dns2, wifi_ssid, wifi_password, ap_name, ap_password,
      ap_wait_time, use_http, http_port, httpconn, http_url, http_header,
      http_time, host_names, ntp_server, lgname, lgpwd, use_tcp, tcpconn;
  size_t tcp_server, tcp_port, tcp_send, tcp_time, device_sn, w_mode, ap_timeout;

  esp_err_t err = ESP_OK;
#define FETCH_STR_LEN(key, len_var)                                                \
  do {                                                                             \
    err = nvs_get_str(nvs_handle, key, NULL, &len_var);                            \
    if (err == ESP_ERR_NVS_NOT_FOUND) {                                            \
      nvs_not_found = true;                                                        \
    } else {                                                                       \
      ESP_ERROR_CHECK(err);                                                        \
    }                                                                              \
  } while (0)

  while (true) {
    bool nvs_not_found = false;

    FETCH_STR_LEN("netconn", netconn);
    FETCH_STR_LEN("is_dhcp", is_dhcp);
    FETCH_STR_LEN("static_ip", static_ip);
    FETCH_STR_LEN("static_netmask", static_netmask);
    FETCH_STR_LEN("static_gateway", static_gateway);
    FETCH_STR_LEN("static_dns1", static_dns1);
    FETCH_STR_LEN("static_dns2", static_dns2);
    FETCH_STR_LEN("wifi_ssid", wifi_ssid);
    FETCH_STR_LEN("wifi_password", wifi_password);
    FETCH_STR_LEN("ap_name", ap_name);
    FETCH_STR_LEN("ap_password", ap_password);
    FETCH_STR_LEN("ap_wait_time", ap_wait_time);
    FETCH_STR_LEN("use_http", use_http);
    FETCH_STR_LEN("http_port", http_port);
    FETCH_STR_LEN("httpconn", httpconn);
    FETCH_STR_LEN("http_url", http_url);
    FETCH_STR_LEN("http_header", http_header);
    FETCH_STR_LEN("http_time", http_time);
    FETCH_STR_LEN("host_names", host_names);
    FETCH_STR_LEN("ntp_server", ntp_server);
    FETCH_STR_LEN("lgname", lgname);
    FETCH_STR_LEN("lgpwd", lgpwd);
    FETCH_STR_LEN("use_tcp", use_tcp);
    FETCH_STR_LEN("tcpconn", tcpconn);
    FETCH_STR_LEN("tcp_server", tcp_server);
    FETCH_STR_LEN("tcp_port", tcp_port);
    FETCH_STR_LEN("tcp_send", tcp_send);
    FETCH_STR_LEN("tcp_time", tcp_time);
    FETCH_STR_LEN("device_sn", device_sn);
    FETCH_STR_LEN("w_mode", w_mode);
    FETCH_STR_LEN("ap_timeout", ap_timeout);

    if (!nvs_not_found) {
      break;
    }

    ESP_LOGW(TAG, "检测到NVS配置缺失，写入默认值");
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "netconn", "2"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "is_dhcp", "1"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_ip", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_netmask", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_gateway", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns1", "8.8.8.8"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "static_dns2", "114.114.114.114"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_ssid", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "wifi_password", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_name", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_password", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_wait_time", "10"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_http", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_port", "5000"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "httpconn", "1"));
    ESP_ERROR_CHECK(
        nvs_set_str(nvs_handle, "http_url", "http://demo.likong.com"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_header", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "http_time", "5"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "host_names", "两路缓存485集线器"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ntp_server", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgname", "admin"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "lgpwd", "12345678"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "use_tcp", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcpconn", ""));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_server", "192.168.1.100"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_port", "8888"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_send", "0"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "tcp_time", "5"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "device_type", "RS5201"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "device_sn", "0000000000000"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "w_mode", "auto_collect"));
    ESP_ERROR_CHECK(nvs_set_str(nvs_handle, "ap_timeout", "30"));
    ESP_ERROR_CHECK(nvs_commit(nvs_handle));
    // 循环重新读取各字段长度，直至全部存在
  }
#undef FETCH_STR_LEN
  // 分配缓冲区
  char *nvs_netconn = malloc(netconn);
  char *nvs_is_dhcp = malloc(is_dhcp);
  char *nvs_static_ip = malloc(static_ip);
  char *nvs_static_netmask = malloc(static_netmask);
  char *nvs_static_gateway = malloc(static_gateway);
  char *nvs_wifi_ssid = malloc(wifi_ssid);
  char *nvs_wifi_password = malloc(wifi_password);
  if (!nvs_netconn || !nvs_is_dhcp || !nvs_static_ip || !nvs_static_netmask ||
      !nvs_static_gateway || !nvs_wifi_ssid || !nvs_wifi_password) {
    ESP_LOGE(TAG, "Memory allocation failed");
    // 释放已分配的内存
    if (nvs_netconn)
      free(nvs_netconn);
    if (nvs_is_dhcp)
      free(nvs_is_dhcp);
    if (nvs_static_ip)
      free(nvs_static_ip);
    if (nvs_static_netmask)
      free(nvs_static_netmask);
    if (nvs_static_gateway)
      free(nvs_static_gateway);
    if (nvs_wifi_ssid)
      free(nvs_wifi_ssid);
    if (nvs_wifi_password)
      free(nvs_wifi_password);
    esp_restart();
  }
  if (nvs_netconn == NULL) {
    printf("Memory allocation failed\n");
    esp_restart();
  } else {
    // 读取字符串到分配的缓冲区
    ret = nvs_get_str(nvs_handle, "netconn", nvs_netconn, &netconn);
    nvs_get_str(nvs_handle, "is_dhcp", nvs_is_dhcp, &is_dhcp);
    nvs_get_str(nvs_handle, "static_ip", nvs_static_ip, &static_ip);
    nvs_get_str(nvs_handle, "static_netmask", nvs_static_netmask,
                &static_netmask);
    nvs_get_str(nvs_handle, "static_gateway", nvs_static_gateway,
                &static_gateway);
    nvs_get_str(nvs_handle, "wifi_ssid", nvs_wifi_ssid, &wifi_ssid);
    nvs_get_str(nvs_handle, "wifi_password", nvs_wifi_password, &wifi_password);

    if (ret == ESP_OK) {
      // 更新全局变量，用于LED控制
      extern char nvs_netconn[10];
      // 使用临时变量避免restrict错误
      char temp_value[10];
      strncpy(temp_value, nvs_netconn, sizeof(temp_value) - 1);
      temp_value[sizeof(temp_value) - 1] = '\0'; // 确保字符串结束
      strncpy(nvs_netconn, temp_value, sizeof(nvs_netconn) - 1);
      nvs_netconn[sizeof(nvs_netconn) - 1] = '\0'; // 确保字符串结束
      start_wifi_task(nvs_wifi_ssid, nvs_wifi_password);
    } else {
      printf("Error reading 'my_key': %s\n", esp_err_to_name(ret));
    }
    free(nvs_netconn); // 释放缓冲区
    free(nvs_is_dhcp);
    free(nvs_static_ip);
    free(nvs_static_netmask);
    free(nvs_static_gateway);
    free(nvs_wifi_ssid);
    free(nvs_wifi_password);
  }
  nvs_close(nvs_handle);
  uart_init();
  create_multi_uart_rx_tasks(); // 创建多UART接收任务
  simple_task();
  http_server_init();
  
  // 初始化时间管理器（在WiFi和Web服务器启动之后）
  time_manager_init();
  ESP_LOGI(TAG, "时间管理器已初始化，等待Web端同步时间");
  
  // 初始化工作模式（在Web服务器启动后）
  init_work_mode();
  // 加载UART配置模式
  load_uart_config_mode_from_nvs();
  // 初始化定时器任务
  init_timer_tasks();

  // 初始化并启动内存监控定时器
  esp_timer_handle_t memory_monitor_timer;
  const esp_timer_create_args_t memory_monitor_timer_args = {
      .callback = &memory_monitor_timer_callback, .name = "memory_monitor"};
  ESP_ERROR_CHECK(
      esp_timer_create(&memory_monitor_timer_args, &memory_monitor_timer));
  ESP_ERROR_CHECK(esp_timer_start_periodic(memory_monitor_timer,
                                           60000000)); // 每60秒运行一次

  // 提高主任务优先级，确保看门狗及时重置
  vTaskPrioritySet(NULL, 24);
  ESP_LOGI(TAG, "主任务优先级已提升到24");
  
  // 将主任务注册到看门狗监控
  esp_task_wdt_add(NULL);
  ESP_LOGI(TAG, "主任务已注册到看门狗监控");
  // 用于控制socket打印的变量
  int i = 0;
  while (1) {
    // 重置看门狗 - 统一管理看门狗重置，只在此处重置系统看门狗
    esp_task_wdt_reset();
    // 循环次n打印一次
    i++;
    if (i == 20) {
      print_sockets();
      print_system_resource_usage();
      i = 0;
    }
    // 短延时，确保看门狗及时重置
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
