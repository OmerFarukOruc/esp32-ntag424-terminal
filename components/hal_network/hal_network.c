/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_network.c
 * @brief Network HAL - WiFi STA implementation
 */

#include "hal_network.h"
#include <ctype.h>
#include <stdatomic.h>
#include "esp_check.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "hal_network";

#define WIFI_ATTEMPTS_BEFORE_DISCONNECTED 10
#define WIFI_BACKOFF_MIN_MS     1000
#define WIFI_BACKOFF_MAX_MS     30000
#define WIFI_NEW_CREDENTIALS_MAX_ATTEMPTS 3
/* Chosen, not from a source: DHCP normally answers within a few seconds. The driver does not end an
 * association that gets no address, so the HAL does. */
#define WIFI_GOT_IP_TIMEOUT_MS  30000

#define WIFI_ATTEMPT_DEADLINE_US ((int64_t)HAL_WIFI_ATTEMPT_DEADLINE_MS * 1000)

#define SCAN_START_DELAY_MS     100
/* A connect attempt in progress makes the scan fail, and the attempt ends by itself, so the scan
 * retries. The count and the delay are chosen, not from a source. The longest wait is 3.5 s. */
#define SCAN_BUSY_ATTEMPTS      8
#define SCAN_BUSY_RETRY_MS      500

typedef enum {
    OP_NONE,
    OP_SCAN,
    OP_DRIVER,
    OP_TEARDOWN,
} net_op_t;

static struct {
    bool initialized;
    bool wifi_initialized;
    esp_netif_t *sta_netif;
    esp_timer_handle_t reconnect_timer;
    esp_timer_handle_t got_ip_timer;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    atomic_bool auto_reconnect;
    /* True from hal_wifi_connect() until the first IP address with those credentials. */
    atomic_bool credentials_unproven;
    atomic_uint connect_generation;
    atomic_uint reconnect_timer_generation;
    atomic_uint got_ip_timer_generation;
    /* The fields below change under s_lock. */
    hal_wifi_state_t state;
    hal_wifi_event_cb_t event_callback;
    esp_event_handler_instance_t got_ip_timeout_handler;
    int retry_count;
    int credential_failures;
    int backoff_ms;
    net_op_t op;
    bool retry_pending;
    hal_wifi_attempt_id_t last_attempt_id;
    hal_wifi_attempt_id_t attempt_id;
    hal_wifi_attempt_status_t attempt_status;
    int64_t attempt_deadline_us;
} s_network;

ESP_EVENT_DEFINE_BASE(HAL_NETWORK_EVENT);
#define HAL_NETWORK_EVENT_GOT_IP_TIMEOUT 0

/* Chosen, not from a source: a full event queue empties within a second. */
#define GOT_IP_TIMEOUT_REPOST_MS 1000

/* Chosen, not from a source: a callback that runs when a timer stops returns within a few milliseconds. */
#define TIMER_STOP_WAIT_MS 100

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static unsigned current_generation(void)
{
    return atomic_load(&s_network.connect_generation);
}

static void set_state(unsigned generation, hal_wifi_state_t new_state)
{
    portENTER_CRITICAL(&s_lock);
    bool current = generation == current_generation();
    bool changed = current && s_network.state != new_state;
    hal_wifi_event_cb_t callback = s_network.event_callback;
    if (current) {
        s_network.state = new_state;
    }
    portEXIT_CRITICAL(&s_lock);

    if (changed && callback) {
        callback(new_state);
    }
}

static bool op_claim_locked(net_op_t op)
{
    if (s_network.op != OP_NONE || (!s_network.initialized && op != OP_TEARDOWN)) {
        return false;
    }
    s_network.op = op;
    return true;
}

static bool op_claim(net_op_t op)
{
    portENTER_CRITICAL(&s_lock);
    bool claimed = op_claim_locked(op);
    portEXIT_CRITICAL(&s_lock);
    return claimed;
}

