/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include APP_STATE_SRC
#include "app_platform.h"
#include "check.h"

#define SESSION_A 0x10001
#define SESSION_B 0x20002

static void drain(void)
{
    event_msg_t m;
    while (xQueueReceive(s_event_queue, &m, 0) == pdTRUE) {
        if (!setjmp(host_sleep_jmp)) {
            handle_event(&m);
        }
    }
}

static void reset(app_state_t state)
{
    memset(&trace, 0, sizeof trace);
    vQueueDelete(s_event_queue);
    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_msg_t));
    s_state = state;
    s_critical_battery_pending = false;
    host_run_tasks();
    drain();
    memset(&trace, 0, sizeof trace);
}

static void write_wifi_config(hal_ble_session_t session, const char *ssid, const char *password, esp_err_t *ack)
{
    hal_ble_wifi_config_t cfg = {0};
    strcpy(cfg.ssid, ssid);
    strcpy(cfg.password, password);
    *ack = ble_event_callback(HAL_BLE_EVENT_WIFI_CONFIG, session, &cfg, sizeof cfg);
    explicit_bzero(&cfg, sizeof cfg);
}

static esp_err_t second_ack = ESP_FAIL;
static void second_wifi_config(void)
{
    write_wifi_config(SESSION_A, "SecondNet", "SECONDPASSWORD1", &second_ack);
}

static void url_set(hal_ble_session_t session)
{
    post_event(APP_EVENT_URL_SET, session, "SHOP", 4, 0);
}

