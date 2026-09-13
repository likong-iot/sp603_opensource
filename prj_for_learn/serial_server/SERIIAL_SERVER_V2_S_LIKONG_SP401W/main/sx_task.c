/*
 * @Author: Orion
 * @Date: 2024-02-03 11:29:54
 * @LastEditors: Orion
 * @LastEditTime: 2025-03-07 13:41:02
 * @FilePath: \SERIIAL_SERVER\main\sx_task.c
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sx_gpio.h"
#include "sx_http_client.h"
#include "sx_utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*************************************************************************************/
/*                                START OF FILE */
/*************************************************************************************/

void led_task(void *param) {
  while (1) {
    gpio_set_level(LED_DAT, 0);
    // gpio_set_level(LED0, 0);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
    gpio_set_level(LED_DAT, 1);
    // gpio_set_level(LED0, 1);
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

void key_task(void *param) {
  while (1) {
    if (gpio_get_level(KEY) == 0) {         // 按键按下
      vTaskDelay(100 / portTICK_PERIOD_MS); // 消抖
      if ((gpio_get_level(KEY) == 0) && (KeyState == 0)) {
        KeyState = 1;
        // LED指示灯切换状态
        if (LED_DAT_STATE == 0) {
          // LED_DAT_ON();
        } else {
          // LED_DAT_OFF();
        }

        // 等待按键释放或超时
        int press_time = 0;
        while (gpio_get_level(KEY) == 0 && press_time < 300) {
          vTaskDelay(10 / portTICK_PERIOD_MS);
          press_time++;
        }

        if (press_time >= 300) { // 长按超过3秒
          printf("RESET ALL!\n");
          perform_factory_reset(true);
        } else if (gpio_get_level(KEY) == 1) { // 短按后释放
          printf("RESTART!\n");
          vTaskDelay(1000 / portTICK_PERIOD_MS);
          esp_restart();
        }
      }
    } else {
      KeyState = 0;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  vTaskDelete(NULL);
}

// void key_task(void *param) {
//   while (1) {
//     if (gpio_get_level(KEY) == 0) {
//       vTaskDelay(100 / portTICK_PERIOD_MS);
//       if ((gpio_get_level(KEY) == 0) && (KeyState == 0)) {
//         KeyState = 1;
//         if (LED_DAT_STATE == 0) {
//           // LED_DAT_ON();
//         } else {
//           // LED_DAT_OFF();
//         }
//         for (int i = 0; i < 300; i++) {
//           vTaskDelay(10 / portTICK_PERIOD_MS);
//           if (gpio_get_level(KEY) == 1) {
//             break;
//           }
//         }
//         if (gpio_get_level(KEY) == 0) {
//         printf("RESET ALL!\n");
//         // esp_wifi_restore();
//         LED_DAT_ON();
//         LED_WIFI_ON();
//         LED_LAN_ON();
//         vTaskDelay(1000 / portTICK_PERIOD_MS);
//         LED_DAT_OFF();
//         LED_WIFI_OFF();
//         LED_LAN_OFF();
//         vTaskDelay(1000 / portTICK_PERIOD_MS);
//         LED_DAT_ON();
//         LED_WIFI_ON();
//         LED_LAN_ON();
//         vTaskDelay(1000 / portTICK_PERIOD_MS);
//         LED_DAT_OFF();
//         LED_WIFI_OFF();
//         LED_LAN_OFF();
//         ESP_ERROR_CHECK(nvs_flash_erase());
//         vTaskDelay(300 / portTICK_PERIOD_MS);
//         esp_restart();
//         }
//       }
//     } else {
//       KeyState = 0;
//     }
//   }
//   vTaskDelete(NULL);
// }

// 增加任务内存使用统计
// void enhanced_memory_monitor(void) {
//     // 堆内存信息
//     multi_heap_info_t info;
//     heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);

//     printf("堆内存总大小: %ld\n", info.total_free_bytes +
//     info.total_allocated_bytes); printf("当前已分配: %ld\n",
//     info.total_allocated_bytes); printf("最大连续可分配: %ld\n",
//     info.largest_free_block); printf("最小空闲块: %ld\n",
//     info.minimum_free_bytes); printf("碎片化程度: %.2f%%\n", 100 -
//     (info.largest_free_block * 100.0f / info.total_free_bytes));

//     // 任务栈使用情况
//     UBaseType_t uxArraySize = uxTaskGetNumberOfTasks();
//     TaskStatus_t *pxTaskStatusArray = pvPortMalloc(uxArraySize *
//     sizeof(TaskStatus_t)); if (pxTaskStatusArray != NULL) {
//         uxArraySize = uxTaskGetSystemState(pxTaskStatusArray, uxArraySize,
//         NULL); for (int i = 0; i < uxArraySize; i++) {
//             printf("任务名: %s, 栈高水位: %d\n",
//                    pxTaskStatusArray[i].pcTaskName,
//                    pxTaskStatusArray[i].usStackHighWaterMark);
//         }
//         vPortFree(pxTaskStatusArray);
//     }
// }

void close_ap_task(void *pvParameter) {
  // 等待五分钟
  vTaskDelay(10 * 60 * 1000 / portTICK_PERIOD_MS);
  // vTaskDelay(5000);
  printf("close success\n");
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  // 关闭AP模式，保持STA连接
  // esp_wifi_set_mode(WIFI_MODE_STA);

  // 删除这个任务
  vTaskDelete(NULL);
}

/*************************************************************************************/
/*                                END OF FILE */
/*************************************************************************************/
