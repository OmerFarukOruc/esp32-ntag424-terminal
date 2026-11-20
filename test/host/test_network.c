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
#define ATTEMPT_DEADLINE_US (180LL * 1000 * 1000)
#define IP_A 0x0100a8c0u
#define IP_B 0x0200a8c0u

static struct host_timer *reconnect_timer;
static struct host_timer *got_ip_timer;
static hal_wifi_attempt_id_t attempt;
static int state_callbacks;
static int critical_calls;

static void count_state(hal_wifi_state_t state)
{
    state_callbacks++;
}

static void start(void)
{
    hal_network_deinit();
    critical_calls += wifi_calls_in_critical;
    host_wifi_reset();
    host_now_us = 1000000;
    hal_network_init();
    reconnect_timer = host_timer_named("wifi_reconnect");
    got_ip_timer = host_timer_named("wifi_got_ip");
    attempt = 0;
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

static esp_err_t connect_queued(const char *ssid, const char *password)
{
    hal_wifi_credentials_t c = {0};
    strcpy(c.ssid, ssid);
    strcpy(c.password, password);
    return hal_wifi_connect(&c, &attempt);
}

static esp_err_t connect_to(const char *ssid, const char *password)
{
    esp_err_t err = connect_queued(ssid, password);
    host_event_loop_run();
    return err;
}

static esp_err_t connect(const char *password)
{
    return connect_to("Home", password);
}

static void queue_got_ip(uint32_t addr)
{
    ip_event_got_ip_t ev = { .ip_info.ip.addr = addr };
    esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &ev, sizeof ev, 0);
}

static void got_ip(uint32_t addr)
{
    queue_got_ip(addr);
    host_event_loop_run();
}

static void join(const char *ssid, uint32_t addr)
{
    host_wifi_link_up(ssid, addr);
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    got_ip(addr);
}

static const char *status_name(hal_wifi_attempt_status_t status)
{
    static const char *names[] = { "NONE", "PENDING", "SUCCEEDED", "FAILED" };
    return names[status];
}

static const char *state_name(void)
{
    static const char *names[] = { "DISCONNECTED", "CONNECTING", "CONNECTED", "ERROR" };
    return names[hal_wifi_get_state()];
}

static void connect_b_during_live_check(void)
{
    connect_queued("HomeB", "password2");
}

static int stale_hook_runs;

static void stale_timeout_meets_connect(void)
{
    stale_hook_runs++;
    unsigned generation = atomic_load(&s_network.connect_generation);
    got_ip_timeout_handler(NULL, HAL_NETWORK_EVENT, 0, &generation);
}

