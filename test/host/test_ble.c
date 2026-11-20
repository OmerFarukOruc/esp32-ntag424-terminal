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

extern uint16_t host_mtu;
extern int host_notify_count;
extern char host_notify_payload[];
extern int host_notify_fail_at;
extern int host_notify_enomem_once;
extern void (*host_on_enomem)(void);

static int cb_count;
static char cb_data[256];
static int app_admits = 1;
static hal_ble_session_t cb_session;

static esp_err_t app_cb(hal_ble_event_t e, hal_ble_session_t s, const void *d, size_t n)
{
    (void)e;
    cb_count++;
    cb_session = s;
    if (d) {
        memcpy(cb_data, d, n);
        cb_data[n] = 0;
    }
    return app_admits ? ESP_OK : ESP_ERR_TIMEOUT;
}

void __real_free(void *p);
static int secret_frees;
void __wrap_free(void *p)
{
    if (p && memmem(p, malloc_usable_size(p), "SECRETPW1", 9)) {
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

static void connect_subscribed(uint16_t conn)
{
    gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_DISCONNECT, .disconnect = { .reason = 0x13 }
    });
    gap((struct ble_gap_event) {
        .type = BLE_GAP_EVENT_CONNECT, .connect = { 0, conn }
    });
    host_mtu = 247;
    subscribe(conn, s_wifi_scan.val_handle, 1);
    subscribe(conn, s_wifi_config.val_handle, 1);
    subscribe(conn, s_url_set.val_handle, 1);
    app_admits = 1;
}

