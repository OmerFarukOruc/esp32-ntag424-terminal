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

#define SCAN_START_DELAY_MS     100
/* A connect attempt in progress makes the scan fail, and the attempt ends by itself, so the scan
 * retries. The count and the delay are chosen, not from a source. The longest wait is 3.5 s. */
#define SCAN_BUSY_ATTEMPTS      8
#define SCAN_BUSY_RETRY_MS      500

static struct {
    bool initialized;
    bool wifi_initialized;
    esp_netif_t *sta_netif;
    esp_timer_handle_t reconnect_timer;
    esp_event_handler_instance_t wifi_handler;
    esp_event_handler_instance_t ip_handler;
    /* The three flags are atomic because event handlers, timers and callers of the HAL set and read them. */
    atomic_bool auto_reconnect;
    /* True while hal_wifi_scan() runs, so the HAL does not connect or retry and break the scan. */
    atomic_bool scan_mode;
    /* True from hal_wifi_connect() until the first IP address with those credentials. */
    atomic_bool credentials_unproven;
    /* The fields below change under s_lock. */
    hal_wifi_state_t state;
    hal_wifi_event_cb_t event_callback;
    int retry_count;
    int backoff_ms;
} s_network;

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Callbacks run outside the lock, and only when the state changes. */
static void set_state(hal_wifi_state_t new_state)
{
    portENTER_CRITICAL(&s_lock);
    bool changed = (s_network.state != new_state);
    hal_wifi_event_cb_t callback = s_network.event_callback;
    s_network.state = new_state;
    portEXIT_CRITICAL(&s_lock);

    if (changed && callback) {
        callback(new_state);
    }
}

static void retry_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    s_network.retry_count = 0;
    s_network.backoff_ms = WIFI_BACKOFF_MIN_MS;
    portEXIT_CRITICAL(&s_lock);
}

/* Counts a failed attempt and returns the count, with the delay before the next attempt. */
static int retry_next(int *delay_ms)
{
    portENTER_CRITICAL(&s_lock);
    int attempt = ++s_network.retry_count;
    *delay_ms = s_network.backoff_ms;
    int doubled_ms = s_network.backoff_ms * 2;
    s_network.backoff_ms = (doubled_ms > WIFI_BACKOFF_MAX_MS) ? WIFI_BACKOFF_MAX_MS : doubled_ms;
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

static void reconnect_timer_callback(void *arg)
{
    (void)arg;
    if (!s_network.auto_reconnect || s_network.scan_mode) {
        return;
    }

    ESP_LOGI(TAG, "Attempting WiFi reconnect...");
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reconnect failed to start: %s", esp_err_to_name(err));
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        /* Events arrive in order, so this enables retries only after the disconnect and stop
         * events of a stopped connection are handled. */
        s_network.auto_reconnect = true;
        retry_reset();
        if (s_network.scan_mode) {
            ESP_LOGI(TAG, "Scan mode active, skipping auto-connect");
            return;
        }
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Connect failed to start: %s", esp_err_to_name(err));
        }
    } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event = event_data;

        ESP_LOGW(TAG, "WiFi disconnected, reason: %d", event->reason);

        if (!s_network.auto_reconnect) {
            return;
        }
        if (s_network.scan_mode) {
            /* The scan owns the radio. Report CONNECTING; hal_wifi_scan() restarts the retries when it ends. */
            if (hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED) {
                set_state(HAL_WIFI_STATE_CONNECTING);
            }
            return;
        }

        int delay_ms;
        int attempt = retry_next(&delay_ms);
        if (s_network.credentials_unproven && attempt >= WIFI_NEW_CREDENTIALS_MAX_ATTEMPTS &&
                reason_is_credential_failure(event->reason)) {
            ESP_LOGW(TAG, "New credentials failed %d times (reason %d), giving up", attempt, event->reason);
            s_network.auto_reconnect = false;
            set_state(HAL_WIFI_STATE_DISCONNECTED);
            return;
        }
        if (attempt >= WIFI_ATTEMPTS_BEFORE_DISCONNECTED) {
            ESP_LOGW(TAG, "Many failures (%d), reporting disconnected", attempt);
            set_state(HAL_WIFI_STATE_DISCONNECTED);
        } else {
            set_state(HAL_WIFI_STATE_CONNECTING);
        }

        ESP_LOGI(TAG, "Reconnecting in %d ms (attempt %d)", delay_ms, attempt);
        esp_timer_stop(s_network.reconnect_timer);
        esp_err_t err = esp_timer_start_once(s_network.reconnect_timer, (uint64_t)delay_ms * 1000);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Reconnect timer failed to start: %s", esp_err_to_name(err));
        }
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

        retry_reset();
        s_network.credentials_unproven = false;
        esp_timer_stop(s_network.reconnect_timer);
        set_state(HAL_WIFI_STATE_CONNECTED);
    } else if (event_id == IP_EVENT_STA_LOST_IP) {
        ESP_LOGW(TAG, "Lost IP address");
        if (hal_wifi_get_state() == HAL_WIFI_STATE_CONNECTED) {
            set_state(HAL_WIFI_STATE_CONNECTING);
        }
    }
}

