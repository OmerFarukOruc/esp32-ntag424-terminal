/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file app_state_priv.h
 * @brief Shared by the files of the application state machine; not a public interface
 */

#ifndef APP_STATE_PRIV_H
#define APP_STATE_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "hal_nfc.h"

typedef enum {
    APP_EVENT_INIT_COMPLETE,
    APP_EVENT_BUTTON_SHORT,
    APP_EVENT_BUTTON_HOLD_3S,
    APP_EVENT_BUTTON_HOLD_10S,
    APP_EVENT_WIFI_CONNECTING,
    APP_EVENT_WIFI_CONNECTED,
    APP_EVENT_WIFI_DISCONNECTED,
    APP_EVENT_WIFI_CONFIG_RECEIVED,
    APP_EVENT_WIFI_SCAN_REQUESTED,
    APP_EVENT_URL_SET,
    APP_EVENT_URL_SET_DONE,
    APP_EVENT_LOW_BATTERY,
    APP_EVENT_CRITICAL_BATTERY,
    APP_EVENT_COUNT,
} app_event_t;

/** NVS namespace for the application keys. */
#define APP_NVS_NAMESPACE "terminal"

/**
 * @brief Queue an event for the state task without waiting
 *
 * Safe from any task and from NimBLE, esp_event and esp_timer callbacks.
 * The data is copied.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE before the queue exists,
 *         ESP_ERR_INVALID_SIZE when the data does not fit an event,
 *         ESP_ERR_TIMEOUT when the queue is full
 */
esp_err_t app_post_event(app_event_t event, const void *data, size_t data_len);

/** Like app_post_event(), but waits for a free queue slot. Only for tasks that may block. */
esp_err_t app_post_event_wait(app_event_t event, const void *data, size_t data_len);

/** Give hal_ble the device id stored in NVS. Without a stored id hal_ble keeps its MAC-derived id. */
esp_err_t app_device_id_publish(void);

/**
 * Handle a URL_SET request: validate the id and start the tag write task. On an invalid id or a task
 * failure it answers the BLE client itself.
 *
 * @return true when the tag task runs and will post URL_SET_DONE
 */
bool app_url_set_start(const char *device_id, size_t len);

/** Answer a URL_SET request that cannot run; the reason becomes the result message. */
void app_url_set_reject(const char *reason);

/**
 * Handle the end of the tag write: BLE result, stored id and diagnostics. The caller owns the feedback.
 *
 * @return true when the tag accepted the configuration
 */
bool app_url_set_finish(hal_nfc_err_t nfc_err);

/** Start a WiFi scan for the BLE client unless one runs. The scan result goes out over BLE. */
void app_wifi_scan_start(void);

#endif /* APP_STATE_PRIV_H */
