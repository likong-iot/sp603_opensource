/*
 * @Author: Orion
 * @Date: 2024-02-03 11:29:35
 * @LastEditors: Orion
 * @LastEditTime: 2025-05-21 14:18:52
 * @FilePath: \SERIIAL_SERVER_P\main\include\sx_gpio.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */

#ifndef SX_GPIO_H
#define SX_GPIO_H

#include "driver/gpio.h"

// 按键引脚定义 - 使用GPIO1
#define KEY GPIO_NUM_1

// 函数声明
void init_gpio(void);

#endif // SX_GPIO_H
