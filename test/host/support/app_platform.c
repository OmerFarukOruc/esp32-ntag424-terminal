/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "hal_ble.h"
#include "hal_button.h"
#include "hal_diagnostics.h"
#include "hal_indicator.h"
#include "hal_network.h"
#include "hal_nfc.h"
#include "hal_power.h"
#include "hal_time.h"
#include "app_platform.h"
#include "host.h"

#define NVS_KEYS_MAX 4
#define NVS_VALUE_MAX 64
#define NVS_HANDLE_READONLY 1
#define NVS_HANDLE_READWRITE 2

struct host_trace trace;
jmp_buf host_sleep_jmp;

const hal_wifi_ap_t host_scan_ap = {
    .ssid = "CafeNet",
    .rssi = -42,
    .channel = 6,
    .auth = HAL_WIFI_AUTH_WPA3,
};

static struct {
    char key[16];
    bool committed;
    bool pending;
    char committed_value[NVS_VALUE_MAX];
    char pending_value[NVS_VALUE_MAX];
} nvs_keys[NVS_KEYS_MAX];
static bool nvs_namespace_created;

void host_nvs_reset(void)
{
    memset(nvs_keys, 0, sizeof nvs_keys);
    nvs_namespace_created = false;
}

static int nvs_slot(const char *key, bool create)
{
    for (int i = 0; i < NVS_KEYS_MAX; i++) {
        if (nvs_keys[i].key[0] != '\0' && strcmp(nvs_keys[i].key, key) == 0) {
            return i;
        }
    }
    for (int i = 0; create && i < NVS_KEYS_MAX; i++) {
        if (nvs_keys[i].key[0] == '\0') {
            snprintf(nvs_keys[i].key, sizeof nvs_keys[i].key, "%s", key);
            return i;
        }
    }
    return -1;
}

const char *host_nvs_committed(const char *key)
{
    int i = nvs_slot(key, false);
    return i >= 0 && nvs_keys[i].committed ? nvs_keys[i].committed_value : NULL;
}

