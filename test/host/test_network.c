/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include HAL_NET_SRC
struct host_timer; extern struct host_timer *host_timer_named(const char *); extern void host_timer_fire(struct host_timer *);
struct host_timer {
    esp_timer_create_args_t args;
    int armed;
    uint64_t us;
    int starts;
};
extern int wifi_connect_calls, wifi_disconnect_calls; extern esp_err_t wifi_connect_result;
#include "check.h"
static hal_wifi_state_t last_cb = -1;
static void cb(hal_wifi_state_t s)
{
    last_cb = s;
}
static int armed_timers(void)
{
    int n = 0;
    for (int i = 0; i < 8; i++) {
        extern struct host_timer host_timers[];
        extern int host_timer_count;
        if (i < host_timer_count && host_timers[i].armed) {
            n++;
        }
    } return n;
}
static void creds(hal_wifi_credentials_t *c, const char *ssid, const char *pwd)
{
    memset(c, 0, sizeof * c);
    strcpy(c->ssid, ssid);
    strcpy(c->password, pwd);
}
int main(void)
{
    char d[200];
    hal_wifi_register_callback(cb);
    hal_network_init();
    hal_wifi_credentials_t c;
    char psk[65]; memset(psk, 'z', 64); psk[64] = 0;
    creds(&c, "Home", psk);
    int disc_before = wifi_disconnect_calls;
    esp_err_t e = hal_wifi_connect(&c);
    snprintf(d, sizeof d, "connect=%s old-connection-stopped=%s", esp_err_to_name(e), wifi_disconnect_calls > disc_before ? "yes" : "no");
    check("a 64-byte password that is not hex is refused before the old connection stops", e == ESP_ERR_INVALID_ARG && wifi_disconnect_calls == disc_before, d);
    memset(psk, 'a', 64); creds(&c, "Home", psk);
    e = hal_wifi_connect(&c);
    snprintf(d, sizeof d, "connect=%s", esp_err_to_name(e));
    check("a 64-digit hex password is accepted", e == ESP_OK, d);

    creds(&c, "Home", "password1");
    hal_wifi_connect(&c);
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    snprintf(d, sizeof d, "armed-timers=%d state=%d", armed_timers(), (int)hal_wifi_get_state());
    check("a connect call that fails at STA_START schedules a retry", host_timer_named("wifi_reconnect")->armed, d);
    wifi_connect_result = ESP_OK;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    struct host_timer *rt = host_timer_named("wifi_reconnect");
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    host_timer_fire(rt);
    snprintf(d, sizeof d, "reconnect-timer-armed=%d state=%d", rt->armed, (int)hal_wifi_get_state());
    check("a connect call that fails from the retry timer schedules a retry", rt->armed, d);
    wifi_connect_result = ESP_OK;
    for (int i = 0; i < 8; i++) {
        extern struct host_timer host_timers[];
        host_timers[i].armed = 0;
    }
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    int disc = wifi_disconnect_calls, fired = 0;
    for (int i = 0; i < 8; i++) {
        extern struct host_timer host_timers[];
        extern int host_timer_count;
        if (i < host_timer_count && host_timers[i].armed) {
            host_timer_fire(&host_timers[i]);
            fired++;
        }
    }
    snprintf(d, sizeof d, "timers-armed-after-association=%d disconnects-after-they-fire=%d state=%d", fired, wifi_disconnect_calls - disc, (int)hal_wifi_get_state());
    check("an association without an IP address in time is disconnected", wifi_disconnect_calls > disc, d);
    for (int i = 0; i < 8; i++) {
        extern struct host_timer host_timers[];
        host_timers[i].armed = 0;
    }
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    ip_event_got_ip_t ip = {0};
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
    snprintf(d, sizeof d, "armed-timers-after-GOT_IP=%d state=%d", armed_timers(), (int)hal_wifi_get_state());
    check("an IP address in time keeps the link", armed_timers() == 0 && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED, d);
    creds(&c, "Home", "password1");
    hal_wifi_connect(&c);
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    int fires = 0;
    while (hal_wifi_get_state() != HAL_WIFI_STATE_DISCONNECTED && fires < 50 && rt->armed) {
        host_timer_fire(rt);
        fires++;
    }
    snprintf(d, sizeof d, "connect always fails: state=%d after %d retries", (int)hal_wifi_get_state(), fires);
    check("connect calls that always fail end in DISCONNECTED", hal_wifi_get_state() == HAL_WIFI_STATE_DISCONNECTED, d);
    extern esp_err_t wifi_disconnect_result;
    struct host_timer *gt = host_timer_named("wifi_got_ip");
    wifi_connect_result = ESP_OK;
    creds(&c, "Home", "password1");
    hal_wifi_connect(&c);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip, sizeof ip, 0);
    disc = wifi_disconnect_calls;
    host_timer_fire(gt);
    snprintf(d, sizeof d, "got-IP event queued when the timer fires: disconnects=%d state=%d", wifi_disconnect_calls - disc, (int)hal_wifi_get_state());
    check("the IP timeout does not disconnect a station whose address event is queued", wifi_disconnect_calls == disc && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED, d);
    for (int i = 0; i < 8; i++) {
        extern struct host_timer host_timers[];
        host_timers[i].armed = 0;
    }
    ip_event_handler(NULL, IP_EVENT, IP_EVENT_STA_LOST_IP, NULL);
    wifi_disconnect_result = ESP_FAIL;
    host_timer_fire(gt);
    wifi_disconnect_result = ESP_OK;
    snprintf(d, sizeof d, "address lost, disconnect failed: reconnect timer armed=%d, IP timer armed=%d", rt->armed, gt->armed);
    check("a failed disconnect on IP timeout schedules the next attempt", rt->armed, d);

    creds(&c, "Home", "password1");
    hal_wifi_connect(&c);
    for (int i = 0; i < 8; i++) {
        extern struct host_timer host_timers[];
        host_timers[i].armed = 0;
    }
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_START, NULL);
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    host_timer_fire(rt);
    host_timer_fire(rt);
    wifi_connect_result = ESP_OK;
    wifi_event_sta_disconnected_t ev = { .reason = WIFI_REASON_NO_AP_FOUND };
    host_timer_fire(rt);
    wifi_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev);
    snprintf(d, sizeof d, "2 failed connect calls then 1 NO_AP_FOUND: reconnect armed=%d state=%d", rt->armed, (int)hal_wifi_get_state());
    check("one credential failure after two failed connect calls keeps new credentials retrying",
          hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING && rt->armed, d);
    return check_summary();
}