static bool op_claim_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    bool claimed = s_network.attempt_status != HAL_WIFI_ATTEMPT_PENDING && op_claim_locked(OP_SCAN);
    portEXIT_CRITICAL(&s_lock);
    return claimed;
}

static bool op_claim_or_defer(unsigned generation)
{
    portENTER_CRITICAL(&s_lock);
    bool claimed = op_claim_locked(OP_DRIVER);
    if (!claimed && s_network.initialized && generation == current_generation()) {
        s_network.retry_pending = true;
    }
    portEXIT_CRITICAL(&s_lock);
    return claimed;
}

static bool op_is_scan(void)
{
    portENTER_CRITICAL(&s_lock);
    bool scanning = s_network.op == OP_SCAN;
    portEXIT_CRITICAL(&s_lock);
    return scanning;
}

static void op_abandon(void)
{
    portENTER_CRITICAL(&s_lock);
    s_network.op = OP_NONE;
    s_network.retry_pending = false;
    portEXIT_CRITICAL(&s_lock);
}

static bool op_end(void)
{
    portENTER_CRITICAL(&s_lock);
    s_network.op = OP_NONE;
    bool pending = s_network.retry_pending;
    s_network.retry_pending = false;
    portEXIT_CRITICAL(&s_lock);
    return pending;
}

static void resume_connection(void);

static void op_release(void)
{
    if (op_end()) {
        resume_connection();
    }
}

static void attempt_expire_locked(int64_t now_us)
{
    if (s_network.attempt_status == HAL_WIFI_ATTEMPT_PENDING && now_us >= s_network.attempt_deadline_us) {
        s_network.attempt_status = HAL_WIFI_ATTEMPT_FAILED;
    }
}

static bool attempt_finish_locked(unsigned generation, hal_wifi_attempt_status_t outcome, int64_t now_us)
{
    if (generation != current_generation()) {
        return false;
    }
    attempt_expire_locked(now_us);
    if (s_network.attempt_status == HAL_WIFI_ATTEMPT_PENDING) {
        s_network.attempt_status = outcome;
    }
    return true;
}

static bool attempt_finish(unsigned generation, hal_wifi_attempt_status_t outcome)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool current = attempt_finish_locked(generation, outcome, now_us);
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static bool link_up(unsigned generation)
{
    int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    bool current = attempt_finish_locked(generation, HAL_WIFI_ATTEMPT_SUCCEEDED, now_us);
    if (current) {
        s_network.retry_count = 0;
        s_network.credential_failures = 0;
        s_network.backoff_ms = WIFI_BACKOFF_MIN_MS;
        atomic_store(&s_network.credentials_unproven, false);
    }
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static hal_wifi_attempt_id_t attempt_begin(int64_t accepted_us, unsigned *generation)
{
    portENTER_CRITICAL(&s_lock);
    *generation = current_generation() + 1;
    atomic_store(&s_network.connect_generation, *generation);
    hal_wifi_attempt_id_t id = ++s_network.last_attempt_id;
    if (id == 0) {
        id = ++s_network.last_attempt_id;
    }
    s_network.attempt_id = id;
    s_network.attempt_status = HAL_WIFI_ATTEMPT_PENDING;
    s_network.attempt_deadline_us = accepted_us + WIFI_ATTEMPT_DEADLINE_US;
    portEXIT_CRITICAL(&s_lock);
    return id;
}

static void attempt_retire(void)
{
    portENTER_CRITICAL(&s_lock);
    s_network.attempt_id = 0;
    s_network.attempt_status = HAL_WIFI_ATTEMPT_NONE;
    portEXIT_CRITICAL(&s_lock);
}

static bool retry_reset(unsigned generation)
{
    portENTER_CRITICAL(&s_lock);
    bool current = generation == current_generation();
    if (current) {
        s_network.retry_count = 0;
        s_network.credential_failures = 0;
        s_network.backoff_ms = WIFI_BACKOFF_MIN_MS;
    }
    portEXIT_CRITICAL(&s_lock);
    return current;
}

static int retry_next(unsigned generation, bool credential_failure, int *delay_ms, int *credential_failures)
{
    portENTER_CRITICAL(&s_lock);
    int attempt = 0;
    if (generation == current_generation()) {
        attempt = ++s_network.retry_count;
        if (credential_failure) {
            s_network.credential_failures++;
        }
        *credential_failures = s_network.credential_failures;
        *delay_ms = s_network.backoff_ms;
        int doubled_ms = s_network.backoff_ms * 2;
        s_network.backoff_ms = (doubled_ms > WIFI_BACKOFF_MAX_MS) ? WIFI_BACKOFF_MAX_MS : doubled_ms;
    }
    portEXIT_CRITICAL(&s_lock);
    return attempt;
}

/* Reasons that mean wrong credentials or no matching access point. */
static bool reason_is_credential_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_MIC_FAILURE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_CONNECTION_FAIL:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        return true;
    default:
        return false;
    }
}