esp_err_t nvs_flash_init(void)
{
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    host_nvs_reset();
    return ESP_OK;
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *h)
{
    if (mode == NVS_READONLY && !nvs_namespace_created) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    nvs_namespace_created = true;
    *h = mode == NVS_READWRITE ? NVS_HANDLE_READWRITE : NVS_HANDLE_READONLY;
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t h, const char *key, const char *value)
{
    if (h != NVS_HANDLE_READWRITE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (trace.nvs_set_err != ESP_OK) {
        return trace.nvs_set_err;
    }
    int i = nvs_slot(key, true);
    if (i < 0 || strlen(value) >= NVS_VALUE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    snprintf(nvs_keys[i].pending_value, NVS_VALUE_MAX, "%s", value);
    nvs_keys[i].pending = true;
    return ESP_OK;
}

esp_err_t nvs_get_str(nvs_handle_t h, const char *key, char *out, size_t *len)
{
    const char *value = host_nvs_committed(key);
    if (value == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    size_t need = strlen(value) + 1;
    if (out != NULL && *len < need) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (out != NULL) {
        memcpy(out, value, need);
    }
    *len = need;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t h)
{
    if (trace.nvs_commit_err != ESP_OK) {
        return trace.nvs_commit_err;
    }
    for (int i = 0; i < NVS_KEYS_MAX; i++) {
        if (nvs_keys[i].pending) {
            memcpy(nvs_keys[i].committed_value, nvs_keys[i].pending_value, NVS_VALUE_MAX);
            nvs_keys[i].committed = true;
            nvs_keys[i].pending = false;
        }
    }
    return ESP_OK;
}

void nvs_close(nvs_handle_t h)
{
    for (int i = 0; i < NVS_KEYS_MAX; i++) {
        nvs_keys[i].pending = false;
    }
}

esp_reset_reason_t esp_reset_reason(void)
{
    return ESP_RST_POWERON;
}

void esp_restart(void)
{
    abort();
}

esp_err_t esp_task_wdt_add(void *t)
{
    return ESP_OK;
}

esp_err_t esp_task_wdt_reset(void)
{
    return ESP_OK;
}

esp_err_t hal_ble_init(hal_ble_event_cb_t cb)
{
    return ESP_OK;
}

esp_err_t hal_ble_set_device_id(const char *id)
{
    if (trace.ble_set_id_err != ESP_OK) {
        return trace.ble_set_id_err;
    }
    snprintf(trace.published_id, sizeof trace.published_id, "%s", id);
    return ESP_OK;
}

esp_err_t hal_ble_start_advertising(void)
{
    return ESP_OK;
}

esp_err_t hal_ble_stop_advertising(void)
{
    return ESP_OK;
}

esp_err_t hal_ble_disconnect(void)
{
    return ESP_OK;
}

esp_err_t hal_ble_deinit(void)
{
    trace.ble_deinit_with_tasks = host_tasks_alive();
    trace.ble_deinit++;
    return trace.ble_deinit_err;
}

bool hal_ble_session_is_current(hal_ble_session_t s)
{
    return s != HAL_BLE_SESSION_NONE && s == trace.ble_current;
}

esp_err_t hal_ble_send_wifi_scan_results(hal_ble_session_t s, const hal_ble_wifi_scan_result_t *r, size_t n)
{
    if (trace.scan_sends < HOST_SCAN_SENDS_MAX) {
        struct host_scan_send *send = &trace.scan[trace.scan_sends];
        send->session = s;
        send->count = n;
        if (n > 0) {
            send->first = r[0];
        }
    }
    trace.scan_sends++;
    trace.ble_used_after_deinit |= trace.ble_deinit > 0;
    return ESP_OK;
}

esp_err_t hal_ble_send_wifi_config_result(hal_ble_session_t s, bool ok, const char *ip, const char *m)
{
    trace.wifi_results++;
    trace.last_wifi_ok = ok;
    return ESP_OK;
}

esp_err_t hal_ble_send_url_set_result(hal_ble_session_t s, bool ok, int32_t code, const char *m)
{
    trace.url_results++;
    trace.last_url_ok = ok;
    trace.last_url_code = code;
    snprintf(trace.last_url_msg, sizeof trace.last_url_msg, "%s", m);
    return ESP_OK;
}

esp_err_t hal_button_init(hal_button_event_cb_t cb)
{
    return ESP_OK;
}

esp_err_t hal_diagnostics_init(void)
{
    return ESP_OK;
}

void hal_diagnostics_record_transition(uint8_t from, uint8_t to, uint8_t event)
{
}

void hal_diagnostics_record_queue_overflow(void)
{
}

void hal_diagnostics_record_url_set_attempt(const hal_diagnostics_url_set_t *a)
{
    trace.diag_result_code = a->result_code;
}

esp_err_t hal_indicator_init(void)
{
    return ESP_OK;
}

esp_err_t hal_led_set_pattern(hal_led_pattern_t p)
{
    return ESP_OK;
}

esp_err_t hal_buzzer_play(hal_buzzer_sound_t s)
{
    return ESP_OK;
}

esp_err_t hal_network_init(void)
{
    return ESP_OK;
}

esp_err_t hal_network_deinit(void)
{
    trace.net_deinit_with_tasks = host_tasks_alive();
    trace.net_deinit++;
    return trace.net_deinit_err;
}

esp_err_t hal_wifi_connect(const hal_wifi_credentials_t *c)
{
    trace.connects++;
    memcpy(&trace.last_creds, c, sizeof * c);
    if (trace.on_connect != NULL) {
        void (*f)(void) = trace.on_connect;
        trace.on_connect = NULL;
        f();
    }
    return trace.connect_err;
}

hal_wifi_state_t hal_wifi_get_state(void)
{
    return trace.wifi_state;
}

void hal_wifi_register_callback(hal_wifi_event_cb_t cb)
{
}

esp_err_t hal_wifi_scan(hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX], uint16_t *count)
{
    trace.scans++;
    trace.wifi_used_after_deinit |= trace.net_deinit > 0;
    aps[0] = host_scan_ap;
    *count = 1;
    return ESP_OK;
}

esp_err_t hal_nfc_init(void)
{
    return ESP_OK;
}

bool hal_nfc_device_id_valid(const char *id, size_t len)
{
    return len > 0 && len <= HAL_NFC_DEVICE_ID_MAX_LEN;
}

hal_nfc_err_t hal_nfc_ntag424_configure_sdm(const char *id, uint32_t t)
{
    trace.nfc_writes++;
    return trace.nfc_result;
}

esp_err_t hal_power_init(void)
{
    return ESP_OK;
}

void hal_power_register_low_battery_cb(hal_low_battery_cb_t cb)
{
}

void hal_power_deep_sleep(void)
{
    trace.slept++;
    longjmp(host_sleep_jmp, 1);
}

esp_err_t hal_time_start_sync(void)
{
    return ESP_OK;
}

void hal_time_stop_sync(void)
{
}
