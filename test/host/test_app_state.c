/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include APP_STATE_SRC
#include "app_platform.h"
#include "check.h"
#include "host.h"

#define URL_SET_FAILED_CODE 1099

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
    host_nvs_reset();
    vQueueDelete(s_event_queue);
    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_msg_t));
    s_state = state;
    s_critical_battery_pending = false;
    s_radio_stop_failures = 0;
    host_run_tasks();
    drain();
    memset(&trace, 0, sizeof trace);
    trace.ble_current = SESSION_A;
}

static void poll_state_task(void)
{
    if (!setjmp(host_sleep_jmp)) {
        state_task_poll();
    }
}

static bool url_reply_is(bool ok, int32_t code, const char *msg)
{
    return trace.url_results == 1 && trace.last_url_ok == ok && trace.last_url_code == code &&
           strcmp(trace.last_url_msg, msg) == 0;
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
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_A, NULL, 0, 0);
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
    trace.ble_current = SESSION_B;
    url_set(SESSION_B);
    drain();
    int writing = s_state == APP_STATE_WRITING_TAG;
    snprintf(d, sizeof d, "tag write started=%d, replies=%d, code=%d, msg=%s", writing, trace.url_results,
             (int)trace.last_url_code, trace.last_url_msg);
    check("URL_SET while a critical battery sleep waits is refused with low_battery",
          !writing && url_reply_is(false, URL_SET_FAILED_CODE, "low_battery"), d);
    host_run_tasks();
    drain();

    struct {
        const char *name;
        esp_err_t nvs_set_err;
        esp_err_t nvs_commit_err;
        esp_err_t ble_set_id_err;
        const char *msg;
    } stored[] = {
        { "NVS save failure after a good tag write reports 1099 tag_configured_id_not_saved", 0x1105, ESP_OK, ESP_OK, "tag_configured_id_not_saved" },
        { "NVS commit failure after a good tag write reports 1099 tag_configured_id_not_saved", ESP_OK, 0x1105, ESP_OK, "tag_configured_id_not_saved" },
        { "DEVICE_INFO update failure after a good tag write reports 1099 tag_configured_id_not_published", ESP_OK, ESP_OK, 0x103, "tag_configured_id_not_published" },
    };
    for (size_t i = 0; i < sizeof stored / sizeof stored[0]; i++) {
        reset(APP_STATE_ONBOARDING_BLE);
        trace.nfc_result = HAL_NFC_OK;
        trace.nvs_set_err = stored[i].nvs_set_err;
        trace.nvs_commit_err = stored[i].nvs_commit_err;
        trace.ble_set_id_err = stored[i].ble_set_id_err;
        url_set(SESSION_A);
        drain();
        host_run_tasks();
        drain();
        snprintf(d, sizeof d, "replies=%d ok=%d code=%d msg=%s, diagnostics code=%d", trace.url_results, trace.last_url_ok,
                 (int)trace.last_url_code, trace.last_url_msg, (int)trace.diag_result_code);
        check(stored[i].name, url_reply_is(false, URL_SET_FAILED_CODE, stored[i].msg) &&
              trace.diag_result_code == URL_SET_FAILED_CODE, d);
    }

    reset(APP_STATE_ONBOARDING_BLE);
    trace.nfc_result = HAL_NFC_OK;
    url_set(SESSION_A);
    drain();
    host_run_tasks();
    drain();
    const char *stored_id = host_nvs_committed("dev_id");
    char published_now[HAL_BLE_DEVICE_ID_MAX];
    snprintf(published_now, sizeof published_now, "%s", trace.published_id);
    trace.published_id[0] = '\0';
    esp_err_t published = app_device_id_publish();
    snprintf(d, sizeof d, "replies=%d ok=%d code=%d msg=%s, stored id=%s, DEVICE_INFO id=%s, after a restart=%s (%s)",
             trace.url_results, trace.last_url_ok, (int)trace.last_url_code, trace.last_url_msg,
             stored_id ? stored_id : "-", published_now, trace.published_id, esp_err_to_name(published));
    check("a good tag write stores and publishes its id, and the stored id is published after a restart",
          url_reply_is(true, 0, "sdm_configured") && trace.diag_result_code == 0 && stored_id != NULL &&
          strcmp(stored_id, "SHOP") == 0 && strcmp(published_now, "SHOP") == 0 && published == ESP_OK &&
          strcmp(trace.published_id, "SHOP") == 0, d);

    reset(APP_STATE_ONBOARDING_BLE);
    trace.nfc_result = HAL_NFC_OK;
    url_set(SESSION_A);
    drain();
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    int slept_during_write = trace.slept;
    host_run_tasks();
    drain();
    snprintf(d, sizeof d, "slept during the write=%d, replies=%d ok=%d, slept after=%d", slept_during_write,
             trace.url_results, trace.last_url_ok, trace.slept);
    check("critical battery during a tag write sleeps after the write reports",
          slept_during_write == 0 && trace.url_results == 1 && trace.last_url_ok && trace.slept == 1, d);

    reset(APP_STATE_IDLE);
    url_set(SESSION_A);
    drain();
    snprintf(d, sizeof d, "state=%s replies=%d code=%d msg=%s", s_state_names[s_state], trace.url_results,
             (int)trace.last_url_code, trace.last_url_msg);
    check("URL_SET after onboarding ended is refused with not_onboarding",
          s_state == APP_STATE_IDLE && url_reply_is(false, URL_SET_FAILED_CODE, "not_onboarding"), d);

    reset(APP_STATE_ONBOARDING_BLE);
    esp_err_t first_ack;
    write_wifi_config(SESSION_A, "FirstNet", "FIRSTPASSWORD1", &first_ack);
    write_wifi_config(SESSION_A, "SecondNet", "SECONDPASSWORD1", &ack);
    drain();
    snprintf(d, sizeof d, "acks=%#x/%#x, connects=%d, ssid=%s", (unsigned)first_ack, (unsigned)ack,
             trace.connects, trace.last_creds.ssid);
    check("a WIFI_CONFIG written while the previous one still waits is refused and leaves the first intact",
          first_ack == ESP_OK && ack == ESP_ERR_INVALID_STATE && trace.connects == 1 &&
          strcmp(trace.last_creds.ssid, "FirstNet") == 0, d);

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
    trace.ble_current = SESSION_B;
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_B, NULL, 0, 0);
    drain();
    int while_running = trace.scan_sends;
    host_run_tasks();
    drain();
    const struct host_scan_send *to_b = &trace.scan[0];
    const struct host_scan_send *to_a = &trace.scan[1];
    snprintf(d, sizeof d, "replies while the scan ran: after the repeat=%d, after B=%d (session %#x, %zu networks); "
             "after the scan=%d (session %#x, %zu networks, %s %d dBm ch %u %s)", same, while_running,
             (unsigned)to_b->session, to_b->count, trace.scan_sends, (unsigned)to_a->session, to_a->count,
             to_a->first.ssid, to_a->first.rssi, to_a->first.channel, to_a->first.security);
    check("while a scan runs, another session gets the empty terminator, a repeat waits, and the scan answers its own session",
          same == 0 && while_running == 1 && to_b->session == SESSION_B && to_b->count == 0 && trace.scan_sends == 2 &&
          to_a->session == SESSION_A && to_a->count == 1 && strcmp(to_a->first.ssid, host_scan_ap.ssid) == 0 &&
          to_a->first.rssi == host_scan_ap.rssi && to_a->first.channel == host_scan_ap.channel &&
          strcmp(to_a->first.security, "WPA3") == 0, d);
    static const struct {
        const char *name;
        app_event_t event;
    } stale[] = {
        { "a URL_SET queued by a client that left starts no tag write and gets no reply", APP_EVENT_URL_SET },
        { "a WIFI_CONFIG queued by a client that left connects nowhere, leaves no credentials and gets no reply", APP_EVENT_WIFI_CONFIG_RECEIVED },
        { "a WIFI_SCAN queued by a client that left starts no scan and gets no reply", APP_EVENT_WIFI_SCAN_REQUESTED },
    };
    for (size_t i = 0; i < sizeof stale / sizeof stale[0]; i++) {
        reset(APP_STATE_ONBOARDING_BLE);
        trace.nfc_result = HAL_NFC_OK;
        if (stale[i].event == APP_EVENT_URL_SET) {
            url_set(SESSION_A);
        } else if (stale[i].event == APP_EVENT_WIFI_CONFIG_RECEIVED) {
            write_wifi_config(SESSION_A, "HomeNet", "SECRETPASSWORD1", &ack);
        } else {
            post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_A, NULL, 0, 0);
        }
        trace.ble_current = SESSION_B;
        drain();
        host_run_tasks();
        drain();
        int left = memmem(&s_wifi_slot, sizeof s_wifi_slot, "SECRETPASSWORD1", 15) != NULL;
        snprintf(d, sizeof d, "tag writes=%d connects=%d scans=%d, replies url/wifi/scan=%d/%d/%d, password in the slot=%d",
                 trace.nfc_writes, trace.connects, trace.scans, trace.url_results, trace.wifi_results,
                 trace.scan_sends, left);
        check(stale[i].name, trace.nfc_writes == 0 && trace.connects == 0 && trace.scans == 0 && trace.url_results == 0 &&
              trace.wifi_results == 0 && trace.scan_sends == 0 && !left, d);
    }

    reset(APP_STATE_ONBOARDING_BLE);
    trace.ble_deinit_err = ESP_FAIL;
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    int slept_while_ble_up = trace.slept;
    int wifi_stops_while_ble_up = trace.net_deinit;
    trace.ble_deinit_err = ESP_OK;
    poll_state_task();
    snprintf(d, sizeof d, "slept while BLE did not stop=%d, WiFi stop attempts meanwhile=%d, BLE stop attempts=%d, "
             "slept after the next poll=%d", slept_while_ble_up, wifi_stops_while_ble_up, trace.ble_deinit,
             trace.slept);
    check("critical battery sleeps only after BLE stops, stops WiFi meanwhile, and the state task retries",
          slept_while_ble_up == 0 && wifi_stops_while_ble_up == 1 && trace.slept == 1, d);

    reset(APP_STATE_ONBOARDING_BLE);
    trace.ble_deinit_err = ESP_FAIL;
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    poll_state_task();
    int slept_after_two = trace.slept;
    poll_state_task();
    trace.ble_deinit_err = ESP_OK;
    snprintf(d, sizeof d, "slept after 2 failed stops=%d, after the 3rd=%d, BLE stop attempts=%d", slept_after_two,
             trace.slept, trace.ble_deinit);
    check("critical battery sleeps after the 3rd failed radio stop anyway",
          slept_after_two == 0 && trace.slept == 1 && trace.ble_deinit == 3, d);

    reset(APP_STATE_ONBOARDING_BLE);
    trace.net_deinit_err = ESP_FAIL;
    app_post_event(APP_EVENT_CRITICAL_BATTERY, NULL, 0);
    drain();
    int slept_while_wifi_up = trace.slept;
    trace.net_deinit_err = ESP_OK;
    poll_state_task();
    snprintf(d, sizeof d, "slept while WiFi did not stop=%d, WiFi stop attempts=%d, slept after the next poll=%d",
             slept_while_wifi_up, trace.net_deinit, trace.slept);
    check("critical battery sleeps only after WiFi stops too",
          slept_while_wifi_up == 0 && trace.net_deinit == 2 && trace.slept == 1, d);
    vQueueDelete(s_event_queue);
    return check_summary();
}
