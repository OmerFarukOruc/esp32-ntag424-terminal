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

static void write_wifi_config(hal_ble_session_t session, const char *ssid, const char *password, esp_err_t *ack);

static void reset(app_state_t state)
{
    memset(&trace, 0, sizeof trace);
    host_nvs_reset();
    vQueueDelete(s_event_queue);
    s_event_queue = xQueueCreate(EVENT_QUEUE_SIZE, sizeof(event_msg_t));
    s_state_task.state = state;
    s_state_task.deep_sleep_postponed = false;
    s_state_task.radio_stop_failures = 0;
    s_state_task.wifi_attempt = 0;
    s_state_task.wifi_result_client = HAL_BLE_SESSION_NONE;
    host_run_tasks();
    drain();
    memset(&trace, 0, sizeof trace);
    trace.ble_current = SESSION_A;
    trace.wifi_cb = wifi_event_callback;
}

static void start_wifi_attempt(void)
{
    esp_err_t ack;
    write_wifi_config(SESSION_A, "HomeNet", "SECRETPASSWORD1", &ack);
    drain();
}

static int fill_queue(void)
{
    int posted = 0;
    while (app_post_event(APP_EVENT_WIFI_CONNECTING, NULL, 0) == ESP_OK) {
        posted++;
    }
    return posted;
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
    int writing = s_state_task.state == APP_STATE_WRITING_TAG;
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
    snprintf(d, sizeof d, "state=%s replies=%d code=%d msg=%s", s_state_names[s_state_task.state], trace.url_results,
             (int)trace.last_url_code, trace.last_url_msg);
    check("URL_SET after onboarding ended is refused with not_onboarding",
          s_state_task.state == APP_STATE_IDLE && url_reply_is(false, URL_SET_FAILED_CODE, "not_onboarding"), d);

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

    reset(APP_STATE_ONBOARDING_BLE);
    start_wifi_attempt();
    app_post_event(APP_EVENT_WIFI_CONNECTED, NULL, 0);
    poll_state_task();
    snprintf(d, sizeof d, "replies=%d, state=%s", trace.wifi_results, s_state_names[s_state_task.state]);
    check("positive control: a WIFI_CONNECTED event while the attempt is pending does not end it",
          trace.wifi_results == 0 && s_state_task.state == APP_STATE_CONNECTING_WIFI, d);

    static const struct {
        bool ok;
        const char *name;
    } results[] = {
        {true, "positive control: a successful attempt is answered once, to its own client, and the device returns to idle"},
        {false, "positive control: a failed attempt is answered once, to its own client, and the device returns to idle"},
    };
    for (size_t i = 0; i < sizeof results / sizeof results[0]; i++) {
        reset(APP_STATE_ONBOARDING_BLE);
        start_wifi_attempt();
        host_wifi_finish(results[i].ok);
        poll_state_task();
        poll_state_task();
        snprintf(d, sizeof d, "replies=%d ok=%d session=%#x, state=%s", trace.wifi_results, trace.last_wifi_ok,
                 (unsigned)trace.last_wifi_session, s_state_names[s_state_task.state]);
        check(results[i].name, trace.wifi_results == 1 && trace.last_wifi_ok == results[i].ok &&
              trace.last_wifi_session == SESSION_A && s_state_task.state == APP_STATE_IDLE, d);
    }

    for (size_t i = 0; i < sizeof results / sizeof results[0]; i++) {
        reset(APP_STATE_ONBOARDING_BLE);
        start_wifi_attempt();
        int filled = fill_queue();
        host_wifi_finish(results[i].ok);
        drain();
        int replies_before_poll = trace.wifi_results;
        poll_state_task();
        poll_state_task();
        snprintf(d, sizeof d, "queue filled with %d events, replies before the poll=%d, after=%d ok=%d, state=%s", filled,
                 replies_before_poll, trace.wifi_results, trace.last_wifi_ok, s_state_names[s_state_task.state]);
        check(results[i].ok ? "a successful outcome whose wake-up the full queue dropped is answered once by polling"
              : "a failed outcome whose wake-up the full queue dropped is answered once by polling",
              filled == EVENT_QUEUE_SIZE && trace.wifi_results == 1 && trace.last_wifi_ok == results[i].ok &&
              s_state_task.state == APP_STATE_IDLE, d);
    }

    reset(APP_STATE_ONBOARDING_BLE);
    start_wifi_attempt();
    poll_state_task();
    TickType_t waiting = host_queue_last_wait;
    host_wifi_finish(true);
    poll_state_task();
    poll_state_task();
    TickType_t idle = host_queue_last_wait;
    snprintf(d, sizeof d, "wait while the request is pending=%u ms, after its reply=%u ms", (unsigned)waiting,
             (unsigned)idle);
    check("the state task waits 100 ms for events while a WiFi request is pending, and 5000 ms otherwise",
          waiting == pdMS_TO_TICKS(100) && idle == pdMS_TO_TICKS(5000), d);

    reset(APP_STATE_ONBOARDING_BLE);
    start_wifi_attempt();
    hal_wifi_attempt_id_t cancelled = trace.attempt;
    app_post_event(APP_EVENT_BUTTON_SHORT, NULL, 0);
    drain();
    hal_wifi_attempt_id_t told_to_cancel = trace.last_cancelled;
    host_wifi_finish(true);
    poll_state_task();
    int replies_after_cancel = trace.wifi_results;
    app_post_event(APP_EVENT_BUTTON_HOLD_3S, NULL, 0);
    drain();
    start_wifi_attempt();
    hal_wifi_attempt_id_t fresh = trace.attempt;
    host_wifi_finish(false);
    poll_state_task();
    snprintf(d, sizeof d, "cancelled attempt %u, replies after the cancel=%d, new attempt %u, replies=%d ok=%d",
             (unsigned)cancelled, replies_after_cancel, (unsigned)fresh,
             trace.wifi_results, trace.last_wifi_ok);
    check("positive control: after a short press, a new request gets its own attempt and its own reply, and the old one none",
          cancelled != 0 && replies_after_cancel == 0 && fresh != cancelled && trace.wifi_results == 1 &&
          !trace.last_wifi_ok && s_state_task.state == APP_STATE_IDLE, d);
    snprintf(d, sizeof d, "HAL told to cancel %u, cancelled %u", (unsigned)told_to_cancel, (unsigned)cancelled);
    check("the HAL is told which attempt the short press detached", told_to_cancel == cancelled, d);

    reset(APP_STATE_ONBOARDING_BLE);
    trace.connect_err = ESP_FAIL;
    start_wifi_attempt();
    poll_state_task();
    snprintf(d, sizeof d, "replies=%d ok=%d, state=%s, wait=%u ms", trace.wifi_results, trace.last_wifi_ok,
             s_state_names[s_state_task.state], (unsigned)host_queue_last_wait);
    check("positive control: a connect that fails at once gets one failure reply and no pending attempt",
          trace.wifi_results == 1 && !trace.last_wifi_ok && s_state_task.state == APP_STATE_ONBOARDING_BLE &&
          host_queue_last_wait == pdMS_TO_TICKS(5000), d);

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

    static const struct {
        const char *name;
        hal_button_event_t event;
        uint32_t hold_ms;
        app_event_t posted;
    } refused_cases[] = {
        {"short release", HAL_BUTTON_EVENT_RELEASE, 400, APP_EVENT_BUTTON_SHORT},
        {"3 s hold", HAL_BUTTON_EVENT_HOLD, 3000, APP_EVENT_BUTTON_HOLD_3S},
        {"10 s hold", HAL_BUTTON_EVENT_HOLD, 10000, APP_EVENT_BUTTON_HOLD_10S},
    };
    for (size_t i = 0; i < sizeof refused_cases / sizeof refused_cases[0]; i++) {
        reset(APP_STATE_IDLE);
        atomic_store(&s_overflow_alarm, false);
        atomic_store(&s_consecutive_overflows, 1);
        event_msg_t filler = {.event = APP_EVENT_BUTTON_SHORT};
        while (xQueueSend(s_event_queue, &filler, 0) == pdTRUE) {
        }
        int refused = 0;
        for (int k = 0; k < QUEUE_OVERFLOW_THRESHOLD + 2; k++) {
            refused += button_callback(refused_cases[i].event, refused_cases[i].hold_ms) == ESP_ERR_TIMEOUT;
        }
        bool alarm = atomic_load(&s_overflow_alarm);
        unsigned streak = atomic_load(&s_consecutive_overflows);
        while (xQueueReceive(s_event_queue, &filler, 0) == pdTRUE) {
        }
        esp_err_t accepted = button_callback(refused_cases[i].event, refused_cases[i].hold_ms);
        event_msg_t queued = {.event = APP_EVENT_COUNT};
        xQueueReceive(s_event_queue, &queued, 0);
        snprintf(d, sizeof d,
                 "refused while the queue was full=%d of %d, overflow alarm=%d, streak=%u, drops recorded=%d, "
                 "accepted after=%s, queued event=%s",
                 refused, QUEUE_OVERFLOW_THRESHOLD + 2, alarm, streak, trace.diag_queue_overflows,
                 esp_err_to_name(accepted), queued.event < APP_EVENT_COUNT ? s_event_names[queued.event] : "none");
        char name[120];
        snprintf(name, sizeof name,
                 "a %s refused by a full event queue is reported to the button HAL, counts as no drop, and is "
                 "queued when offered again", refused_cases[i].name);
        check(name, refused == QUEUE_OVERFLOW_THRESHOLD + 2 && !alarm && streak == 1 &&
              trace.diag_queue_overflows == 0 && accepted == ESP_OK && queued.event == refused_cases[i].posted, d);
    }
    static const struct {
        bool finished_ok;
        bool finish_first;
        const char *name;
    } alarm_cases[] = {
        {false, false, "an overflow alarm that ends a pending WiFi request answers it once with a failure"},
        {true, true, "an overflow alarm that finds the WiFi request already succeeded answers it once with success"},
    };
    for (size_t i = 0; i < sizeof alarm_cases / sizeof alarm_cases[0]; i++) {
        reset(APP_STATE_ONBOARDING_BLE);
        start_wifi_attempt();
        if (alarm_cases[i].finish_first) {
            host_wifi_finish(true);
        }
        app_post_event(APP_EVENT_WIFI_CONNECTING, NULL, 0);
        atomic_store(&s_overflow_alarm, true);
        poll_state_task();
        poll_state_task();
        snprintf(d, sizeof d, "replies=%d ok=%d session=%#x, state=%s, HAL told to cancel %u", trace.wifi_results,
                 trace.last_wifi_ok, (unsigned)trace.last_wifi_session, s_state_names[s_state_task.state],
                 (unsigned)trace.last_cancelled);
        check(alarm_cases[i].name, trace.wifi_results == 1 && trace.last_wifi_ok == alarm_cases[i].finished_ok &&
              trace.last_wifi_session == SESSION_A && s_state_task.state == APP_STATE_ERROR &&
              trace.last_cancelled != 0, d);
        atomic_store(&s_overflow_alarm, false);
    }

    reset(APP_STATE_ONBOARDING_BLE);
    post_event(APP_EVENT_WIFI_SCAN_REQUESTED, SESSION_A, NULL, 0, 0);
    drain();
    start_wifi_attempt();
    int replies_during_scan = trace.wifi_results;
    bool refused = !trace.last_wifi_ok && trace.last_wifi_session == SESSION_A;
    int connects_during_scan = trace.connects;
    app_state_t state_during_scan = s_state_task.state;
    host_run_tasks();
    drain();
    snprintf(d, sizeof d, "replies=%d refused=%d connects=%d, state during the scan=%s, scan replies=%d",
             replies_during_scan, refused, connects_during_scan, s_state_names[state_during_scan], trace.scan_sends);
    check("a WiFi request that arrives while a scan runs is refused with a failure reply and starts no connect",
          replies_during_scan == 1 && refused && connects_during_scan == 0 &&
          state_during_scan == APP_STATE_ONBOARDING_BLE && trace.scan_sends >= 1, d);

    vQueueDelete(s_event_queue);
    return check_summary();
}
