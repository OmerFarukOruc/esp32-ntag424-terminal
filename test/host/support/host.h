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
#include "host/ble_hs.h"

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
extern int host_task_wakeups;
extern jmp_buf *host_task_blocks;
extern void (*host_on_delay)(void);
void host_run_tasks(void);
unsigned char *host_queue_storage(QueueHandle_t q, size_t *bytes);

void host_event_loop_run(void);
void host_wifi_reset(void);
extern int wifi_connect_calls;
extern int wifi_disconnect_calls;
extern esp_err_t wifi_connect_result;
extern esp_err_t wifi_disconnect_result;
extern esp_err_t wifi_stop_result;
extern esp_err_t wifi_deinit_result;
extern int host_event_post_fails;

#define HOST_NOTIFY_MAX 16
#define HOST_NOTIFY_LEN 600
extern uint16_t host_mtu;
extern int host_notify_count;
extern char host_notify_payload[HOST_NOTIFY_MAX][HOST_NOTIFY_LEN];
extern int host_notify_conn[HOST_NOTIFY_MAX];
extern int host_notify_fail_at;
extern bool host_notify_enomem_once;
extern bool host_notify_enomem_always;
extern void (*host_on_enomem)(void);
extern void (*host_before_alloc)(void);
extern int host_stalled_waits;
extern int host_conn_id;
extern int host_terminations;
extern int host_mbuf_live;
extern int host_port_stop_rc;
extern int host_port_deinit_rc;
extern int host_callout_init_rc;
extern int host_add_svcs_rc;
extern int host_callouts_released_armed;
extern int64_t host_put_delay_us;
extern int host_adv_starts;
extern int host_terminate_rc;
int host_fire_callouts(void);
extern int host_notify_calls;
extern void (*host_on_enomem_always)(void);
void host_notify_reset(void);
void host_run_host(void);
void host_post_gap(ble_gap_event_fn *fn, struct ble_gap_event ev);

extern bool host_in_host;
extern bool host_blocked_in_host;
extern void (*host_on_block)(void);

extern jmp_buf host_hang_jmp;
extern bool host_hang_armed;
extern int64_t host_hang_limit_us;
extern int64_t host_byte_gap_us;
extern long host_bytes_read;

#define HOST_TX_LOG_MAX 512
extern bool host_uart_installed;
extern int host_uart_installs;
extern uint8_t host_tx_log[HOST_TX_LOG_MAX];
extern size_t host_bytes_written;

extern int host_gpio_level;
extern gpio_isr_t host_gpio_isr;
