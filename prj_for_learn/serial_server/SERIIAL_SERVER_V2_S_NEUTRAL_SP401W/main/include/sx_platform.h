/*
 * @Author: Orion
 * @Date: 2024-04-02 10:23:09
 * @LastEditors: Orion
 * @LastEditTime: 2025-06-11 13:37:11
 * @FilePath: \ETH_TH\main\include\sx_time.h
 * @Description:
 *
 * Copyright (c) 2024 by SX-IOT, All Rights Reserved.
 */
#pragma once

#include <stdint.h> // Include for uint8_t type definition
#include <stdlib.h> // Include for size_t

#ifdef __cplusplus
extern "C" {
#endif

// 全局变量声明
extern char strftime_buf[64];

// 加密函数声明
char *encrypt_data(const char *plaintext);
char *decrypt_data(const char *ciphertext_b64);
char *create_encrypted_json(const char *original_json);
void generate_key(const char *salt, uint8_t *key, size_t key_len);

// 时间处理函数
void init_load_time_from_nvs(void);

// HTTP请求处理函数
void http_rest_with_url_platform(char *url, char *header);
int http_rest_with_url_heartbeat(char *url, char *header);

#ifdef __cplusplus
}
#endif
