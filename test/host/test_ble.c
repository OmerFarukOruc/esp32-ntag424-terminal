/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#define _GNU_SOURCE
#include HAL_BLE_SRC
#include <malloc.h>
#include <stdlib.h>
#include "check.h"
#include "host.h"

#define SECRET "SECRETPW1"
#define SECRET_SLICE 4
#define BLE_MTU_DEFAULT 247

static int cb_count;
static char cb_data[256];
static bool app_admits = true;
static hal_ble_session_t cb_session;

static esp_err_t app_cb(hal_ble_event_t e, hal_ble_session_t s, const void *d, size_t n)
{
    cb_count++;
    cb_session = s;
    if (d != NULL) {
        memcpy(cb_data, d, n);
        cb_data[n] = 0;
    }
    return app_admits ? ESP_OK : ESP_ERR_TIMEOUT;
}

void __real_free(void *p);
static int secret_frees;

static bool holds_secret_slice(const void *p, size_t n)
{
    for (size_t i = 0; i + SECRET_SLICE <= strlen(SECRET); i++) {
        if (memmem(p, n, SECRET + i, SECRET_SLICE) != NULL) {
            return true;
        }
    }
    return false;
}

void __wrap_free(void *p)
{
    if (p != NULL && holds_secret_slice(p, malloc_usable_size(p))) {
        secret_frees++;
    }
    __real_free(p);
}

static void post_gap(struct ble_gap_event ev)
{
    host_post_gap(gap_event_handler, ev);
}

static void post_subscribe(uint16_t attr, int on)
{
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_SUBSCRIBE };
    ev.subscribe.conn_handle = 1;
    ev.subscribe.attr_handle = attr;
    ev.subscribe.cur_notify = on;
    post_gap(ev);
}

static void subscribe(uint16_t attr, int on)
{
    post_subscribe(attr, on);
    host_run_host();
}

static void post_reconnect(void)
{
    post_gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_DISCONNECT, .disconnect = { .reason = 0x13 }
    });
    post_gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_CONNECT, .connect = { 0, 1 }
    });
    post_subscribe(s_wifi_scan.val_handle, 1);
    post_subscribe(s_wifi_config.val_handle, 1);
    post_subscribe(s_url_set.val_handle, 1);
}

static void connect_subscribed(void)
{
    post_reconnect();
    host_run_host();
    host_mtu = BLE_MTU_DEFAULT;
    app_admits = true;
    host_notify_reset();
}

static int notifications_to(int conn_id)
{
    int n = 0;
    for (int i = 0; i < host_notify_count && i < HOST_NOTIFY_MAX; i++) {
        n += host_notify_conn[i] == conn_id;
    }
    return n;
}

static int write_chr(ble_gatt_access_fn *fn, const char *bytes, size_t len)
{
    struct os_mbuf om = { .len = (uint16_t)len };
    memcpy(om.data, bytes, len);
    struct ble_gatt_access_ctxt c = { .op = 1, .om = &om };
    cb_count = 0;
    return fn(1, s_url_set.val_handle, &c, NULL);
}

#define WRITE(fn, s) write_chr(fn, s, sizeof(s) - 1)

static void check_rejected(const char *name, int att)
{
    char d[160];
    snprintf(d, sizeof d, "ATT=0x%02x events=%d", att, cb_count);
    check(name, att == BLE_ATT_ERR_VALUE_NOT_ALLOWED && cb_count == 0, d);
}

static bool json_number_is(cJSON *j, const char *key, double want)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsNumber(item) && item->valuedouble == want;
}

static bool json_string_is(cJSON *j, const char *key, const char *want)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(j, key);
    return cJSON_IsString(item) && strcmp(item->valuestring, want) == 0;
}

static bool is_terminator(int index, double count, double skipped)
{
    cJSON *j = cJSON_Parse(host_notify_payload[index]);
    bool ok = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "done")) && json_number_is(j, "count", count) &&
              json_number_is(j, "skipped", skipped);
    cJSON_Delete(j);
    return ok;
}

