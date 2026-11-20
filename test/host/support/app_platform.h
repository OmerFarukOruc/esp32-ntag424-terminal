/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <setjmp.h>
#include "hal_ble.h"
#include "hal_network.h"
#include "hal_nfc.h"

#define HOST_SCAN_SENDS_MAX 8

struct host_scan_send {
    hal_ble_session_t session;
    size_t count;
    hal_ble_wifi_scan_result_t first;
};

struct host_trace {
    esp_err_t nvs_set_err;
    esp_err_t nvs_commit_err;
    esp_err_t ble_set_id_err;
    char published_id[HAL_BLE_DEVICE_ID_MAX];
    int ble_deinit;
    int ble_deinit_with_tasks;
    int net_deinit;
    int net_deinit_with_tasks;
    int slept;
    int scan_sends;
    struct host_scan_send scan[HOST_SCAN_SENDS_MAX];
    bool wifi_used_after_deinit;
    bool ble_used_after_deinit;
    int wifi_results;
    bool last_wifi_ok;
    int url_results;
    bool last_url_ok;
    int32_t last_url_code;
    char last_url_msg[64];
    int32_t diag_result_code;
    int connects;
    esp_err_t connect_err;
    hal_wifi_state_t wifi_state;
    hal_wifi_credentials_t last_creds;
    hal_nfc_err_t nfc_result;
    void (*on_connect)(void);
};

extern struct host_trace trace;
extern jmp_buf host_sleep_jmp;
extern const hal_wifi_ap_t host_scan_ap;

void host_nvs_reset(void);
const char *host_nvs_committed(const char *key);