/* Copies the saved SSID, empty when none is saved. The config holds the password, so it is wiped. */
static esp_err_t read_saved_ssid(char ssid[33])
{
    wifi_config_t saved = {0};
    esp_err_t err = esp_wifi_get_config(WIFI_IF_STA, &saved);
    if (err == ESP_OK) {
        memcpy(ssid, saved.sta.ssid, sizeof(saved.sta.ssid));
        ssid[sizeof(saved.sta.ssid)] = '\0';
    }
    explicit_bzero(&saved, sizeof(saved));
    return err;
}

static bool all_hex(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (!isxdigit((unsigned char)text[i])) {
            return false;
        }
    }
    return true;
}

static esp_err_t arm_timer(esp_timer_handle_t timer, atomic_uint *timer_generation, unsigned generation,
                           uint32_t delay_ms)
{
    portENTER_CRITICAL(&s_lock);
    bool current = generation == current_generation();
    if (current) {
        atomic_store(timer_generation, generation);
    }
    portEXIT_CRITICAL(&s_lock);
    if (!current) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_timer_stop(timer);
    return esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
}

static void end_attempt_on_timer_error(unsigned generation, esp_err_t err, const char *name)
{
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        return;
    }
    ESP_LOGE(TAG, "%s timer failed to start: %s", name, esp_err_to_name(err));
    attempt_finish(generation, HAL_WIFI_ATTEMPT_FAILED);
}

static void arm_reconnect_timer(unsigned generation, uint32_t delay_ms)
{
    esp_err_t err = arm_timer(s_network.reconnect_timer, &s_network.reconnect_timer_generation, generation,
                              delay_ms);
    end_attempt_on_timer_error(generation, err, "Reconnect");
}

static void arm_got_ip_timer(unsigned generation, uint32_t delay_ms)
{
    esp_err_t err = arm_timer(s_network.got_ip_timer, &s_network.got_ip_timer_generation, generation, delay_ms);
    end_attempt_on_timer_error(generation, err, "IP address");
}

/* Counts a failed attempt, reports the state it leads to and starts the timer for the next one. */
static void retry_after_failure(unsigned generation, uint8_t reason)
{
    int delay_ms;
    int credential_failures;
    int attempt = retry_next(generation, reason_is_credential_failure(reason), &delay_ms, &credential_failures);
    if (attempt == 0) {
        return;
    }
    if (s_network.credentials_unproven && credential_failures >= WIFI_NEW_CREDENTIALS_MAX_ATTEMPTS) {
        ESP_LOGW(TAG, "New credentials failed %d times (reason %d), giving up", credential_failures, reason);
        s_network.auto_reconnect = false;
        attempt_finish(generation, HAL_WIFI_ATTEMPT_FAILED);
        set_state(generation, HAL_WIFI_STATE_DISCONNECTED);
        return;
    }
    if (attempt >= WIFI_ATTEMPTS_BEFORE_DISCONNECTED) {
        ESP_LOGW(TAG, "Many failures (%d), reporting disconnected", attempt);
        attempt_finish(generation, HAL_WIFI_ATTEMPT_FAILED);
        set_state(generation, HAL_WIFI_STATE_DISCONNECTED);
    } else {
        set_state(generation, HAL_WIFI_STATE_CONNECTING);
    }

    ESP_LOGI(TAG, "Reconnecting in %d ms (attempt %d)", delay_ms, attempt);
    arm_reconnect_timer(generation, delay_ms);
}

