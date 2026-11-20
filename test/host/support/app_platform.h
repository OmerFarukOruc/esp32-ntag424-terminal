/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <setjmp.h>
#include "hal_network.h"
#include "hal_nfc.h"
struct host_trace {
    int queue_cap, task_create_fails, nvs_open_err, nvs_set_err, nvs_commit_err, ble_set_id_err, published_id_calls;
    int ble_deinit, ble_deinit_with_tasks, net_deinit, net_deinit_with_tasks, slept, scan_sends, wifi_used_after_deinit, ble_used_after_deinit;
    int wifi_results, last_wifi_ok, url_results, last_url_ok, last_url_code; char last_url_msg[64];
    int diag_result_code, diag_nfc_err, led, sound, connects, connect_err; hal_wifi_state_t wifi_state; hal_wifi_credentials_t last_creds;
    hal_nfc_err_t nfc_result; void *ble_cb, *button_cb, *battery_cb; void (*on_connect)(void);
};
extern struct host_trace trace;
extern jmp_buf host_sleep_jmp;
int host_tasks_alive(void);
void host_run_tasks(void);
unsigned char *host_queue_storage(void *q, size_t *bytes);
