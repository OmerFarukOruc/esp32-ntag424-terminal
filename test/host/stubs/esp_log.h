/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdio.h>
#include "esp_err.h"
extern int host_log_enabled;
#define HOST_LOG(l, tag, fmt, ...) do { if (host_log_enabled) fprintf(stderr, l " (%s) " fmt "\n", tag, ##__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) ((void)0)