/* A connect call that fails at once raises no disconnect event, so it counts as a failed attempt here. */
static void start_connect(unsigned generation)
{
    if (!op_claim_or_defer(generation)) {
        ESP_LOGI(TAG, "Connect deferred: another WiFi operation runs");
        return;
    }
    bool current = generation == current_generation();
    esp_err_t err = current ? esp_wifi_connect() : ESP_OK;
    op_release();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Connect failed to start: %s", esp_err_to_name(err));
        retry_after_failure(generation, WIFI_REASON_UNSPECIFIED);
    }
}

static void reconnect_timer_callback(void *arg)
{
    (void)arg;
    unsigned generation = atomic_load(&s_network.reconnect_timer_generation);
    if (!s_network.auto_reconnect) {
        return;
    }

    ESP_LOGI(TAG, "Attempting WiFi reconnect...");
    start_connect(generation);
}

/* Runs on the event loop task, after any got-IP event queued before it. The disconnect event that
 * follows counts the attempt as failed. */
static void got_ip_timeout_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    unsigned generation = *(const unsigned *)event_data;
    if (hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED) {
        return;
    }
    if (!op_claim(OP_DRIVER)) {
        arm_got_ip_timer(generation, GOT_IP_TIMEOUT_REPOST_MS);
        return;
    }
    bool current = generation == current_generation();
    esp_err_t err = ESP_OK;
    if (current) {
        ESP_LOGW(TAG, "No IP address %d s after joining, disconnecting", WIFI_GOT_IP_TIMEOUT_MS / 1000);
        err = esp_wifi_disconnect();
    } else {
        ESP_LOGI(TAG, "IP address timeout of an earlier connection ignored");
    }
    op_release();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Disconnect failed: %s", esp_err_to_name(err));
        retry_after_failure(generation, WIFI_REASON_UNSPECIFIED);
    }
}

static void got_ip_timer_callback(void *arg)
{
    (void)arg;
    unsigned generation = atomic_load(&s_network.got_ip_timer_generation);
    esp_err_t err = esp_event_post(HAL_NETWORK_EVENT, HAL_NETWORK_EVENT_GOT_IP_TIMEOUT, &generation,
                                   sizeof(generation), 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IP address timeout not queued: %s", esp_err_to_name(err));
        arm_got_ip_timer(generation, GOT_IP_TIMEOUT_REPOST_MS);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    unsigned generation = current_generation();
    if (event_id == WIFI_EVENT_STA_START) {
        /* Events arrive in order, so this enables retries only after the disconnect and stop
         * events of a stopped connection are handled. */
        if (!retry_reset(generation)) {
            return;
        }
        s_network.auto_reconnect = true;
        start_connect(generation);
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        arm_got_ip_timer(generation, WIFI_GOT_IP_TIMEOUT_MS);
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;

        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);
        esp_timer_stop(s_network.got_ip_timer);

        if (!s_network.auto_reconnect) {
            return;
        }
        if (op_is_scan()) {
            /* The scan owns the radio. Report CONNECTING; hal_wifi_scan() restarts the retries when it ends. */
            if (hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED) {
                set_state(generation, HAL_WIFI_STATE_CONNECTING);
            }
            return;
        }

        retry_after_failure(generation, event->reason);
    }
}

/* esp_netif_get_ip_info() returns the copy kept for an interface that is down, so the address alone does not
 * show a current link. */
