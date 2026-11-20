/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "hal_ble.h"
#include "hal_button.h"
#include "hal_diagnostics.h"
#include "hal_indicator.h"
#include "hal_network.h"
#include "hal_nfc.h"
#include "hal_power.h"
#include "hal_time.h"
#include "app_platform.h"
struct host_trace trace;
struct queue {
    size_t item, cap, head, count;
    unsigned char *buf;
};
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item)
{
    struct queue *q = calloc(1, sizeof * q);
    q->item = item;
    q->cap = trace.queue_cap ? trace.queue_cap : len;
    q->buf = calloc(len, item);
    return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *it, TickType_t w)
{
    struct queue *q = h;
    if (q->count == q->cap) {
        return pdFALSE;
    } memcpy(q->buf + ((q->head + q->count) % q->cap) * q->item, it, q->item);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *it, TickType_t w)
{
    struct queue *q = h;
    if (!q->count) {
        return pdFALSE;
    } memcpy(it, q->buf + q->head * q->item, q->item);
    q->head = (q->head + 1) % q->cap;
    q->count--;
    return pdTRUE;
}
void vQueueDelete(QueueHandle_t h)
{
    struct queue *q = h;
    if (q) {
        free(q->buf);
        free(q);
    }
}
unsigned char *host_queue_storage(QueueHandle_t h, size_t *bytes)
{
    struct queue *q = h;
    *bytes = q->cap * q->item;
    return q->buf;
}
struct host_task {
    TaskFunction_t fn;
    void *arg;
    int alive;
} tasks[16]; int ntasks;
static jmp_buf task_exit; static int in_task;
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t st, void *arg, UBaseType_t p, TaskHandle_t *out)
{
    if (trace.task_create_fails) {
        return pdFAIL;
    } tasks[ntasks++] = (struct host_task) {
        fn, arg, 1
    };
    return pdPASS;
}
void vTaskDelete(TaskHandle_t t)
{
    if (in_task) {
        longjmp(task_exit, 1);
    }
}
int host_tasks_alive(void)
{
    int n = 0;
    for (int i = 0; i < ntasks; i++) {
        n += tasks[i].alive;
    } return n;
}
void host_run_tasks(void)
{
    for (int i = 0; i < ntasks; i++) if (tasks[i].alive) {
            in_task = 1;
            if (!setjmp(task_exit)) {
                tasks[i].fn(tasks[i].arg);
            } in_task = 0;
            tasks[i].alive = 0;
        }
}
esp_err_t nvs_flash_init(void)
{
    return 0;
} esp_err_t nvs_flash_erase(void)
{
    return 0;
}
esp_err_t nvs_open(const char *n, nvs_open_mode_t m, nvs_handle_t *h)
{
    return trace.nvs_open_err;
}
esp_err_t nvs_set_str(nvs_handle_t h, const char *k, const char *v)
{
    return trace.nvs_set_err;
}
esp_err_t nvs_get_str(nvs_handle_t h, const char *k, char *o, size_t *l)
{
    return ESP_ERR_NVS_NOT_FOUND;
}
esp_err_t nvs_commit(nvs_handle_t h)
{
    return trace.nvs_commit_err;
}
void nvs_close(nvs_handle_t h) {}
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
    return 0;
} esp_err_t esp_task_wdt_reset(void)
{
    return 0;
}
esp_err_t hal_ble_init(hal_ble_event_cb_t cb)
{
    trace.ble_cb = (void *)cb;
    return 0;
}
esp_err_t hal_ble_set_device_id(const char *id)
{
    trace.published_id_calls++;
    return trace.ble_set_id_err;
}
esp_err_t hal_ble_start_advertising(void)
{
    return 0;
} esp_err_t hal_ble_stop_advertising(void)
{
    return 0;
}
esp_err_t hal_ble_disconnect(void)
{
    return 0;
}
esp_err_t hal_ble_deinit(void)
{
    trace.ble_deinit_with_tasks = host_tasks_alive();
    trace.ble_deinit++;
    return 0;
}
bool hal_ble_session_is_current(hal_ble_session_t s)
{
    return s != HAL_BLE_SESSION_NONE;
}
esp_err_t hal_ble_send_wifi_scan_results(hal_ble_session_t s, const hal_ble_wifi_scan_result_t *r, size_t n)
{
    trace.scan_sends++;
    trace.ble_used_after_deinit |= trace.ble_deinit > 0;
    return 0;
}
esp_err_t hal_ble_send_wifi_config_result(hal_ble_session_t s, bool ok, const char *ip, const char *m)
{
    trace.wifi_results++;
    trace.last_wifi_ok = ok;
    return 0;
}
esp_err_t hal_ble_send_url_set_result(hal_ble_session_t s, bool ok, int32_t code, const char *m)
{
    trace.url_results++;
    trace.last_url_ok = ok;
    trace.last_url_code = code;
    snprintf(trace.last_url_msg, sizeof trace.last_url_msg, "%s", m);
    return 0;
}
esp_err_t hal_button_init(hal_button_event_cb_t cb)
{
    trace.button_cb = (void *)cb;
    return 0;
}
esp_err_t hal_diagnostics_init(void)
{
    return 0;
}
void hal_diagnostics_record_transition(uint8_t a, uint8_t b, uint8_t c) {}
void hal_diagnostics_record_queue_overflow(void) {}
void hal_diagnostics_record_url_set_attempt(const hal_diagnostics_url_set_t *a)
{
    trace.diag_result_code = a->result_code;
    trace.diag_nfc_err = a->nfc_err;
}
esp_err_t hal_indicator_init(void)
{
    return 0;
}
esp_err_t hal_led_set_pattern(hal_led_pattern_t p)
{
    trace.led = p;
    return 0;
}
esp_err_t hal_buzzer_play(hal_buzzer_sound_t s)
{
    trace.sound = s;
    return 0;
}
esp_err_t hal_network_init(void)
{
    return 0;
}
esp_err_t hal_network_deinit(void)
{
    trace.net_deinit_with_tasks = host_tasks_alive();
    trace.net_deinit++;
    return 0;
}
esp_err_t hal_wifi_connect(const hal_wifi_credentials_t *c)
{
    trace.connects++;
    memcpy(&trace.last_creds, c, sizeof * c);
    if (trace.on_connect) {
        void (*f)(void) = trace.on_connect;
        trace.on_connect = NULL;
        f();
    } return trace.connect_err;
}
hal_wifi_state_t hal_wifi_get_state(void)
{
    return trace.wifi_state;
}
void hal_wifi_register_callback(hal_wifi_event_cb_t cb) {}
esp_err_t hal_wifi_scan(hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX], uint16_t *count)
{
    trace.wifi_used_after_deinit |= trace.net_deinit > 0;
    *count = 0;
    return 0;
}
esp_err_t hal_nfc_init(void)
{
    return 0;
}
bool hal_nfc_device_id_valid(const char *id, size_t len)
{
    return len > 0 && len <= HAL_NFC_DEVICE_ID_MAX_LEN;
}
hal_nfc_err_t hal_nfc_ntag424_configure_sdm(const char *id, uint32_t t)
{
    return trace.nfc_result;
}
esp_err_t hal_power_init(void)
{
    return 0;
}
void hal_power_register_low_battery_cb(hal_low_battery_cb_t cb)
{
    trace.battery_cb = (void *)cb;
}
jmp_buf host_sleep_jmp;
void hal_power_deep_sleep(void)
{
    trace.slept++;
    longjmp(host_sleep_jmp, 1);
}
esp_err_t hal_time_start_sync(void)
{
    return 0;
} void hal_time_stop_sync(void) {}