static void stale_ip_checks(char *d, size_t n)
{
    start();
    connect_to("HomeA", "password1");
    host_wifi_link_up("HomeA", IP_A);
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    hal_wifi_attempt_id_t first = attempt;
    queue_got_ip(IP_A);
    connect_queued("HomeB", "password2");
    hal_wifi_attempt_id_t second = attempt;
    host_event_loop_run();
    hal_wifi_attempt_status_t status = hal_wifi_attempt_take(second);
    snprintf(d, n, "attempt B=%s, state=%s, attempt A=%s", status_name(status), state_name(),
             status_name(hal_wifi_attempt_take(first)));
    check("a GOT_IP queued before replacement credentials does not complete them",
          status == HAL_WIFI_ATTEMPT_PENDING && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING, d);

    join("HomeB", IP_B);
    status = hal_wifi_attempt_take(second);
    hal_wifi_attempt_status_t again = hal_wifi_attempt_take(second);
    snprintf(d, n, "first take=%s, second take=%s, state=%s", status_name(status), status_name(again), state_name());
    check("positive control: the IP address of the live new association completes the attempt",
          status == HAL_WIFI_ATTEMPT_SUCCEEDED && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED, d);
    check("a result is handed out once", again == HAL_WIFI_ATTEMPT_NONE, d);

    start();
    connect("password1");
    host_wifi_link_up("Other", IP_A);
    got_ip(IP_A);
    snprintf(d, n, "attempt=%s, state=%s", status_name(hal_wifi_attempt_take(attempt)), state_name());
    check("a GOT_IP while the driver is associated with another network is ignored",
          hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING && hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    host_wifi_link_up("Home", IP_B);
    got_ip(IP_A);
    snprintf(d, n, "state=%s", state_name());
    check("a GOT_IP whose address the interface does not hold is ignored",
          hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING && hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    host_wifi_link_up("Home", 0);
    got_ip(0);
    snprintf(d, n, "state=%s", state_name());
    check("a GOT_IP with address 0 is ignored",
          hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING && hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    host_wifi_link_up("Home", IP_A);
    host_wifi_link_down();
    got_ip(IP_A);
    snprintf(d, n, "state=%s", state_name());
    check("a GOT_IP after the link went down is ignored although the interface keeps its address",
          hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING && hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_timer_fire(got_ip_timer);
    connect_queued("Home", "password2");
    int disc = wifi_disconnect_calls;
    host_event_loop_run();
    snprintf(d, n, "disconnects after the replacement=%d, attempt=%s", wifi_disconnect_calls - disc,
             status_name(hal_wifi_attempt_take(attempt)));
    check("an IP timeout queued by an earlier connection does not disconnect the new one",
          wifi_disconnect_calls == disc && hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect_to("HomeA", "password1");
    host_wifi_link_up("HomeA", IP_A);
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_wifi_hook_call = "esp_netif_get_ip_info";
    host_wifi_hook = connect_b_during_live_check;
    got_ip(IP_A);
    status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "attempt B=%s, state=%s, credentials unproven=%d", status_name(status), state_name(),
             atomic_load(&s_network.credentials_unproven));
    check("a GOT_IP of A that passed the live check does not complete B started meanwhile",
          status == HAL_WIFI_ATTEMPT_PENDING && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING &&
          atomic_load(&s_network.credentials_unproven), d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    connect_queued("Home", "password2");
    host_event_loop_run();
    disc = wifi_disconnect_calls;
    got_ip_timer->args.callback(got_ip_timer->args.arg);
    host_event_loop_run();
    snprintf(d, n, "disconnects=%d, attempt=%s", wifi_disconnect_calls - disc,
             status_name(hal_wifi_attempt_take(attempt)));
    check("an IP timeout callback of an earlier connection that runs after a replacement does not disconnect it",
          wifi_disconnect_calls == disc && hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_timer_stop_hook_name = "wifi_got_ip";
    host_timer_stop_hook = stale_timeout_meets_connect;
    stale_hook_runs = 0;
    connect_queued("Home", "password2");
    host_event_loop_run();
    disc = wifi_disconnect_calls;
    host_advance(1000000);
    snprintf(d, n, "timeout handler ran during the connect=%d, disconnects after the re-armed timer=%d, attempt=%s",
             stale_hook_runs, wifi_disconnect_calls - disc, status_name(hal_wifi_attempt_take(attempt)));
    check("an IP timeout that meets a connect call re-arms for its own connection and cannot disconnect the new one",
          stale_hook_runs == 1 && wifi_disconnect_calls == disc &&
          hal_wifi_attempt_take(attempt) == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    wifi_connect_result = ESP_OK;
    connect_queued("Home", "password2");
    host_event_loop_run();
    int connects = wifi_connect_calls;
    reconnect_timer->args.callback(reconnect_timer->args.arg);
    snprintf(d, n, "connect calls after the stale callback=%d", wifi_connect_calls - connects);
    check("a reconnect timer callback of an earlier connection that runs after a replacement does not call connect",
          wifi_connect_calls == connects, d);

    start();
    connect("password1");
    unsigned old_generation = current_generation();
    connect("password2");
    disconnected(WIFI_REASON_AUTH_FAIL);
    int retries = s_network.retry_count;
    int failures = s_network.credential_failures;
    int backoff = s_network.backoff_ms;
    hal_wifi_state_t state = hal_wifi_get_state();
    int callbacks = state_callbacks;
    retry_after_failure(old_generation, WIFI_REASON_AUTH_FAIL);
    bool reset = retry_reset(old_generation);
    snprintf(d, n, "retry count %d to %d, credential failures %d to %d, backoff %d to %d ms, reset reported=%d", retries,
             s_network.retry_count, failures, s_network.credential_failures, backoff, s_network.backoff_ms, reset);
    check("a failure or a retry reset of an earlier connection changes no retry counter",
          !reset && s_network.retry_count == retries && s_network.credential_failures == failures &&
          s_network.backoff_ms == backoff, d);

    set_state(old_generation, HAL_WIFI_STATE_CONNECTED);
    snprintf(d, n, "state %s, callbacks %d", state_name(), state_callbacks - callbacks);
    check("a state change of an earlier connection changes no state and calls no callback",
          hal_wifi_get_state() == state && state_callbacks == callbacks, d);

    esp_timer_stop(s_network.reconnect_timer);
    arm_reconnect_timer(old_generation, 5000);
    snprintf(d, n, "retry timer armed=%d", reconnect_timer->armed);
    check("a retry timer of an earlier connection is not armed", !reconnect_timer->armed, d);

    op_claim(OP_DRIVER);
    start_connect(old_generation);
    bool deferred = s_network.retry_pending;
    op_abandon();
    snprintf(d, n, "retry pending after the refused claim=%d", deferred);
    check("a connect start of an earlier connection that meets a running operation is not deferred", !deferred, d);
}

static void slow_setup(void)
{
    host_now_us += 5000000;
}

static hal_wifi_attempt_status_t status_after_slow_connect(bool before_record)
{
    start();
    connect("password1");
    int64_t accepted_us = host_now_us;
    if (before_record) {
        host_timer_stop_hook_name = "wifi_reconnect";
        host_timer_stop_hook = slow_setup;
    } else {
        host_wifi_hook_call = "esp_wifi_set_config";
        host_wifi_hook = slow_setup;
    }
    connect("password2");
    host_now_us = accepted_us + ATTEMPT_DEADLINE_US;
    return hal_wifi_attempt_take(attempt);
}

static void deadline_checks(char *d, size_t n)
{
    start();
    connect("password1");
    int64_t deadline = host_now_us + ATTEMPT_DEADLINE_US;
    host_now_us = deadline - 1;
    join("Home", IP_A);
    hal_wifi_attempt_status_t status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "1 us before the deadline: %s", status_name(status));
    check("positive control: an IP address 1 us before the deadline completes the attempt",
          status == HAL_WIFI_ATTEMPT_SUCCEEDED, d);

    start();
    connect("password1");
    host_now_us += ATTEMPT_DEADLINE_US;
    join("Home", IP_A);
    status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "at the deadline: %s", status_name(status));
    check("an IP address at the deadline does not complete the attempt", status == HAL_WIFI_ATTEMPT_FAILED, d);

    start();
    int callbacks = state_callbacks;
    connect("password1");
    int after_connect = state_callbacks;
    host_now_us += ATTEMPT_DEADLINE_US - 1;
    hal_wifi_attempt_status_t before = hal_wifi_attempt_take(attempt);
    host_now_us += 1;
    status = hal_wifi_attempt_take(attempt);
    hal_wifi_attempt_status_t again = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "before=%s at=%s again=%s, state callbacks after the connect=%d, state=%s", status_name(before),
             status_name(status), status_name(again), state_callbacks - after_connect, state_name());
    check("a polled attempt ends as failed at the deadline with no event and no state change, and reports once",
          before == HAL_WIFI_ATTEMPT_PENDING && status == HAL_WIFI_ATTEMPT_FAILED && again == HAL_WIFI_ATTEMPT_NONE &&
          state_callbacks == after_connect && callbacks < after_connect, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    uint64_t dhcp_delay_us = got_ip_timer->delay_us;
    int disc = wifi_disconnect_calls;
    host_advance((int64_t)dhcp_delay_us - 1);
    int disconnects_before = wifi_disconnect_calls - disc;
    host_advance(1);
    snprintf(d, n, "IP timer armed for %llu us, disconnects 1 us before=%d, at its time=%d",
             (unsigned long long)dhcp_delay_us, disconnects_before, wifi_disconnect_calls - disc);
    check("the IP address timer is armed for 30 s and disconnects at that time and not before",
          dhcp_delay_us == 30000000 && disconnects_before == 0 && wifi_disconnect_calls - disc == 1, d);

    start();
    connect("password1");
    deadline = host_now_us + ATTEMPT_DEADLINE_US;
    int cycles = 0;
    int64_t last_pending_us = host_now_us;
    status = HAL_WIFI_ATTEMPT_PENDING;
    while (status == HAL_WIFI_ATTEMPT_PENDING && cycles < ATTEMPTS_BEFORE_DISCONNECTED + 2) {
        host_wifi_link_up("Home", IP_A);
        deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
        host_advance((int64_t)got_ip_timer->delay_us);
        cycles++;
        status = hal_wifi_attempt_take(attempt);
        if (status == HAL_WIFI_ATTEMPT_PENDING) {
            last_pending_us = host_now_us;
            host_advance((int64_t)reconnect_timer->delay_us);
        }
    }
    snprintf(d, n, "attempt %s after %d joins without an IP address, %lld ms after acceptance, state=%s, retry armed=%d",
             status_name(status), cycles, (long long)((host_now_us - (deadline - ATTEMPT_DEADLINE_US)) / 1000),
             state_name(), reconnect_timer->armed);
    check("a network that never answers DHCP ends the attempt at the deadline, and the background retries go on",
          status == HAL_WIFI_ATTEMPT_FAILED && host_now_us >= deadline && last_pending_us < deadline &&
          cycles < ATTEMPTS_BEFORE_DISCONNECTED && hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTING &&
          reconnect_timer->armed, d);

    start();
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    static const uint64_t backoff_s[] = { 1, 2, 4, 8, 16, 30, 30, 30, 30 };
    int matching = 0;
    int failed_calls = 1;
    status = hal_wifi_attempt_take(attempt);
    while (status == HAL_WIFI_ATTEMPT_PENDING && failed_calls < 50) {
        matching += failed_calls <= 9 && reconnect_timer->delay_us == backoff_s[failed_calls - 1] * 1000000;
        host_advance((int64_t)reconnect_timer->delay_us);
        failed_calls++;
        status = hal_wifi_attempt_take(attempt);
    }
    snprintf(d, n, "attempt %s after %d failed connect calls, %d of 9 backoffs as documented, next backoff %llu us",
             status_name(status), failed_calls, matching, (unsigned long long)reconnect_timer->delay_us);
    check("repeated immediate connect errors back off 1, 2, 4, 8, 16 then 30 s",
          matching == 9 && reconnect_timer->delay_us == 30000000, d);
    check("positive control: repeated immediate connect errors end the attempt as failed on the 10th",
          status == HAL_WIFI_ATTEMPT_FAILED && failed_calls == ATTEMPTS_BEFORE_DISCONNECTED, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_event_post_fails = 1;
    host_timer_fire(got_ip_timer);
    snprintf(d, n, "IP timer armed=%d for %llu us", got_ip_timer->armed, (unsigned long long)got_ip_timer->delay_us);
    check("an IP timeout that cannot be queued is posted again after 1 s",
          got_ip_timer->armed && got_ip_timer->delay_us == 1000000, d);

    status = status_after_slow_connect(false);
    snprintf(d, n, "attempt %s at 180 s after the claim, with 5 s lost in the driver config step", status_name(status));
    check("the deadline runs from the claim of the connect call, not from the end of its driver calls",
          status == HAL_WIFI_ATTEMPT_FAILED, d);

    status = status_after_slow_connect(true);
    snprintf(d, n, "attempt %s at 180 s after the claim, with 5 s lost in the timer stop step", status_name(status));
    check("the deadline runs from the claim of the connect call, not from the creation of the attempt record",
          status == HAL_WIFI_ATTEMPT_FAILED, d);

    start();
    connect("password1");
    for (int i = 0; i < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP; i++) {
        disconnected(WIFI_REASON_AUTH_FAIL);
        if (i + 1 < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP) {
            fire(reconnect_timer);
        }
    }
    status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "attempt %s, reconnect armed=%d", status_name(status), reconnect_timer->armed);
    check("positive control: 3 credential failures end the attempt as failed and the retries",
          status == HAL_WIFI_ATTEMPT_FAILED && !reconnect_timer->armed, d);

    start();
    host_timer_start_fails = 1;
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "attempt %s", status_name(status));
    check("a retry timer that does not start ends the attempt as failed", status == HAL_WIFI_ATTEMPT_FAILED, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_timer_start_fails = 1;
    deliver(IP_EVENT, IP_EVENT_STA_LOST_IP, NULL, 0);
    status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "attempt %s", status_name(status));
    check("an IP timer that does not start ends the attempt as failed", status == HAL_WIFI_ATTEMPT_FAILED, d);
}

static void outcome_checks(char *d, size_t n)
{
    start();
    esp_err_t e = connect("short");
    hal_wifi_attempt_id_t refused = attempt;
    attempt = 77;
    hal_wifi_credentials_t c = {0};
    strcpy(c.ssid, "Home");
    strcpy(c.password, "password1");
    esp_err_t no_out = hal_wifi_connect(&c, NULL);
    snprintf(d, n, "refused connect=%s id=%u, NULL output=%s", esp_err_to_name(e), (unsigned)refused,
             esp_err_to_name(no_out));
    check("positive control: a refused connect returns attempt id 0, and a missing id output is refused",
          e == ESP_ERR_INVALID_ARG && refused == 0 && no_out == ESP_ERR_INVALID_ARG, d);

    start();
    connect("password1");
    hal_wifi_attempt_id_t first = attempt;
    connect("password2");
    hal_wifi_attempt_id_t second = attempt;
    host_wifi_link_up("Home", IP_A);
    hal_wifi_attempt_status_t replaced = hal_wifi_attempt_take(first);
    snprintf(d, n, "ids %u then %u, the first attempt after the second started: %s", (unsigned)first, (unsigned)second,
             status_name(replaced));
    check("each accepted attempt gets a nonzero id that is not reused, and a new connect retires the old attempt",
          first != 0 && second != 0 && second != first && replaced == HAL_WIFI_ATTEMPT_NONE, d);

    hal_network_deinit();
    hal_network_init();
    connect("password1");
    snprintf(d, n, "id after a deinit and init: %u, earlier ids %u and %u", (unsigned)attempt, (unsigned)first,
             (unsigned)second);
    check("attempt ids keep counting across a deinit and init", attempt != first && attempt != second, d);

    start();
    connect("password1");
    host_wifi_link_up("Home", IP_A);
    hal_wifi_attempt_cancel(attempt);
    hal_wifi_attempt_status_t cancelled = hal_wifi_attempt_take(attempt);
    hal_wifi_attempt_id_t old = attempt;
    got_ip(IP_A);
    hal_wifi_attempt_status_t after_event = hal_wifi_attempt_take(old);
    connect("password2");
    hal_wifi_attempt_status_t fresh = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "cancelled id: %s, after the IP event: %s, new id %u (old %u), new attempt: %s",
             status_name(cancelled), status_name(after_event), (unsigned)attempt, (unsigned)old, status_name(fresh));
    check("a cancelled attempt reports nothing later, and the next attempt gets a fresh id",
          cancelled == HAL_WIFI_ATTEMPT_NONE && after_event == HAL_WIFI_ATTEMPT_NONE && attempt != old &&
          fresh == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    for (int i = 0; i < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP; i++) {
        disconnected(WIFI_REASON_AUTH_FAIL);
        if (i + 1 < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP) {
            fire(reconnect_timer);
        }
    }
    join("Home", IP_A);
    hal_wifi_attempt_status_t kept = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "attempt %s", status_name(kept));
    check("a stored failure is not overwritten by a later IP address", kept == HAL_WIFI_ATTEMPT_FAILED, d);

    start();
    connect("password1");
    first = attempt;
    wifi_stop_result = ESP_FAIL;
    e = connect("password2");
    hal_wifi_attempt_status_t retired = hal_wifi_attempt_take(first);
    snprintf(d, n, "second connect=%s id=%u, first attempt %u: %s", esp_err_to_name(e), (unsigned)attempt,
             (unsigned)first, status_name(retired));
    check("a connect that fails after it stopped the old connection retires the old attempt",
          e == ESP_FAIL && retired == HAL_WIFI_ATTEMPT_NONE, d);
    wifi_stop_result = ESP_OK;

    start();
    wifi_start_result = ESP_FAIL;
    e = connect("password1");
    hal_wifi_attempt_id_t tentative = s_network.last_attempt_id;
    hal_wifi_attempt_status_t left = hal_wifi_attempt_take(tentative);
    snprintf(d, n, "connect=%s id=%u state=%s, tentative attempt %u: %s", esp_err_to_name(e), (unsigned)attempt,
             state_name(), (unsigned)tentative, status_name(left));
    check("a connect that fails after it stopped the old connection returns id 0 and leaves no attempt",
          e == ESP_FAIL && attempt == 0 && hal_wifi_get_state() == HAL_WIFI_STATE_DISCONNECTED &&
          left == HAL_WIFI_ATTEMPT_NONE, d);
    wifi_start_result = ESP_OK;
    hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX];
    uint16_t count = 99;
    e = hal_wifi_scan(aps, &count);
    snprintf(d, n, "scan=%s", esp_err_to_name(e));
    check("positive control: a failed connect call releases the Wi-Fi driver for the next scan", e == ESP_OK, d);
}

static hal_wifi_credentials_t contender = { .ssid = "Other", .password = "password9" };
static esp_err_t contender_connect_err;
static esp_err_t contender_deinit_err;
static esp_err_t contender_scan_err;
static hal_wifi_attempt_id_t contender_attempt = 123;
static int snapshot_mutations;
static int snapshot_timers_alive;

static void reconnect_timer_fires_for_current_connection(void)
{
    atomic_store(&s_network.reconnect_timer_generation, atomic_load(&s_network.connect_generation));
    reconnect_timer->args.callback(reconnect_timer->args.arg);
}

static int driver_mutations(void)
{
    return wifi_disconnect_calls + wifi_stop_calls + wifi_set_config_calls + wifi_start_calls + wifi_connect_calls;
}

static void contend_with_connect(void)
{
    snapshot_mutations = driver_mutations();
    contender_connect_err = hal_wifi_connect(&contender, &contender_attempt);
}

static void contend_with_deinit(void)
{
    snapshot_mutations = driver_mutations();
    contender_deinit_err = hal_network_deinit();
    snapshot_timers_alive = host_timer_named("wifi_reconnect") != NULL && host_timer_named("wifi_got_ip") != NULL;
}

static void contend_with_scan(void)
{
    hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX];
    uint16_t found = 99;
    snapshot_mutations = wifi_scan_calls;
    contender_scan_err = hal_wifi_scan(aps, &found);
    contender_attempt = found;
}

static void contend_with_reconnect_and_connect(void)
{
    snapshot_mutations = driver_mutations();
    reconnect_timer->armed = false;
    reconnect_timer_fires_for_current_connection();
    contender_connect_err = hal_wifi_connect(&contender, &contender_attempt);
}

static int timeout_disconnects;
static void run_got_ip_timeout(void)
{
    int before = wifi_disconnect_calls;
    host_timer_fire(got_ip_timer);
    host_event_loop_run();
    timeout_disconnects = wifi_disconnect_calls - before;
}

static int state_in_scan;
static bool retry_armed_in_scan;
static void drop_link_during_scan(void)
{
    host_wifi_link_down();
    disconnected(WIFI_REASON_ASSOC_LEAVE);
    state_in_scan = hal_wifi_get_state();
    retry_armed_in_scan = reconnect_timer->armed;
}

static void contend_with_reconnect_callback(void)
{
    host_wifi_link_down();
    reconnect_timer->armed = false;
    reconnect_timer_fires_for_current_connection();
}

static void run_event_loop(void)
{
    host_event_loop_run();
}

static void lifecycle_checks(char *d, size_t n)
{
    start();
    host_timer_start_fails = 1;
    host_timer_start_error = ESP_ERR_INVALID_STATE;
    wifi_connect_result = ESP_ERR_WIFI_CONN;
    connect("password1");
    hal_wifi_attempt_status_t status = hal_wifi_attempt_take(attempt);
    host_timer_start_error = ESP_FAIL;
    snprintf(d, n, "attempt %s", status_name(status));
    check("a retry timer that another task armed first does not end the attempt",
          status == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    s_network.last_attempt_id = UINT32_MAX;
    connect("password1");
    status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "id after the largest id: %u, attempt %s", (unsigned)attempt, status_name(status));
    check("the attempt id counter skips the reserved id 0 when it wraps",
          attempt != 0 && status == HAL_WIFI_ATTEMPT_PENDING, d);

    start();
    connect("password1");
    disconnected(WIFI_REASON_AUTH_FAIL);
    bool armed = reconnect_timer->armed;
    esp_err_t e = hal_network_deinit();
    snprintf(d, n, "retry timer armed before=%d, deinit=%s, timers left=%d", armed, esp_err_to_name(e),
             host_timer_named("wifi_reconnect") != NULL);
    check("a deinit stops an armed retry timer and deletes it",
          armed && e == ESP_OK && host_timer_named("wifi_reconnect") == NULL, d);

    start();
    host_timer_delete_fails = 1;
    e = hal_network_deinit();
    bool kept = s_network.reconnect_timer != NULL;
    esp_err_t again = hal_network_deinit();
    snprintf(d, n, "deinit while a timer does not delete=%s, handle kept=%d, retry=%s", esp_err_to_name(e), kept,
             esp_err_to_name(again));
    check("a deinit that cannot delete a timer reports it, keeps the handle, and its retry finishes",
          e == ESP_ERR_INVALID_STATE && kept && again == ESP_OK && s_network.reconnect_timer == NULL, d);

    start();
    connect("password1");
    unsigned generation = current_generation();
    hal_network_deinit();
    int connects = wifi_connect_calls;
    int callbacks = state_callbacks;
    start_connect(generation);
    snprintf(d, n, "connect calls after the deinit=%d, state callbacks=%d", wifi_connect_calls - connects,
             state_callbacks - callbacks);
    check("a connect start that runs after the deinit touches no driver and reports no state",
          wifi_connect_calls == connects && state_callbacks == callbacks, d);
}

static void gate_checks(char *d, size_t n)
{
    hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX];
    uint16_t count;

    start();
    host_wifi_hook_call = "esp_wifi_scan_start";
    host_wifi_hook = contend_with_connect;
    esp_err_t scan_err = hal_wifi_scan(aps, &count);
    snprintf(d, n, "scan=%s, connect during the scan=%s id=%u, driver mutations during the scan=%d, overlapping calls=%d",
             esp_err_to_name(scan_err), esp_err_to_name(contender_connect_err), (unsigned)contender_attempt,
             driver_mutations() - snapshot_mutations, wifi_overlapping_calls);
    check("a connect requested during a scan is refused before any driver call, and the scan finishes",
          scan_err == ESP_OK && contender_connect_err == ESP_ERR_NOT_FINISHED && contender_attempt == 0 &&
          driver_mutations() == snapshot_mutations && wifi_overlapping_calls == 0, d);

    start();
    host_wifi_hook_call = "esp_wifi_set_config";
    host_wifi_hook = contend_with_scan;
    esp_err_t connect_err = connect("password1");
    snprintf(d, n, "connect=%s, scan during the connect=%s found=%u, scan driver calls=%d, overlapping calls=%d",
             esp_err_to_name(connect_err), esp_err_to_name(contender_scan_err), (unsigned)contender_attempt,
             wifi_scan_calls, wifi_overlapping_calls);
    check("a scan requested during a connect is refused with an empty result before any driver call",
          connect_err == ESP_OK && contender_scan_err == ESP_ERR_NOT_FINISHED && contender_attempt == 0 &&
          wifi_scan_calls == 0 && wifi_overlapping_calls == 0, d);

    start();
    connect("password1");
    count = 99;
    scan_err = hal_wifi_scan(aps, &count);
    host_now_us += ATTEMPT_DEADLINE_US - 1;
    join("Home", IP_A);
    hal_wifi_attempt_status_t status = hal_wifi_attempt_take(attempt);
    snprintf(d, n, "scan=%s count=%u scan driver calls=%d, attempt then %s", esp_err_to_name(scan_err),
             (unsigned)count, wifi_scan_calls, status_name(status));
    check("a scan requested while an attempt is pending is refused and leaves the attempt and its deadline alone",
          scan_err == ESP_ERR_NOT_FINISHED && count == 0 && wifi_scan_calls == 0 && status == HAL_WIFI_ATTEMPT_SUCCEEDED, d);

    start();
    host_wifi_hook_call = "esp_wifi_connect";
    host_wifi_hook = contend_with_reconnect_and_connect;
    connect("password1");
    snprintf(d, n, "connect calls=%d overlapping=%d, connect during the start=%s id=%u, retry armed=%d",
             wifi_connect_calls, wifi_overlapping_calls, esp_err_to_name(contender_connect_err),
             (unsigned)contender_attempt, reconnect_timer->armed);
    check("a reconnect and a connect that meet a reconnect start do not overlap it, and the reconnect resumes",
          wifi_connect_calls == 1 && wifi_overlapping_calls == 0 && contender_connect_err == ESP_ERR_NOT_FINISHED &&
          contender_attempt == 0 && driver_mutations() == snapshot_mutations + 1 && reconnect_timer->armed, d);
    fire(reconnect_timer);
    snprintf(d, n, "connect calls=%d", wifi_connect_calls);
    check("the deferred reconnect runs once when its timer fires", wifi_connect_calls == 2, d);

    start();
    host_wifi_hook_call = "esp_wifi_connect";
    host_wifi_hook = contend_with_deinit;
    connect("password1");
    snprintf(d, n, "deinit during the reconnect start=%s, timers alive=%d, driver mutations during it=%d",
             esp_err_to_name(contender_deinit_err), snapshot_timers_alive, driver_mutations() - snapshot_mutations);
    check("a deinit during a reconnect start is refused with timers and driver untouched",
          contender_deinit_err == ESP_ERR_NOT_FINISHED && snapshot_timers_alive && driver_mutations() == snapshot_mutations + 1 &&
          wifi_stop_calls == 1, d);

    start();
    host_wifi_hook_call = "esp_wifi_start";
    host_wifi_hook = run_event_loop;
    connect_queued("Home", "password1");
    int during = wifi_connect_calls;
    snprintf(d, n, "connect calls during the connect=%d overlapping=%d retry armed=%d", during,
             wifi_overlapping_calls, reconnect_timer->armed);
    check("a STA_START handled while a connect call runs defers its connect until the call ends",
          during == 0 && wifi_overlapping_calls == 0 && reconnect_timer->armed, d);
    fire(reconnect_timer);
    snprintf(d, n, "connect calls=%d", wifi_connect_calls);
    check("positive control: the deferred connect starts when the retry timer fires", wifi_connect_calls == 1, d);

    start();
    host_wifi_hook_call = "esp_wifi_scan_start";
    host_wifi_hook = contend_with_deinit;
    scan_err = hal_wifi_scan(aps, &count);
    int refused_stops = wifi_stop_calls;
    int mutations_after_scan = driver_mutations();
    esp_err_t deinit_after = hal_network_deinit();
    esp_err_t init_after = hal_network_init();
    snprintf(d, n, "deinit during the scan=%s, timers alive=%d, driver mutations during the scan=%d, deinit after=%s, "
             "stop calls=%d, init after=%s", esp_err_to_name(contender_deinit_err), snapshot_timers_alive,
             mutations_after_scan - snapshot_mutations, esp_err_to_name(deinit_after), wifi_stop_calls,
             esp_err_to_name(init_after));
    check("a deinit during a scan is refused with timers and driver untouched, and succeeds once after the scan",
          scan_err == ESP_OK && contender_deinit_err == ESP_ERR_NOT_FINISHED && snapshot_timers_alive &&
          mutations_after_scan == snapshot_mutations && deinit_after == ESP_OK && refused_stops == 0 &&
          wifi_stop_calls == 1 && init_after == ESP_OK, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    hal_wifi_attempt_cancel(attempt);
    host_wifi_hook_call = "esp_wifi_scan_start";
    host_wifi_hook = run_got_ip_timeout;
    scan_err = hal_wifi_scan(aps, &count);
    int disconnects_in_scan = timeout_disconnects;
    snprintf(d, n, "disconnects during the scan=%d, IP timer armed again=%d, overlapping=%d", disconnects_in_scan,
             got_ip_timer->armed, wifi_overlapping_calls);
    check("an IP timeout that meets a scan does not disconnect during it and tries again later",
          scan_err == ESP_OK && disconnects_in_scan == 0 && got_ip_timer->armed && wifi_overlapping_calls == 0, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_wifi_hook_call = "esp_wifi_disconnect";
    host_wifi_hook = contend_with_reconnect_callback;
    int connects = wifi_connect_calls;
    host_timer_fire(got_ip_timer);
    host_event_loop_run();
    snprintf(d, n, "connect calls during the disconnect=%d, retry armed after it=%d, overlapping=%d",
             wifi_connect_calls - connects, reconnect_timer->armed, wifi_overlapping_calls);
    check("a reconnect that meets the disconnect of an IP timeout waits and resumes after it",
          wifi_connect_calls == connects && reconnect_timer->armed && wifi_overlapping_calls == 0, d);

    start();
    connect("password1");
    hal_wifi_attempt_cancel(attempt);
    reconnect_timer->armed = false;
    scan_err = hal_wifi_scan(aps, &count);
    snprintf(d, n, "scan=%s, retry armed after the scan=%d", esp_err_to_name(scan_err), reconnect_timer->armed);
    check("positive control: after a scan the HAL starts the saved network again when the station is not connected",
          scan_err == ESP_OK && reconnect_timer->armed, d);

    start();
    connect("password1");
    hal_wifi_attempt_cancel(attempt);
    host_wifi_link_up("Home", IP_A);
    reconnect_timer->armed = false;
    scan_err = hal_wifi_scan(aps, &count);
    snprintf(d, n, "scan=%s, retry armed after the scan=%d", esp_err_to_name(scan_err), reconnect_timer->armed);
    check("positive control: after a scan the HAL leaves a connected station alone", scan_err == ESP_OK && !reconnect_timer->armed, d);

    start();
    connect("password1");
    hal_wifi_attempt_cancel(attempt);
    host_wifi_forget_config();
    reconnect_timer->armed = false;
    scan_err = hal_wifi_scan(aps, &count);
    snprintf(d, n, "scan=%s, retry armed after the scan=%d", esp_err_to_name(scan_err), reconnect_timer->armed);
    check("positive control: after a scan with no saved network the HAL starts no connect", scan_err == ESP_OK && !reconnect_timer->armed, d);

    start();
    connect("password1");
    for (int i = 0; i < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP; i++) {
        disconnected(WIFI_REASON_AUTH_FAIL);
        if (i + 1 < NEW_CREDENTIAL_FAILURES_BEFORE_GIVING_UP) {
            fire(reconnect_timer);
        }
    }
    hal_wifi_attempt_take(attempt);
    scan_err = hal_wifi_scan(aps, &count);
    snprintf(d, n, "scan=%s, retry armed after the scan=%d", esp_err_to_name(scan_err), reconnect_timer->armed);
    check("positive control: after a scan the HAL does not restart a connection whose retries ended", scan_err == ESP_OK && !reconnect_timer->armed, d);

    start();
    connect("password1");
    hal_wifi_attempt_cancel(attempt);
    join("Home", IP_A);
    host_wifi_hook_call = "esp_wifi_scan_start";
    host_wifi_hook = drop_link_during_scan;
    scan_err = hal_wifi_scan(aps, &count);
    snprintf(d, n, "state during the scan=%s, retry armed during it=%d, retry armed after it=%d",
    (const char *[]) {
        "DISCONNECTED", "CONNECTING", "CONNECTED", "ERROR"
    }[state_in_scan], retry_armed_in_scan,
    reconnect_timer->armed);
    check("positive control: a disconnect during a scan reports CONNECTING without a retry, and the scan end starts the connect",
          scan_err == ESP_OK && state_in_scan == HAL_WIFI_STATE_CONNECTING && !retry_armed_in_scan &&
          reconnect_timer->armed, d);
}

int main(void)
{
    char d[200];
    char psk[65] = {0};

    hal_wifi_register_callback(count_state);
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
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    int disc = wifi_disconnect_calls;
    bool fired = fire(got_ip_timer);
    snprintf(d, sizeof d, "IP timer fired=%d disconnects=%d", fired, wifi_disconnect_calls - disc);
    check("an association without an IP address in time is disconnected", fired && wifi_disconnect_calls - disc == 1, d);

    start();
    connect("password1");
    deliver(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL, 0);
    host_wifi_link_up("Home", IP_A);
    got_ip(IP_A);
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
    host_wifi_link_up("Home", IP_A);
    queue_got_ip(IP_A);
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

    start();
    wifi_stop_result = ESP_FAIL;
    esp_err_t stop_failed = hal_network_deinit();
    esp_err_t init_while_pending = hal_network_init();
    wifi_stop_result = ESP_OK;
    esp_err_t retried = hal_network_deinit();
    esp_err_t init_after = hal_network_init();
    snprintf(d, sizeof d, "deinit while WiFi does not stop=%s, init meanwhile=%s, deinit retry=%s, init after it=%s",
             esp_err_to_name(stop_failed), esp_err_to_name(init_while_pending), esp_err_to_name(retried),
             esp_err_to_name(init_after));
    check("hal_network_deinit reports a WiFi driver that does not stop, and its retry finishes the teardown",
          stop_failed == ESP_FAIL && init_while_pending == ESP_ERR_INVALID_STATE && retried == ESP_OK &&
          init_after == ESP_OK, d);

    wifi_deinit_result = ESP_ERR_NO_MEM;
    esp_err_t deinit_failed = hal_network_deinit();
    init_while_pending = hal_network_init();
    wifi_deinit_result = ESP_OK;
    retried = hal_network_deinit();
    init_after = hal_network_init();
    snprintf(d, sizeof d, "deinit while the WiFi driver does not deinit=%s, init meanwhile=%s, deinit retry=%s, init "
             "after it=%s", esp_err_to_name(deinit_failed), esp_err_to_name(init_while_pending),
             esp_err_to_name(retried), esp_err_to_name(init_after));
    check("hal_network_deinit reports a WiFi driver that does not deinit, and its retry finishes the teardown",
          deinit_failed == ESP_ERR_NO_MEM && init_while_pending == ESP_ERR_INVALID_STATE && retried == ESP_OK &&
          init_after == ESP_OK, d);

    stale_ip_checks(d, sizeof d);
    deadline_checks(d, sizeof d);
    outcome_checks(d, sizeof d);
    lifecycle_checks(d, sizeof d);
    gate_checks(d, sizeof d);

    start();
    hal_network_deinit();
    critical_calls += wifi_calls_in_critical;
    snprintf(d, sizeof d, "driver calls made inside a critical section=%d", critical_calls);
    check("positive control: no Wi-Fi driver call runs inside a critical section", critical_calls == 0, d);
    return check_summary();
}