static bool link_is_live(const ip_event_got_ip_t *event)
{
    wifi_ap_record_t ap_info;
    char configured_ssid[33];
    esp_netif_ip_info_t ip_info;
    return esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && read_saved_ssid(configured_ssid) == ESP_OK &&
           strncmp((const char *)ap_info.ssid, configured_ssid, sizeof(configured_ssid)) == 0 &&
           esp_netif_get_ip_info(s_network.sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0 &&
           ip_info.ip.addr == event->ip_info.ip.addr;
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    unsigned generation = current_generation();
    if (event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        if (!link_is_live(event)) {
            ESP_LOGW(TAG, "Got IP event ignored: the driver is not associated with the configured network at " IPSTR,
                     IP2STR(&event->ip_info.ip));
            return;
        }
        if (!link_up(generation)) {
            ESP_LOGW(TAG, "Got IP event ignored: the connection it belongs to was replaced");
            return;
        }
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        esp_timer_stop(s_network.reconnect_timer);
        esp_timer_stop(s_network.got_ip_timer);
        set_state(generation, HAL_WIFI_STATE_CONNECTED);
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address");
        if (hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED) {
            set_state(generation, HAL_WIFI_STATE_CONNECTING);
        }
        arm_got_ip_timer(generation, WIFI_GOT_IP_TIMEOUT_MS);
    }
}

static void resume_connection(void)
{
    char saved_ssid[33];
    wifi_ap_record_t ap_info;
    bool associated = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    if (s_network.auto_reconnect && !associated &&
            read_saved_ssid(saved_ssid) == ESP_OK && saved_ssid[0] != '\0') {
        arm_reconnect_timer(current_generation(), 0);
    }
}

static esp_err_t delete_timer(esp_timer_handle_t *timer)
{
    if (*timer == NULL) {
        return ESP_OK;
    }
    esp_timer_stop_blocking(*timer, pdMS_TO_TICKS(TIMER_STOP_WAIT_MS));
    ESP_RETURN_ON_ERROR(esp_timer_delete(*timer), TAG, "Timer delete failed");
    *timer = NULL;
    return ESP_OK;
}

static esp_err_t network_teardown(void)
{
    s_network.initialized = false;
    if (s_network.wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_network.wifi_handler);
        s_network.wifi_handler = NULL;
    }
    if (s_network.ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_network.ip_handler);
        s_network.ip_handler = NULL;
    }
    if (s_network.got_ip_timeout_handler) {
        esp_event_handler_instance_unregister(HAL_NETWORK_EVENT, HAL_NETWORK_EVENT_GOT_IP_TIMEOUT,
                                              s_network.got_ip_timeout_handler);
        s_network.got_ip_timeout_handler = NULL;
    }
    ESP_RETURN_ON_ERROR(delete_timer(&s_network.reconnect_timer), TAG, "Reconnect timer delete failed");
    ESP_RETURN_ON_ERROR(delete_timer(&s_network.got_ip_timer), TAG, "IP address timer delete failed");
    if (s_network.wifi_initialized) {
        ESP_RETURN_ON_ERROR(esp_wifi_stop(), TAG, "WiFi stop failed");
        ESP_RETURN_ON_ERROR(esp_wifi_deinit(), TAG, "WiFi deinit failed");
        s_network.wifi_initialized = false;
    }
    if (s_network.sta_netif) {
        esp_netif_destroy_default_wifi(s_network.sta_netif);
    }

    hal_wifi_event_cb_t callback = s_network.event_callback;
    hal_wifi_attempt_id_t last_attempt_id = s_network.last_attempt_id;
    unsigned generation = current_generation();
    memset(&s_network, 0, sizeof(s_network));
    s_network.event_callback = callback;
    s_network.last_attempt_id = last_attempt_id;
    atomic_store(&s_network.connect_generation, generation);
    return ESP_OK;
}

