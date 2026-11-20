/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

extern int64_t host_now_us;

struct host_timer {
    esp_timer_create_args_t args;
    bool used;
    bool armed;
    bool periodic;
};
struct host_timer *host_timer_named(const char *name);
bool host_timer_fire(struct host_timer *t);

int host_tasks_alive(void);
void host_run_tasks(void);
unsigned char *host_queue_storage(QueueHandle_t q, size_t *bytes);

void host_event_loop_run(void);
void host_wifi_reset(void);
extern int wifi_connect_calls;
extern int wifi_disconnect_calls;
extern esp_err_t wifi_connect_result;
extern esp_err_t wifi_disconnect_result;
extern int host_event_post_fails;

#define HOST_NOTIFY_MAX 16
#define HOST_NOTIFY_LEN 600
extern uint16_t host_mtu;
extern int host_notify_count;
extern char host_notify_payload[HOST_NOTIFY_MAX][HOST_NOTIFY_LEN];
extern int host_notify_fail_at;
extern bool host_notify_enomem_once;
extern void (*host_on_enomem)(void);
void host_notify_reset(void);

extern jmp_buf host_hang_jmp;
extern bool host_hang_armed;
extern int64_t host_hang_limit_us;
extern int64_t host_byte_gap_us;
extern long host_bytes_read;

extern int host_gpio_level;
extern gpio_isr_t host_gpio_isr;