int main(void)
{
    char d[240];
    esp_err_t ack;

    reset(APP_STATE_ONBOARDING_BLE);
    app_post_event(APP_EVENT_WIFI_SCAN_REQUESTED, NULL, 0);
    drain();
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    int deinit_during_scan = trace.net_deinit_with_tasks || trace.ble_deinit_with_tasks;
    host_run_tasks();
    drain();
    snprintf(d, sizeof d, "radios stopped while the scan task ran=%d, scan used WiFi/BLE after stop=%d/%d, slept=%d",
             deinit_during_scan, trace.wifi_used_after_deinit, trace.ble_used_after_deinit, trace.slept);
    check("critical battery during a scan sleeps after the scan ends",
          !deinit_during_scan && !trace.wifi_used_after_deinit && !trace.ble_used_after_deinit && trace.slept == 1, d);

    reset(APP_STATE_ONBOARDING_BLE);
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    snprintf(d, sizeof d, "slept=%d", trace.slept);
    check("critical battery with no work running sleeps at once", trace.slept == 1, d);

    reset(APP_STATE_ONBOARDING_BLE);
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_A, NULL, 0, 0);
    drain();
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    trace.nfc_result = HAL_NFC_OK;
    url_set(SESSION_B);
    drain();
    int writing = s_state == APP_STATE_WRITING_TAG;
    snprintf(d, sizeof d, "tag write started=%d, replies=%d, code=%d, msg=%s", writing, trace.url_results,
             (int)trace.last_url_code, trace.last_url_msg);
    check("URL_SET while a critical battery sleep waits is refused with low_battery",
          !writing && trace.url_results == 1 && trace.last_url_code == 1099 && strcmp(trace.last_url_msg, "low_battery") == 0, d);
    host_run_tasks();
    drain();

    struct {
        const char *name;
        int nvs_set_err, ble_set_id_err;
        const char *msg;
    } stored[] = {
        { "NVS save failure after a good tag write reports 1099 tag_configured_id_not_saved", 0x1105, 0, "tag_configured_id_not_saved" },
        { "DEVICE_INFO update failure after a good tag write reports 1099 tag_configured_id_not_published", 0, 0x103, "tag_configured_id_not_published" },
        { "a good tag write whose id is stored reports success", 0, 0, "sdm_configured" },
    };
    for (size_t i = 0; i < sizeof stored / sizeof stored[0]; i++) {
        reset(APP_STATE_ONBOARDING_BLE);
        trace.nfc_result = HAL_NFC_OK;
        trace.nvs_set_err = stored[i].nvs_set_err;
        trace.ble_set_id_err = stored[i].ble_set_id_err;
        url_set(SESSION_A);
        drain();
        host_run_tasks();
        drain();
        int expect_code = strcmp(stored[i].msg, "sdm_configured") == 0 ? 0 : 1099;
        snprintf(d, sizeof d, "replies=%d ok=%d code=%d msg=%s, diagnostics code=%d", trace.url_results, trace.last_url_ok,
                 (int)trace.last_url_code, trace.last_url_msg, (int)trace.diag_result_code);
        check(stored[i].name, trace.url_results == 1 && trace.last_url_ok == (expect_code == 0) &&
              trace.last_url_code == expect_code && trace.diag_result_code == expect_code &&
              strcmp(trace.last_url_msg, stored[i].msg) == 0, d);
    }

    reset(APP_STATE_ONBOARDING_BLE);
    write_wifi_config(SESSION_A, "HomeNet", "SECRETPASSWORD1", &ack);
    drain();
    size_t qbytes;
    unsigned char *qs = host_queue_storage(s_event_queue, &qbytes);
    int in_queue = memmem(qs, qbytes, "SECRETPASSWORD1", 15) != NULL;
    snprintf(d, sizeof d, "ack=%d, password bytes in queue storage=%d, connects=%d, ssid=%s", ack, in_queue, trace.connects,
             trace.last_creds.ssid);
    check("a WiFi password reaches hal_wifi_connect and leaves no copy in the event queue",
          ack == ESP_OK && !in_queue && trace.connects == 1 && strcmp(trace.last_creds.password, "SECRETPASSWORD1") == 0, d);

    reset(APP_STATE_WRITING_TAG);
    write_wifi_config(SESSION_A, "HomeNet", "SECRETPASSWORD1", &ack);
    drain();
    int left = memmem(&s_wifi_slot, sizeof s_wifi_slot, "SECRETPASSWORD1", 15) != NULL;
    snprintf(d, sizeof d, "password in the WiFi slot=%d, connects=%d, failure replies=%d", left, trace.connects,
             trace.wifi_results);
    check("a WiFi config refused during a tag write leaves no credential copy and gets a failure reply",
          !left && trace.connects == 0 && trace.wifi_results == 1 && !trace.last_wifi_ok, d);

    reset(APP_STATE_ONBOARDING_BLE);
    trace.on_connect = second_wifi_config;
    trace.connect_err = ESP_FAIL;
    write_wifi_config(SESSION_A, "FirstNet", "FIRSTPASSWORD1", &ack);
    drain();
    snprintf(d, sizeof d, "acks=%d/%d, connects=%d, last ssid=%s, replies=%d", ack, second_ack, trace.connects,
             trace.last_creds.ssid, trace.wifi_results);
    check("a WIFI_CONFIG written while the previous connect starts keeps its credentials and gets a reply",
          ack == ESP_OK && second_ack == ESP_OK && trace.connects == 2 && strcmp(trace.last_creds.ssid, "SecondNet") == 0 &&
          trace.wifi_results == 2, d);

    reset(APP_STATE_CONNECTING_WIFI);
    trace.wifi_state = HAL_WIFI_STATE_CONNECTING;
    app_post_event(APP_EVENT_WIFI_CONNECTED, NULL, 0);
    drain();
    snprintf(d, sizeof d, "replies=%d, state=%s", trace.wifi_results, s_state_names[s_state]);
    check("a WIFI_CONNECTED event while the driver still connects does not end the attempt",
          trace.wifi_results == 0 && s_state == APP_STATE_CONNECTING_WIFI, d);

    reset(APP_STATE_CONNECTING_WIFI);
    trace.wifi_state = HAL_WIFI_STATE_CONNECTED;
    app_post_event(APP_EVENT_WIFI_CONNECTED, NULL, 0);
    drain();
    snprintf(d, sizeof d, "replies=%d ok=%d, state=%s", trace.wifi_results, trace.last_wifi_ok, s_state_names[s_state]);
    check("a WIFI_CONNECTED event while the driver is connected reports success",
          trace.wifi_results == 1 && trace.last_wifi_ok && s_state == APP_STATE_IDLE, d);

    reset(APP_STATE_ONBOARDING_BLE);
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_A, NULL, 0, 0);
    drain();
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_A, NULL, 0, 0);
    drain();
    int same = trace.scan_sends;
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_B, NULL, 0, 0);
    drain();
    int other = trace.scan_sends - same;
    host_run_tasks();
    drain();
    snprintf(d, sizeof d, "replies while the scan ran: same session=%d, other session=%d; after the scan=%d", same, other,
             trace.scan_sends - same - other);
    check("while a scan runs, a request from another session gets the empty terminator and a repeat waits",
          same == 0 && other == 1 && trace.scan_sends - same - other == 1, d);
    vQueueDelete(s_event_queue);
    return check_summary();
}