esp_err_t hal_network_init(void)
{
    if (s_network.initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!s_network.wifi_initialized, ESP_ERR_INVALID_STATE, TAG,
                        "a failed deinit waits for its retry");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");

    /* The default event loop may exist already, and it belongs to the application. */
    esp_err_t ret = esp_event_loop_create_default();
    ESP_RETURN_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, ret, TAG, "event loop create failed");

    s_network.sta_netif = esp_netif_create_default_wifi_sta();
    ESP_GOTO_ON_FALSE(s_network.sta_netif, ESP_FAIL, fail, TAG, "station netif create failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_GOTO_ON_ERROR(esp_wifi_init(&cfg), fail, TAG, "WiFi init failed");
    s_network.wifi_initialized = true;
    ESP_GOTO_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), fail, TAG, "WiFi set mode failed");

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_timer_callback,
        .name = "wifi_reconnect",
    };
    ESP_GOTO_ON_ERROR(esp_timer_create(&timer_args, &s_network.reconnect_timer), fail, TAG,
                      "reconnect timer create failed");
    const esp_timer_create_args_t got_ip_timer_args = {
        .callback = got_ip_timer_callback,
        .name = "wifi_got_ip",
    };
    ESP_GOTO_ON_ERROR(esp_timer_create(&got_ip_timer_args, &s_network.got_ip_timer), fail, TAG,
                      "IP address timer create failed");

    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                          NULL, &s_network.wifi_handler),
                      fail, TAG, "WiFi event handler register failed");
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler,
                                                          NULL, &s_network.ip_handler),
                      fail, TAG, "IP event handler register failed");
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(HAL_NETWORK_EVENT, HAL_NETWORK_EVENT_GOT_IP_TIMEOUT,
                                                          got_ip_timeout_handler, NULL,
                                                          &s_network.got_ip_timeout_handler),
                      fail, TAG, "IP address timeout handler register failed");

    s_network.initialized = true;

    char saved_ssid[33];
    ret = read_saved_ssid(saved_ssid);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read saved WiFi config: %s", esp_err_to_name(ret));
    } else if (saved_ssid[0] == '\0') {
        ESP_LOGI(TAG, "No saved WiFi credentials found");
    } else {
        /* The saved credentials need an explicit start; STA_START then connects. */
        s_network.auto_reconnect = true;
        retry_reset(current_generation());
        set_state(current_generation(), HAL_WIFI_STATE_CONNECTING);

        ret = esp_wifi_start();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Saved WiFi '%s' found, auto-connecting on boot", saved_ssid);
        } else {
            ESP_LOGE(TAG, "Failed to start WiFi for auto-connect: %s", esp_err_to_name(ret));
            set_state(current_generation(), HAL_WIFI_STATE_ERROR);
        }
    }

    ESP_LOGI(TAG, "Initialized");
    return ESP_OK;

fail:
    network_teardown();
    return ret;
}