static void network_teardown(void)
{
    if (s_network.wifi_handler) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_network.wifi_handler);
    }
    if (s_network.ip_handler) {
        esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, s_network.ip_handler);
    }
    if (s_network.reconnect_timer) {
        esp_timer_stop(s_network.reconnect_timer);
        esp_timer_delete(s_network.reconnect_timer);
    }
    if (s_network.wifi_initialized) {
        esp_wifi_stop();
        esp_wifi_deinit();
    }
    if (s_network.sta_netif) {
        esp_netif_destroy_default_wifi(s_network.sta_netif);
    }

    hal_wifi_event_cb_t callback = s_network.event_callback;
    memset(&s_network, 0, sizeof(s_network));
    s_network.event_callback = callback;
}

esp_err_t hal_network_init(void)
{
    if (s_network.initialized) {
        return ESP_OK;
    }

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

    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler,
                                                          NULL, &s_network.wifi_handler),
                      fail, TAG, "WiFi event handler register failed");
    ESP_GOTO_ON_ERROR(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event_handler,
                                                          NULL, &s_network.ip_handler),
                      fail, TAG, "IP event handler register failed");

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
        retry_reset();
        set_state(HAL_WIFI_STATE_CONNECTING);

        ret = esp_wifi_start();
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Saved WiFi '%s' found, auto-connecting on boot", saved_ssid);
        } else {
            ESP_LOGE(TAG, "Failed to start WiFi for auto-connect: %s", esp_err_to_name(ret));
            set_state(HAL_WIFI_STATE_ERROR);
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
    if (!s_network.initialized) {
        return ESP_OK;
    }

    network_teardown();
    ESP_LOGI(TAG, "Deinitialized");
    return ESP_OK;
}

esp_err_t hal_wifi_connect(const hal_wifi_credentials_t *credentials)
{
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
    memcpy(wifi_config.sta.ssid, credentials->ssid, ssid_len);
    memcpy(wifi_config.sta.password, credentials->password, password_len);
    /* The driver defaults to WPA2 for a password of 8 or more characters. Setting it also makes the
     * choice explicit: WEP and WPA-only access points are not joined. */
    wifi_config.sta.threshold.authmode = password_len > 0 ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    /* Stop the old connection first, and keep its disconnect event from starting a retry.
     * The STA_START handler turns retries on again. */
    s_network.auto_reconnect = false;
    esp_timer_stop(s_network.reconnect_timer);

    /* A driver that is not started has nothing to disconnect. */
    esp_err_t ret = esp_wifi_disconnect();
    ESP_GOTO_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_WIFI_NOT_STARTED, ret, fail, TAG, "WiFi disconnect failed");
    ESP_GOTO_ON_ERROR(esp_wifi_stop(), fail, TAG, "WiFi stop failed");

    ESP_GOTO_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), fail, TAG, "WiFi set config failed");

    retry_reset();
    s_network.credentials_unproven = true;
    set_state(HAL_WIFI_STATE_CONNECTING);
    ESP_GOTO_ON_ERROR(esp_wifi_start(), fail, TAG, "WiFi start failed");

    ESP_LOGI(TAG, "Connecting to SSID: %s", credentials->ssid);
    return ESP_OK;

fail:
    s_network.auto_reconnect = false;
    set_state(HAL_WIFI_STATE_DISCONNECTED);
    return ret;
}

hal_wifi_state_t hal_wifi_get_state(void)
{
    return s_network.state;
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
    ESP_RETURN_ON_FALSE(!atomic_exchange(&s_network.scan_mode, true), ESP_ERR_INVALID_STATE, TAG,
                        "A scan is already running");

    esp_err_t ret = scan_joinable(aps, count);
    if (ret != ESP_OK) {
        *count = 0;
    }

    /* The scan muted the retry loop and the connect on start, so the station is not left idle. */
    atomic_store(&s_network.scan_mode, false);
    char saved_ssid[33];
    /* Ask the driver, not the HAL state: a disconnect handled during the scan may not have
     * updated the state yet. */
    wifi_ap_record_t ap_info;
    bool associated = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
    if (s_network.auto_reconnect && !associated &&
            read_saved_ssid(saved_ssid) == ESP_OK && saved_ssid[0] != '\0') {
        esp_timer_stop(s_network.reconnect_timer);
        esp_err_t timer_err = esp_timer_start_once(s_network.reconnect_timer, 0);
        if (timer_err != ESP_OK) {
            ESP_LOGE(TAG, "Reconnect timer failed to start: %s", esp_err_to_name(timer_err));
        }
    }
    return ret;
}
