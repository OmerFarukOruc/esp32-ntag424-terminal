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

static void gap(struct ble_gap_event ev)
{
    gap_event_handler(&ev, NULL);
}

static void subscribe(uint16_t conn, uint16_t attr, int on)
{
    struct ble_gap_event ev = { .type = BLE_GAP_EVENT_SUBSCRIBE };
    ev.subscribe.conn_handle = conn;
    ev.subscribe.attr_handle = attr;
    ev.subscribe.cur_notify = on;
    gap(ev);
}

static void connect_subscribed(void)
{
    gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_DISCONNECT, .disconnect = { .reason = 0x13 }
    });
    gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_CONNECT, .connect = { 0, 1 }
    });
    host_mtu = BLE_MTU_DEFAULT;
    subscribe(1, s_wifi_scan.val_handle, 1);
    subscribe(1, s_wifi_config.val_handle, 1);
    subscribe(1, s_url_set.val_handle, 1);
    app_admits = true;
    host_notify_reset();
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
    subscribe(1, s_url_set.val_handle, 0);
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
    return check_summary();
}