static bool is_entry(int index, const hal_ble_wifi_scan_result_t *r)
{
    cJSON *j = cJSON_Parse(host_notify_payload[index]);
    bool ok = json_string_is(j, "ssid", r->ssid) && json_number_is(j, "rssi", r->rssi) &&
              json_string_is(j, "sec", r->security) && json_number_is(j, "ch", r->channel);
    cJSON_Delete(j);
    return ok;
}

static bool reply_message_is(int index, const char *want)
{
    cJSON *j = cJSON_Parse(host_notify_payload[index]);
    bool ok = json_string_is(j, "message", want);
    cJSON_Delete(j);
    return ok;
}

static void check_mtu(const char *name, uint16_t mtu, int want_att)
{
    char d[160];
    connect_subscribed();
    host_mtu = mtu;
    int att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    snprintf(d, sizeof d, "MTU %u: ATT=0x%02x events=%d", mtu, att, cb_count);
    check(name, att == want_att && cb_count == (want_att == 0 ? 1 : 0), d);
}

static void slow_stack(void)
{
    host_now_us += 150000;
}

static hal_ble_session_t url_set_request(void)
{
    connect_subscribed();
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    return cb_session;
}

int main(void)
{
    char d[2 * HOST_NOTIFY_LEN + 200];
    int att;
    esp_err_t e;
    s_wifi_scan.val_handle = 3;
    s_wifi_config.val_handle = 5;
    s_url_set.val_handle = 7;
    hal_ble_init(app_cb);
    connect_subscribed();

    check_rejected("bytes after the JSON value are rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"} trailing"));
    check_rejected("an escaped NUL in a value is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\\u0000WRONG\"}"));
    check_rejected("a raw NUL after a complete JSON value is rejected",
                   WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"}\0{\"deviceId\":\"WRONG\"}"));
    check_rejected("a \\u escape whose 1st digit is not hex is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"AB\\uZ041CD\"}"));
    check_rejected("a \\u escape whose 2nd digit is not hex is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"AB\\u0Z41CD\"}"));
    check_rejected("a \\u escape whose 3rd digit is not hex is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"AB\\u00Z1CD\"}"));
    check_rejected("a \\u escape whose 4th digit is not hex is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"AB\\u004ZCD\"}"));
    check_rejected("a control byte after the JSON value is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"ABCD\"}\x01"));

    att = WRITE(wifi_config_chr_access, "{\"ssid\":\"a\\\\u0000b\",\"password\":\"12345678\"}");
    const char *ssid = ((hal_ble_wifi_config_t *)cb_data)->ssid;
    snprintf(d, sizeof d, "ATT=0x%02x events=%d ssid=%s", att, cb_count, cb_count ? ssid : "-");
    check("an escaped backslash before u0000 is accepted as text",
          att == 0 && cb_count == 1 && strcmp(ssid, "a\\u0000b") == 0, d);
    att = WRITE(url_set_chr_access, " {\"deviceId\":\"S\\u0041OP_01\"}\t\r\n");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d id=%s", att, cb_count, cb_count ? cb_data : "-");
    check("a valid \\u escape and JSON whitespace are accepted", att == 0 && cb_count == 1 && strcmp(cb_data, "SAOP_01") == 0, d);

    secret_frees = 0;
    att = WRITE(wifi_config_chr_access, "{\"ssid\":\"A\",\"password\":\"" SECRET "\"} x");
    snprintf(d, sizeof d, "ATT=0x%02x, freed heap blocks holding 4 bytes of the password=%d", att, secret_frees);
    check("a rejected WIFI_CONFIG frees no heap block that holds part of the password",
          att == BLE_ATT_ERR_VALUE_NOT_ALLOWED && secret_frees == 0, d);
    secret_frees = 0;
    att = WRITE(wifi_config_chr_access, "{\"ssid\":\"A\",\"password\":\"" SECRET "\"}");
    snprintf(d, sizeof d, "ATT=0x%02x, freed heap blocks holding 4 bytes of the password=%d", att, secret_frees);
    check("an accepted WIFI_CONFIG frees no heap block that holds part of the password", att == 0 && secret_frees == 0, d);

    connect_subscribed();
    app_admits = false;
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    snprintf(d, sizeof d, "ATT=0x%02x (application queue full)", att);
    check("a write the application cannot queue fails with ATT 0x11", att == BLE_ATT_ERR_INSUFFICIENT_RES, d);
    connect_subscribed();
    subscribe(s_url_set.val_handle, 0);
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d", att, cb_count);
    check("a write with notifications off fails with ATT 0xFD", att == 0xFD && cb_count == 0, d);
    check_mtu("a write on an MTU of 99 fails with ATT 0xFC", 99, 0xFC);
    check_mtu("a write on an MTU of 100 is accepted", 100, 0);

    connect_subscribed();
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    hal_ble_session_t session_a = cb_session;
    connect_subscribed();
    e = hal_ble_send_url_set_result(session_a, true, 0, "sdm_configured");
    snprintf(d, sizeof d, "send=%s notifications=%d", esp_err_to_name(e), host_notify_count);
    check("a reply for a client that left does not reach the next client", e == ESP_ERR_INVALID_STATE && host_notify_count == 0, d);
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_02\"}");
    e = hal_ble_send_url_set_result(cb_session, true, 0, "sdm_configured");
    snprintf(d, sizeof d, "send=%s notifications=%d payload=%s", esp_err_to_name(e), host_notify_count,
             host_notify_payload[0]);
    check("a reply reaches the client that sent the request",
          e == ESP_OK && host_notify_count == 1 && reply_message_is(0, "sdm_configured"), d);

    connect_subscribed();
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"}");
    hal_ble_session_t old = cb_session;
    host_notify_enomem_once = true;
    host_on_enomem = connect_subscribed;
    e = hal_ble_send_url_set_result(old, true, 0, "ok");
    snprintf(d, sizeof d, "send=%s, notifications to the new client=%d", esp_err_to_name(e), host_notify_count);
    check("a retry after ENOMEM does not reach a client that connected meanwhile",
          e == ESP_ERR_INVALID_STATE && host_notify_count == 0, d);

    connect_subscribed();
    WRITE(wifi_scan_chr_access, "{}");
    hal_ble_session_t scan_session = cb_session;
    hal_ble_wifi_scan_result_t r = { .rssi = -127, .channel = 11 };
    strcpy(r.security, "WPA3");
    memset(r.ssid, 'A', 32);
    r.ssid[32] = 0;
    size_t entry_len = strlen("{\"ssid\":\"\",\"rssi\":-127,\"sec\":\"WPA3\",\"ch\":11}") + 32;
    host_mtu = (uint16_t)(entry_len + 3);
    e = hal_ble_send_wifi_scan_results(scan_session, &r, 1);
    snprintf(d, sizeof d, "MTU %u, entry %zu bytes: send=%s notifications=%d first=%s last=%s", host_mtu, entry_len,
             esp_err_to_name(e), host_notify_count, host_notify_payload[0], host_notify_payload[1]);
    check("a scan entry of exactly MTU minus 3 bytes is sent",
          e == ESP_OK && host_notify_count == 2 && is_entry(0, &r) && is_terminator(1, 1, 0), d);

    host_notify_reset();
    host_mtu = (uint16_t)(entry_len + 2);
    e = hal_ble_send_wifi_scan_results(scan_session, &r, 1);
    snprintf(d, sizeof d, "MTU %u, entry %zu bytes: send=%s notifications=%d first=%s", host_mtu, entry_len,
             esp_err_to_name(e), host_notify_count, host_notify_payload[0]);
    check("a scan entry one byte over MTU minus 3 is skipped and counted",
          e == ESP_OK && host_notify_count == 1 && is_terminator(0, 0, 1), d);

    hal_ble_wifi_scan_result_t three[3] = { r, r, r };
    for (int k = 0; k < 3; k++) {
        snprintf(three[k].ssid, sizeof three[k].ssid, "net%d", k);
    }
    host_mtu = BLE_MTU_DEFAULT;
    host_notify_reset();
    host_notify_fail_at = 2;
    e = hal_ble_send_wifi_scan_results(scan_session, three, 3);
    snprintf(d, sizeof d, "3 networks, the 2nd send fails: notifications=%d first=%s last=%s", host_notify_count,
             host_notify_payload[0], host_notify_payload[1]);
    check("the terminator after a failed send counts every network not sent",
          host_notify_count == 2 && is_entry(0, &three[0]) && is_terminator(1, 1, 2), d);
    hal_ble_session_t session = url_set_request();
    int conn_a = host_conn_id;
    host_before_alloc = post_reconnect;
    e = hal_ble_send_url_set_result(session, true, 0, "for_a");
    host_run_host();
    snprintf(d, sizeof d, "send=%s, notifications to A=%d, to the client that took over the handle=%d", esp_err_to_name(e),
             notifications_to(conn_a), notifications_to(host_conn_id));
    check("a reply whose client is replaced while its buffer is allocated does not reach the new client",
          host_conn_id != conn_a && notifications_to(host_conn_id) == 0, d);

    session = url_set_request();
    host_stalled_waits = 1000;
    int64_t t0 = host_now_us;
    e = hal_ble_send_url_set_result(session, true, 0, "late");
    int64_t waited_us = host_now_us - t0;
    host_stalled_waits = 0;
    host_run_host();
    snprintf(d, sizeof d, "send=%s after %lld ms, notifications=%d, terminations=%d, session current=%d",
             esp_err_to_name(e), (long long)(waited_us / 1000), host_notify_count, host_terminations,
             hal_ble_session_is_current(session));
    check("a reply the host task does not take within 250 ms fails, is never sent, and ends that connection",
          e == ESP_ERR_TIMEOUT && waited_us <= 250000 && host_notify_count == 0 && host_terminations == 1 &&
          !hal_ble_session_is_current(session), d);

    session = url_set_request();
    host_put_delay_us = 300000;
    e = hal_ble_send_url_set_result(session, true, 0, "slow_put");
    host_put_delay_us = 0;
    snprintf(d, sizeof d, "send=%s, notifications=%d, session current=%d", esp_err_to_name(e), host_notify_count,
             hal_ble_session_is_current(session));
    check("a reply whose hand-over to the host queue takes 300 ms still gets its 250 ms and is sent",
          e == ESP_OK && host_notify_count == 1 && hal_ble_session_is_current(session), d);

    session = url_set_request();
    host_stalled_waits = 1000;
    esp_err_t waiting[4];
    for (int k = 0; k < 4; k++) {
        waiting[k] = hal_ble_send_url_set_result(session, true, 0, "stalled");
    }
    esp_err_t fifth = hal_ble_send_url_set_result(session, true, 0, "fifth");
    host_stalled_waits = 0;
    host_run_host();
    session = url_set_request();
    e = hal_ble_send_url_set_result(session, true, 0, "after");
    snprintf(d, sizeof d, "4 waiting: %s %s %s %s, 5th=%s, after the host task ran=%s with notifications=%d (%s)",
             esp_err_to_name(waiting[0]), esp_err_to_name(waiting[1]), esp_err_to_name(waiting[2]),
             esp_err_to_name(waiting[3]), esp_err_to_name(fifth), esp_err_to_name(e), host_notify_count,
             host_notify_payload[0]);
    check("with 4 replies waiting for the host task a 5th fails with no memory, and replies work after the host task runs",
          waiting[0] == ESP_ERR_TIMEOUT && waiting[1] == ESP_ERR_TIMEOUT && waiting[2] == ESP_ERR_TIMEOUT &&
          waiting[3] == ESP_ERR_TIMEOUT && fifth == ESP_ERR_NO_MEM && e == ESP_OK && host_notify_count == 1 &&
          reply_message_is(0, "after"), d);

    session = url_set_request();
    host_notify_enomem_always = true;
    e = hal_ble_send_url_set_result(session, true, 0, "no_buffers");
    host_run_host();
    snprintf(d, sizeof d, "send=%s, attempts=%d, terminations=%d, session current=%d", esp_err_to_name(e),
             host_notify_calls, host_terminations, hal_ble_session_is_current(session));
    check("a reply the stack keeps refusing for lack of buffers is tried 3 times and ends that connection",
          e == ESP_ERR_NO_MEM && host_notify_calls == 3 && host_terminations == 1 &&
          !hal_ble_session_is_current(session), d);

    session = url_set_request();
    host_notify_enomem_always = true;
    host_on_enomem_always = slow_stack;
    e = hal_ble_send_url_set_result(session, true, 0, "slow_no_buffers");
    host_run_host();
    snprintf(d, sizeof d, "send=%s, attempts=%d", esp_err_to_name(e), host_notify_calls);
    check("a reply whose attempts each take 150 ms stops retrying at the 250 ms limit",
          e == ESP_ERR_NO_MEM && host_notify_calls == 2, d);

    connect_subscribed();
    post_gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_DISCONNECT, .disconnect = { .reason = 0x13 }
    });
    post_gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_CONNECT, .connect = { 0, 1 }
    });
    host_run_host();
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"}");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d", att, cb_count);
    check("a client that connects after a subscribed client left must turn notifications on before it writes",
          att == ATT_ERR_CCCD_IMPROPER && cb_count == 0, d);

    connect_subscribed();
    post_reconnect();
    hal_ble_disconnect();
    host_run_host();
    int ended_stale = host_terminations;
    bool replacement_current = atomic_load(&s_session) != HAL_BLE_SESSION_NONE;
    hal_ble_disconnect();
    host_run_host();
    snprintf(d, sizeof d, "terminations after a request for the replaced client=%d (replacement connected=%d), "
             "after a request for the replacement=%d", ended_stale, replacement_current, host_terminations);
    check("hal_ble_disconnect ends the current client and never a client that replaced it before the host ran",
          ended_stale == 0 && replacement_current && host_terminations == 1, d);

    connect_subscribed();
    host_terminate_rc = 12;
    hal_ble_disconnect();
    host_run_host();
    int ended_at_once = host_terminations;
    int retries = host_fire_callouts();
    host_run_host();
    int ended_on_retry = host_terminations;
    host_terminate_rc = BLE_HS_EALREADY;
    connect_subscribed();
    hal_ble_disconnect();
    host_run_host();
    int retries_after_already = host_fire_callouts();
    host_terminate_rc = BLE_HS_ENOTCONN;
    connect_subscribed();
    hal_ble_disconnect();
    host_run_host();
    int retries_after_gone = host_fire_callouts();
    host_terminate_rc = 0;
    snprintf(d, sizeof d, "after a controller error: terminations=%d, retries armed=%d, terminations after the "
             "retry=%d; retries armed after EALREADY=%d, after ENOTCONN=%d", ended_at_once, retries, ended_on_retry,
             retries_after_already, retries_after_gone);
    check("a disconnect the controller refuses is tried again later, and one already under way or done is not",
          ended_at_once == 0 && retries == 1 && ended_on_retry == 1 && retries_after_already == 0 &&
          retries_after_gone == 0, d);

    connect_subscribed();
    hal_ble_session_t newer = atomic_load(&s_session);
    request_end(newer);
    request_end(newer - 1);
    host_run_host();
    snprintf(d, sizeof d, "terminations=%d, session %#x current=%d", host_terminations, (unsigned)newer,
             hal_ble_session_is_current(newer));
    check("of two end requests waiting for the host task the newer session wins",
          host_terminations == 1 && !hal_ble_session_is_current(newer), d);

    session = url_set_request();
    subscribe(s_url_set.val_handle, 0);
    e = hal_ble_send_url_set_result(session, true, 0, "unsubscribed");
    host_run_host();
    snprintf(d, sizeof d, "send=%s, notifications=%d, terminations=%d", esp_err_to_name(e), host_notify_count,
             host_terminations);
    check("a reply to a client that turned notifications off is not sent and ends that connection",
          e == ESP_ERR_INVALID_STATE && host_notify_count == 0 && host_terminations == 1, d);

    connect_subscribed();
    WRITE(wifi_scan_chr_access, "{}");
    session = cb_session;
    host_stalled_waits = 1;
    e = hal_ble_send_wifi_scan_results(session, three, 3);
    host_stalled_waits = 0;
    host_run_host();
    snprintf(d, sizeof d, "send=%s, notifications=%d, terminations=%d", esp_err_to_name(e), host_notify_count,
             host_terminations);
    check("a scan entry the host task does not take in time ends the connection without a terminator",
          e == ESP_ERR_TIMEOUT && host_notify_count == 0 && host_terminations == 1, d);

    hal_ble_session_t first = url_set_request();
    for (int k = 0; k < 65535; k++) {
        post_gap((struct ble_gap_event) {
            .type = BLE_GAP_EVENT_DISCONNECT, .disconnect = { .reason = 0x13 }
        });
        post_gap((struct ble_gap_event) {
            .type = BLE_GAP_EVENT_CONNECT, .connect = { 0, 1 }
        });
        host_run_host();
    }
    hal_ble_session_t later = url_set_request();
    snprintf(d, sizeof d, "session of the 1st connection=%#x, 65536 connections later=%#x", (unsigned)first,
             (unsigned)later);
    check("a session number does not come back after 65536 more connections on the same handle", later != first, d);

    s_host_last_session = HAL_BLE_SESSION_NONE - 1;
    host_notify_reset();
    post_reconnect();
    host_run_host();
    snprintf(d, sizeof d, "terminations=%d", host_terminations);
    check("a connection after the last session number of the boot is ended", host_terminations == 1, d);
    s_host_last_session = first;

    host_port_stop_rc = 1;
    esp_err_t stop_failed = hal_ble_deinit();
    esp_err_t adv_after_failure = hal_ble_start_advertising();
    host_run_host();
    connect_subscribed();
    session = url_set_request();
    esp_err_t reply_after_failure = hal_ble_send_url_set_result(session, true, 0, "still_up");
    host_port_stop_rc = 0;
    host_terminate_rc = 12;
    hal_ble_disconnect();
    host_run_host();
    esp_err_t stopped = hal_ble_deinit();
    esp_err_t adv_after_stop = hal_ble_start_advertising();
    esp_err_t reply_after_stop = hal_ble_send_url_set_result(session, true, 0, "stopped");
    snprintf(d, sizeof d, "deinit with a host task that does not stop=%s, advertising and a reply after it=%s %s; "
             "deinit after it stops=%s, advertising and a reply after it=%s %s, session current=%d, retry timers "
             "released while armed=%d", esp_err_to_name(stop_failed), esp_err_to_name(adv_after_failure),
             esp_err_to_name(reply_after_failure), esp_err_to_name(stopped), esp_err_to_name(adv_after_stop),
             esp_err_to_name(reply_after_stop), hal_ble_session_is_current(session), host_callouts_released_armed);
    check("hal_ble_deinit fails and keeps BLE working while the NimBLE host does not stop",
          stop_failed != ESP_OK && adv_after_failure == ESP_OK && reply_after_failure == ESP_OK &&
          stopped == ESP_OK && adv_after_stop == ESP_ERR_INVALID_STATE && reply_after_stop == ESP_ERR_INVALID_STATE, d);
    check("hal_ble_deinit forgets the client and stops a pending disconnect retry before it releases the timer",
          !hal_ble_session_is_current(session) && host_callouts_released_armed == 0, d);

    esp_err_t init_again = hal_ble_init(app_cb);
    connect_subscribed();
    hal_ble_start_advertising();
    session = url_set_request();
    host_stalled_waits = 1000;
    esp_err_t left_queued = hal_ble_send_url_set_result(session, true, 0, "left_queued");
    host_stalled_waits = 0;
    esp_err_t stopped_with_queued = hal_ble_deinit();
    esp_err_t init_third = hal_ble_init(app_cb);
    int adv_starts_before = host_adv_starts;
    post_gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_CONNECT, .connect = { BLE_HS_ENOTCONN, 1 }
    });
    post_gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_CONNECT, .connect = { 0, 1 }
    });
    host_run_host();
    int adv_starts_after_reinit = host_adv_starts - adv_starts_before;
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"}");
    connect_subscribed();
    session = url_set_request();
    host_stalled_waits = 1000;
    for (int k = 0; k < 4; k++) {
        waiting[k] = hal_ble_send_url_set_result(session, true, 0, "stalled");
    }
    host_stalled_waits = 0;
    host_run_host();
    session = url_set_request();
    e = hal_ble_send_url_set_result(session, true, 0, "after_reinit");
    snprintf(d, sizeof d, "init=%s, queued reply=%s, deinit=%s, init=%s, advertising started after it=%d, write "
             "without notifications on=ATT 0x%02x, 4 waiting=%s %s %s %s, reply=%s", esp_err_to_name(init_again),
             esp_err_to_name(left_queued), esp_err_to_name(stopped_with_queued), esp_err_to_name(init_third),
             adv_starts_after_reinit, att, esp_err_to_name(waiting[0]), esp_err_to_name(waiting[1]),
             esp_err_to_name(waiting[2]), esp_err_to_name(waiting[3]), esp_err_to_name(e));
    check("BLE starts again after a deinit with a reply still queued, with no advertising or subscription left over",
          init_again == ESP_OK && left_queued == ESP_ERR_TIMEOUT && stopped_with_queued == ESP_OK &&
          init_third == ESP_OK && adv_starts_after_reinit == 0 && att == ATT_ERR_CCCD_IMPROPER &&
          waiting[0] == ESP_ERR_TIMEOUT && waiting[1] == ESP_ERR_TIMEOUT && waiting[2] == ESP_ERR_TIMEOUT &&
          waiting[3] == ESP_ERR_TIMEOUT && e == ESP_OK, d);

    esp_err_t stopped_again = hal_ble_deinit();
    host_callout_init_rc = -1;
    esp_err_t no_timer = hal_ble_init(app_cb);
    host_callout_init_rc = 0;
    host_add_svcs_rc = BLE_HS_ENOMEM;
    esp_err_t no_services = hal_ble_init(app_cb);
    host_add_svcs_rc = 0;
    esp_err_t init_after_failures = hal_ble_init(app_cb);
    snprintf(d, sizeof d, "deinit=%s, init without a timer=%s, init whose services fail=%s, init after both=%s",
             esp_err_to_name(stopped_again), esp_err_to_name(no_timer), esp_err_to_name(no_services),
             esp_err_to_name(init_after_failures));
    check("an init that fails partway reports it and leaves the port ready for the next init",
          stopped_again == ESP_OK && no_timer == ESP_ERR_NO_MEM && no_services == ESP_ERR_NO_MEM &&
          init_after_failures == ESP_OK, d);

    connect_subscribed();
    session = url_set_request();
    host_port_deinit_rc = 1;
    esp_err_t port_failed = hal_ble_deinit();
    host_port_deinit_rc = 0;
    esp_err_t deinit_retry = hal_ble_deinit();
    esp_err_t init_after_port_failure = hal_ble_init(app_cb);
    esp_err_t reply_after_port_failure = hal_ble_send_url_set_result(session, true, 0, "port_failed");
    snprintf(d, sizeof d, "deinit with a port that does not deinit=%s, deinit again=%s, init=%s, reply=%s",
             esp_err_to_name(port_failed), esp_err_to_name(deinit_retry), esp_err_to_name(init_after_port_failure),
             esp_err_to_name(reply_after_port_failure));
    check("after the NimBLE port fails to deinit BLE stays off and the port deinit never runs twice",
          port_failed == ESP_FAIL && deinit_retry == ESP_FAIL && init_after_port_failure == ESP_ERR_INVALID_STATE &&
          reply_after_port_failure == ESP_ERR_INVALID_STATE, d);

    snprintf(d, sizeof d, "a host callback waited or slept=%d, notification buffers not handed to the stack=%d",
             host_blocked_in_host, host_mbuf_live);
    check("NimBLE host callbacks never wait and every notification buffer reaches the stack",
          !host_blocked_in_host && host_mbuf_live == 0, d);
    return check_summary();
}
