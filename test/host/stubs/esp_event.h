/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
typedef void *esp_event_handler_instance_t;
typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);
extern esp_event_base_t const WIFI_EVENT;
extern esp_event_base_t const IP_EVENT;
#define ESP_EVENT_ANY_ID -1
esp_err_t esp_event_loop_create_default(void);
esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id, esp_event_handler_t h, void *arg, esp_event_handler_instance_t *out);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b, int32_t id, esp_event_handler_instance_t inst);
#define ESP_EVENT_DEFINE_BASE(id) esp_event_base_t const id = #id
esp_err_t esp_event_post(esp_event_base_t b, int32_t id, const void *data, size_t size, uint32_t ticks);
void host_event_loop_run(void);
