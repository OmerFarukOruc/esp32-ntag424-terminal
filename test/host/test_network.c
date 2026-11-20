/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include HAL_NET_SRC
#include "check.h"
#include "host.h"

#define ATTEMPTS_BEFORE_DISCONNECTED 10
#define NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP 3

static struct host_timer *reconnect_timer;
static struct host_timer *got_ip_timer;

static void start(void)
{
    hal_network_deinit();
    host_wifi_reset();
    hal_network_init();
    reconnect_timer = host_timer_named("wifi_reconnect");
    got_ip_timer = host_timer_named("wifi_got_ip");
}

static void deliver(esp_event_base_t base, int32_t id, const void *data, size_t size)
{
    esp_event_post(base, id, data, size, 0);
    host_event_loop_run();
}

static void disconnected(uint8_t reason)
{
    wifi_event_sta_disconnected_t ev = { .reason = reason };
    deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev, sizeof ev);
}

static bool fire(struct host_timer *t)
{
    bool fired = host_timer_fire(t);
    host_event_loop_run();
    return fired;
}

static esp_err_t connect(const char *password)
{
    hal_wifi_credentials_t c = {0};
    strcpy(c.ssid, "Home");
    strcpy(c.password, password);
    esp_err_t err = hal_wifi_connect(&c);
    host_event_loop_run();
    return err;
}

static const char *state_name(void)
{
    static const char *names[] = { "DISCONNECTED", "CONNECTING", "CONNECTED", "ERROR" };
    return names[hal_wifi_get_state()];
}

int main(void)
{
    char d[200];
    char psk[65] = {0};

    start();
    memset(psk, 'z', 64);
    esp_err_t e = connect(psk);
    snprintf(d, sizeof d, "connect=%s disconnects=%d", esp_err_to_name(e), wifi_disconnect_calls);
    check("a 64-byte password that is not hex is refused before the old connection stops",
          e == ESP_ERR_INVALID_ARG && wifi_disconnect_calls == 0, d);

    memset(psk, 'a', 64);
    e = connect(psk);
    snprintf(d, sizeof d, "connect=%s", esp_err_to_name(e));
    check("a 64-digit hex password is accepted", e == ESP_OK, d);

    start();
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    snprintf(d, sizeof d, "connect calls=%d reconnect armed=%d state=%s", wifi_connect_calls, reconnect_timer->armed,
             state_name());
    check("a connect call that fails at STA_START schedules a retry", wifi_connect_calls == 1 && reconnect_timer->armed, d);

    bool fired = fire(reconnect_timer);
    snprintf(d, sizeof d, "fired=%d connect calls=%d reconnect armed=%d", fired, wifi_connect_calls,
             reconnect_timer->armed);
    check("a connect call that fails from the retry timer schedules a retry",
          fired && wifi_connect_calls == 2 && reconnect_timer->armed, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    int disc = wifi_disconnect_calls;
    fired = fire(got_ip_timer);
    snprintf(d, sizeof d, "IP timer fired=%d disconnects=%d", fired, wifi_disconnect_calls - disc);
    check("an association without an IP address in time is disconnected", fired && wifi_disconnect_calls - disc == 1, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    ip_event_got_ip_t ip = {0};
    deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip, sizeof ip);
    snprintf(d, sizeof d, "IP timer armed=%d reconnect armed=%d state=%s", got_ip_timer->armed, reconnect_timer->armed,
             state_name());
    check("an IP address in time keeps the link",
          !got_ip_timer->armed && !reconnect_timer->armed && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED, d);

    start();
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    int attempts = 1;
    while (hal_wifi_get_state() != HAL_WIFI_STATE_DISCONNECTED && attempts < 50 && fire(reconnect_timer)) {
        attempts++;
    }
    snprintf(d, sizeof d, "state=%s after %d failed attempts, reconnect armed=%d", state_name(), attempts,
             reconnect_timer->armed);
    check("connect calls that always fail report DISCONNECTED on the 10th attempt and keep retrying",
          hal_wifi_get_state() == HAL_WIFI_STATE_DISCONNECTED && attempts == ATTEMPTS_BEFORE_DISCONNECTED &&
          reconnect_timer->armed, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip, sizeof ip, 0);
    disc = wifi_disconnect_calls;
    fire(got_ip_timer);
    snprintf(d, sizeof d, "disconnects=%d state=%s", wifi_disconnect_calls - disc, state_name());
    check("the IP timeout does not disconnect a station whose address event is queued",
          wifi_disconnect_calls == disc && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED, d);

    deliver(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL, 0);
    wifi_disconnect_result = ESP_FAIL;
    fired = fire(got_ip_timer);
    snprintf(d, sizeof d, "IP timer fired=%d, reconnect armed=%d", fired, reconnect_timer->armed);
    check("a failed disconnect on IP timeout schedules the next attempt", fired && reconnect_timer->armed, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_event_post_fails = 1;
    disc = wifi_disconnect_calls;
    fire(got_ip_timer);
    int disc_after_lost_post = wifi_disconnect_calls - disc;
    bool rearmed = got_ip_timer->armed;
    fire(got_ip_timer);
    snprintf(d, sizeof d, "disconnects after the lost post=%d, IP timer armed again=%d, disconnects after the repost=%d",
             disc_after_lost_post, rearmed, wifi_disconnect_calls - disc);
    check("an IP timeout that cannot be queued is posted again",
          disc_after_lost_post == 0 && rearmed && wifi_disconnect_calls - disc == 1, d);

    start();
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    fire(reconnect_timer);
    wifi_connect_result = ESP_OK;
    fire(reconnect_timer);
    disconnected(WIFI_REASON_NO_AP_FOUND);
    snprintf(d, sizeof d, "connect calls=%d, then 1 NO_AP_FOUND: reconnect armed=%d state=%s", wifi_connect_calls,
             reconnect_timer->armed, state_name());
    check("one credential failure after two failed connect calls keeps new credentials retrying",
          wifi_connect_calls == 3 && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING && reconnect_timer->armed, d);

    start();
    connect("password1");
    for (int i = 0; i < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP; i++) {
        disconnected(WIFI_REASON_NO_AP_FOUND);
        if (i + 1 < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP) {
            fire(reconnect_timer);
        }
    }
    snprintf(d, sizeof d, "connect calls=%d, after 3 NO_AP_FOUND: reconnect armed=%d state=%s", wifi_connect_calls,
             reconnect_timer->armed, state_name());
    check("new credentials that fail 3 times end in DISCONNECTED with no retry",
          wifi_connect_calls == 3 && hal_wifi_get_state() == HAL_WIFI_STATE_DISCONNECTED && !reconnect_timer->armed, d);

    hal_network_deinit();
    return check_summary();
}