static void reconnect_new_client(void)
{
    connect_subscribed(1);
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

int main(void)
{
    char d[240];
    int att;
    esp_err_t e;
    s_wifi_scan.val_handle = 3;
    s_wifi_config.val_handle = 5;
    s_url_set.val_handle = 7;
    hal_ble_init(app_cb);
    connect_subscribed(1);

    check_rejected("bytes after the JSON value are rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"} trailing"));
    check_rejected("an escaped NUL in a value is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\\u0000WRONG\"}"));
    check_rejected("a raw NUL inside the JSON value is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"SH\0OP\"}"));
    check_rejected("a raw NUL after a complete JSON value is rejected",
                   WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"}\0{\"deviceId\":\"WRONG\"}"));
    check_rejected("a \\u escape whose first digit is not hex is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"AB\\uZZZZCD\"}"));
    check_rejected("a \\u escape whose third digit is not hex is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"AB\\u00zzCD\"}"));
    check_rejected("a control byte after the JSON value is rejected", WRITE(url_set_chr_access, "{\"deviceId\":\"ABCD\"}\x01"));

    att = WRITE(wifi_config_chr_access, "{\"ssid\":\"a\\\\u0000b\",\"password\":\"12345678\"}");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d ssid=%s", att, cb_count, cb_count ? ((hal_ble_wifi_config_t *)cb_data)->ssid : "-");
    check("an escaped backslash before u0000 is accepted as text",
          att == 0 && cb_count == 1 && strcmp(((hal_ble_wifi_config_t *)cb_data)->ssid, "a\\u0000b") == 0, d);
    att = WRITE(url_set_chr_access, " {\"deviceId\":\"S\\u0041OP_01\"}\t\r\n");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d id=%s", att, cb_count, cb_count ? cb_data : "-");
    check("a valid \\u escape and JSON whitespace are accepted", att == 0 && cb_count == 1 && strcmp(cb_data, "SAOP_01") == 0, d);

    secret_frees = 0;
    att = WRITE(wifi_config_chr_access, "{\"ssid\":\"A\",\"password\":\"SECRETPW1\"} x");
    snprintf(d, sizeof d, "ATT=0x%02x, freed heap blocks holding the password=%d", att, secret_frees);
    check("a rejected WIFI_CONFIG frees no heap block that holds the password",
          att == BLE_ATT_ERR_VALUE_NOT_ALLOWED && secret_frees == 0, d);
    secret_frees = 0;
    att = WRITE(wifi_config_chr_access, "{\"ssid\":\"A\",\"password\":\"SECRETPW1\"}");
    snprintf(d, sizeof d, "ATT=0x%02x, freed heap blocks holding the password=%d", att, secret_frees);
    check("an accepted WIFI_CONFIG frees no heap block that holds the password", att == 0 && secret_frees == 0, d);

    app_admits = 0;
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    snprintf(d, sizeof d, "ATT=0x%02x (application queue full)", att);
    check("a write the application cannot queue fails with ATT 0x11", att == BLE_ATT_ERR_INSUFFICIENT_RES, d);
    connect_subscribed(1);
    subscribe(1, s_url_set.val_handle, 0);
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d", att, cb_count);
    check("a write with notifications off fails with ATT 0xFD", att == 0xFD && cb_count == 0, d);
    connect_subscribed(1);
    host_mtu = 23;
    att = WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    snprintf(d, sizeof d, "ATT=0x%02x events=%d", att, cb_count);
    check("a write on an MTU under 100 fails with ATT 0xFC", att == 0xFC && cb_count == 0, d);

    connect_subscribed(1);
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_01\"}");
    hal_ble_session_t session_a = cb_session;
    connect_subscribed(1);
    host_notify_count = 0;
    e = hal_ble_send_url_set_result(session_a, true, 0, "sdm_configured");
    snprintf(d, sizeof d, "send=%s notifications=%d", esp_err_to_name(e), host_notify_count);
    check("a reply for a client that left does not reach the next client", e == ESP_ERR_INVALID_STATE && host_notify_count == 0, d);
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP_02\"}");
    host_notify_count = 0;
    e = hal_ble_send_url_set_result(cb_session, true, 0, "sdm_configured");
    snprintf(d, sizeof d, "send=%s notifications=%d payload=%s", esp_err_to_name(e), host_notify_count, host_notify_payload);
    check("a reply reaches the client that sent the request",
          e == ESP_OK && host_notify_count == 1 && strstr(host_notify_payload, "\"message\":\"sdm_configured\"") != NULL, d);

    connect_subscribed(1);
    WRITE(url_set_chr_access, "{\"deviceId\":\"SHOP\"}");
    hal_ble_session_t old = cb_session;
    host_notify_count = 0;
    host_notify_enomem_once = 1;
    host_on_enomem = reconnect_new_client;
    e = hal_ble_send_url_set_result(old, true, 0, "ok");
    host_on_enomem = NULL;
    snprintf(d, sizeof d, "send=%s, notifications to the new client=%d", esp_err_to_name(e), host_notify_count);
    check("a retry after ENOMEM does not reach a client that connected meanwhile",
          e == ESP_ERR_INVALID_STATE && host_notify_count == 0, d);

    connect_subscribed(1);
    WRITE(wifi_scan_chr_access, "{}");
    hal_ble_session_t scan_session = cb_session;
    hal_ble_wifi_scan_result_t r = { .rssi = -127, .channel = 11 };
    strcpy(r.security, "WPA3");
    size_t fixed = strlen("{\"ssid\":\"\",\"rssi\":-127,\"sec\":\"WPA3\",\"ch\":11}");
    memset(r.ssid, 'A', 32);
    r.ssid[32] = 0;
    host_mtu = (uint16_t)(fixed + 32 + 3);
    host_notify_count = 0;
    e = hal_ble_send_wifi_scan_results(scan_session, &r, 1);
    snprintf(d, sizeof d, "MTU %u, entry %zu bytes: send=%s notifications=%d last=%s", host_mtu, fixed + 32,
             esp_err_to_name(e), host_notify_count, host_notify_payload);
    check("a scan entry of exactly MTU minus 3 bytes is sent",
          e == ESP_OK && host_notify_count == 2 && strstr(host_notify_payload, "\"count\":1,\"skipped\":0") != NULL, d);

    hal_ble_wifi_scan_result_t three[3] = { r, r, r };
    for (int k = 0; k < 3; k++) {
        snprintf(three[k].ssid, sizeof three[k].ssid, "net%d", k);
    }
    host_mtu = 247;
    host_notify_count = 0;
    host_notify_fail_at = 2;
    e = hal_ble_send_wifi_scan_results(scan_session, three, 3);
    host_notify_fail_at = 0;
    snprintf(d, sizeof d, "3 networks, the 2nd send fails: notifications=%d last=%s", host_notify_count, host_notify_payload);
    check("the terminator after a failed send counts every network not sent",
          strstr(host_notify_payload, "\"done\":true,\"count\":1,\"skipped\":2") != NULL, d);
    return check_summary();
}