esp_err_t hal_network_deinit(void)
{
    if (!s_network.initialized && !s_network.wifi_initialized) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(op_claim(OP_TEARDOWN), ESP_ERR_NOT_FINISHED, TAG, "Deinit refused: WiFi work is running");

    esp_err_t err = network_teardown();
    if (err != ESP_OK) {
        op_abandon();
        ESP_LOGE(TAG, "Deinit failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

esp_err_t hal_wifi_connect(const hal_wifi_credentials_t *credentials, hal_wifi_attempt_id_t *attempt_id)
{
    ESP_RETURN_ON_FALSE(attempt_id != NULL, ESP_ERR_INVALID_ARG, TAG, "Attempt id output is NULL");
    *attempt_id = 0;
    ESP_RETURN_ON_FALSE(s_network.initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(credentials != NULL, ESP_ERR_INVALID_ARG, TAG, "Credentials are NULL");

    /* The driver fields hold 32 and 64 bytes with no terminator, so copy by length. */
    wifi_config_t wifi_config = {0};
    size_t ssid_len = strnlen(credentials->ssid, sizeof(credentials->ssid));
    size_t password_len = strnlen(credentials->password, sizeof(credentials->password));
    ESP_RETURN_ON_FALSE(ssid_len > 0 && ssid_len <= sizeof(wifi_config.sta.ssid), ESP_ERR_INVALID_ARG, TAG,
                        "SSID must be 1 to 32 bytes");
    /* WPA2 needs 8 to 63 characters or a 64-digit hex key; check before the old connection stops. */
    ESP_RETURN_ON_FALSE(password_len == 0 || (password_len >= 8 && password_len <= sizeof(wifi_config.sta.password)),
                        ESP_ERR_INVALID_ARG, TAG, "Password must be empty or 8 to 64 bytes");
    ESP_RETURN_ON_FALSE(password_len < sizeof(wifi_config.sta.password) || all_hex(credentials->password, password_len),
                        ESP_ERR_INVALID_ARG, TAG, "A 64-byte password must be 64 hex digits");
    memcpy(wifi_config.sta.ssid, credentials->ssid, ssid_len);
    memcpy(wifi_config.sta.password, credentials->password, password_len);
    /* The driver defaults to WPA2 for a password of 8 or more characters. Setting it also makes the
     * choice explicit: WEP and WPA-only access points are not joined. */
    wifi_config.sta.threshold.authmode = password_len > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    if (!op_claim(OP_DRIVER)) {
        explicit_bzero(&wifi_config, sizeof(wifi_config));
        ESP_LOGW(TAG, "Connect refused: WiFi work is running");
        return ESP_ERR_NOT_FINISHED;
    }
    int64_t accepted_us = esp_timer_get_time();

    /* Stop the old connection first, and keep its disconnect event from starting a retry.
     * The STA_START handler turns retries on again. */
    s_network.auto_reconnect = false;
    esp_timer_stop_blocking(s_network.reconnect_timer, pdMS_TO_TICKS(TIMER_STOP_WAIT_MS));
    esp_timer_stop_blocking(s_network.got_ip_timer, pdMS_TO_TICKS(TIMER_STOP_WAIT_MS));
    unsigned generation;
    *attempt_id = attempt_begin(accepted_us, &generation);

    /* A driver that is not started has nothing to disconnect. */
    esp_err_t ret = esp_wifi_disconnect();
    ESP_GOTO_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_WIFI_NOT_STARTED, ret, fail, TAG, "WiFi disconnect failed");
    ESP_GOTO_ON_ERROR(esp_wifi_stop(), fail, TAG, "WiFi stop failed");

    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), fail, TAG, "WiFi set config failed");

    retry_reset(generation);
    s_network.credentials_unproven = true;
    set_state(generation, HAL_WIFI_STATE_CONNECTING);
    ESP_GOTO_ON_ERROR(esp_wifi_start(), fail, TAG, "WiFi start failed");

    ESP_LOGI(TAG, "Connecting to SSID: %s", credentials->ssid);
    explicit_bzero(&wifi_config, sizeof(wifi_config));
    op_release();
    return ESP_OK;

fail:
    explicit_bzero(&wifi_config, sizeof(wifi_config));
    s_network.auto_reconnect = false;
    attempt_retire();
    *attempt_id = 0;
    op_abandon();
    set_state(generation, HAL_WIFI_STATE_DISCONNECTED);
    return ret;
}

hal_wifi_attempt_status_t hal_wifi_attempt_take(hal_wifi_attempt_id_t attempt_id)
{
    int64_t now_us = esp_timer_get_time();
    hal_wifi_attempt_status_t status = HAL_WIFI_ATTEMPT_NONE;
    portENTER_CRITICAL(&s_lock);
    if (attempt_id == s_network.attempt_id) {
        attempt_expire_locked(now_us);
        status = s_network.attempt_status;
        if (status != HAL_WIFI_ATTEMPT_PENDING) {
            s_network.attempt_id = 0;
            s_network.attempt_status = HAL_WIFI_ATTEMPT_NONE;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return status;
}

void hal_wifi_attempt_cancel(hal_wifi_attempt_id_t attempt_id)
{
    portENTER_CRITICAL(&s_lock);
    if (attempt_id == s_network.attempt_id) {
        s_network.attempt_id = 0;
        s_network.attempt_status = HAL_WIFI_ATTEMPT_NONE;
    }
    portEXIT_CRITICAL(&s_lock);
}

hal_wifi_state_t hal_wifi_get_state(void)
{
    portENTER_CRITICAL(&s_lock);
    hal_wifi_state_t state = s_network.state;
    portEXIT_CRITICAL(&s_lock);
    return state;
}

void hal_wifi_register_callback(hal_wifi_event_cb_t callback)
{
    portENTER_CRITICAL(&s_lock);
    s_network.event_callback = callback;
    portEXIT_CRITICAL(&s_lock);
}

/* True for the networks hal_wifi_connect() can join. A mixed network reports its
 * stronger mode. */
static bool joinable_auth(wifi_auth_mode_t mode, hal_wifi_auth_t *auth)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        *auth = HAL_WIFI_AUTH_OPEN;
        return true;
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
        *auth = HAL_WIFI_AUTH_WPA2;
        return true;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
        *auth = HAL_WIFI_AUTH_WPA3;
        return true;
    default:
        return false;
    }
}

static esp_err_t scan_joinable(hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX], uint16_t *count)
{
    /* The driver may already run for a saved network, and it is not started when none is saved. */
    esp_err_t err = esp_wifi_start();
    ESP_RETURN_ON_FALSE(err == ESP_OK || err == ESP_ERR_WIFI_STATE, err, TAG, "WiFi start failed");
    vTaskDelay(pdMS_TO_TICKS(SCAN_START_DELAY_MS));

    const wifi_scan_config_t scan_config = {
        .show_hidden = true,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    err = esp_wifi_scan_start(&scan_config, true);
    for (int attempt = 1; err == ESP_ERR_WIFI_STATE && attempt < SCAN_BUSY_ATTEMPTS; attempt++) {
        ESP_LOGW(TAG, "Scan blocked by a connect attempt, retry %d of %d", attempt, SCAN_BUSY_ATTEMPTS - 1);
        vTaskDelay(pdMS_TO_TICKS(SCAN_BUSY_RETRY_MS));
        err = esp_wifi_scan_start(&scan_config, true);
    }
    ESP_RETURN_ON_ERROR(err, TAG, "WiFi scan failed");

    wifi_ap_record_t records[HAL_WIFI_SCAN_MAX];
    uint16_t found = HAL_WIFI_SCAN_MAX;
    ESP_RETURN_ON_ERROR(esp_wifi_scan_get_ap_records(&found, records), TAG, "WiFi scan records failed");

    *count = 0;
    for (uint16_t i = 0; i < found; i++) {
        hal_wifi_ap_t *ap = &aps[*count];
        if (!joinable_auth(records[i].authmode, &ap->auth)) {
            continue;
        }
        strlcpy(ap->ssid, (const char *)records[i].ssid, sizeof(ap->ssid));
        ap->rssi = records[i].rssi;
        ap->channel = records[i].primary;
        (*count)++;
    }
    return ESP_OK;
}

esp_err_t hal_wifi_scan(hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX], uint16_t *count)
{
    ESP_RETURN_ON_FALSE(s_network.initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(aps != NULL && count != NULL, ESP_ERR_INVALID_ARG, TAG, "Scan arguments are NULL");
    *count = 0;
    ESP_RETURN_ON_FALSE(op_claim_scan(), ESP_ERR_NOT_FINISHED, TAG,
                        "Scan refused: a scan, an attempt, or another WiFi operation is running");

    esp_err_t ret = scan_joinable(aps, count);
    if (ret != ESP_OK) {
        *count = 0;
    }

    /* The scan muted the retry loop and the connect on start, so the station is not left idle. */
    op_end();
    resume_connection();
    return ret;
}
