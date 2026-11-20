/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_network.h
 * @brief Wi-Fi station connectivity
 *
 * The HAL keeps the station connected: after a drop or a failed attempt it
 * retries with a backoff from 1 s up to 30 s. Credentials persist in the
 * default NVS partition through the Wi-Fi driver. The HAL creates the default
 * event loop when none exists and never deletes it.
 *
 * The Wi-Fi state describes the link and says nothing about one request. A
 * request to join a network with hal_wifi_connect() is an attempt with its own
 * id and one stored result, which hal_wifi_attempt_take() hands out once.
 */

#ifndef HAL_NETWORK_H
#define HAL_NETWORK_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Wi-Fi connection state
 */
typedef enum {
    HAL_WIFI_STATE_DISCONNECTED = 0,    /**< Not connected, and no attempt in progress or retries exhausted */
    HAL_WIFI_STATE_CONNECTING,          /**< First attempt, or reconnecting after a drop or lost IP */
    HAL_WIFI_STATE_CONNECTED,           /**< Station has an IP address */
    HAL_WIFI_STATE_ERROR,               /**< The Wi-Fi driver could not start for the saved network at boot */
} hal_wifi_state_t;

/**
 * @brief Wi-Fi credentials
 *
 * With a password, the HAL joins WPA2 or stronger networks. WPA3 uses both SAE
 * password element methods. With an empty password it joins open networks.
 * WEP and WPA-only access points are not joined.
 */
typedef struct {
    char ssid[33];          /**< SSID, 1 to 32 bytes, NUL-terminated */
    char password[65];      /**< Password, empty or 8 to 64 bytes (64 bytes must be the PSK in hex digits), NUL-terminated */
} hal_wifi_credentials_t;

/**
 * @brief Wi-Fi state callback
 *
 * Called once per state change, on the task that causes the change (usually the
 * system event task, which has a small stack). Keep it short and never block.
 * It is a wake-up only: the result of an attempt comes from
 * hal_wifi_attempt_take(), which stays valid when the callback missed an event.
 *
 * Sequence after a drop: CONNECTED, then CONNECTING at the first failed
 * reconnect, then DISCONNECTED once after 10 consecutive failures. The HAL
 * keeps retrying afterwards and reports CONNECTED when the link returns.
 *
 * @param state New Wi-Fi state
 */
typedef void (*hal_wifi_event_cb_t)(hal_wifi_state_t state);

/**
 * @brief Initialize the network HAL
 *
 * Starts connecting at once when the Wi-Fi driver holds saved credentials.
 *
 * @return ESP_OK on success, or the error of the failing driver call
 */
esp_err_t hal_network_init(void);

/**
 * @brief Deinitialize the network HAL
 *
 * Refuses while a scan, a connect call, or another Wi-Fi driver call of the HAL
 * runs, and changes nothing then. Repeat the call after that work ends.
 *
 * The registered callback stays registered for the next init. When the WiFi
 * driver does not stop or deinit, the HAL is no longer usable, hal_network_init()
 * returns ESP_ERR_INVALID_STATE, and the call can be repeated.
 *
 * @return ESP_OK on success, ESP_ERR_NOT_FINISHED while Wi-Fi work runs, or the
 *         error of esp_wifi_stop() or esp_wifi_deinit()
 */
esp_err_t hal_network_deinit(void);

/**
 * @brief Identifies one attempt to join a network, 0 for none
 *
 * Ids start at 1 after each boot and count up, skipping 0 if the counter wraps.
 */
typedef uint32_t hal_wifi_attempt_id_t;

/**
 * @brief Result of an attempt
 */
typedef enum {
    HAL_WIFI_ATTEMPT_NONE = 0,  /**< No such attempt: unknown, already taken, cancelled, or replaced */
    HAL_WIFI_ATTEMPT_PENDING,   /**< Still running */
    HAL_WIFI_ATTEMPT_SUCCEEDED, /**< The station joined the network and got an IP address */
    HAL_WIFI_ATTEMPT_FAILED,    /**< The attempt ended without an IP address */
} hal_wifi_attempt_status_t;

/** An attempt ends as failed when it is still pending this long after hal_wifi_connect() accepted it. Chosen, not from a source. */
#define HAL_WIFI_ATTEMPT_DEADLINE_MS 180000

/**
 * @brief Connect to a Wi-Fi network
 *
 * Does not wait for the connection. It returns after the driver starts the
 * attempt, and hal_wifi_attempt_take() reports the result: SUCCEEDED when the
 * station holds an IP address from the access point with these credentials,
 * FAILED when the attempts fail or the deadline passes. Until the first IP
 * address with these credentials, 3 failed attempts whose reason means wrong
 * credentials or no matching access point end the retries (a few tens of
 * seconds). Any other failure reason, or a connection that worked before,
 * follows the 10 attempt rule (about 3 minutes). An attempt that joins the
 * access point but gets no IP address within 30 s, or whose connect call
 * fails at once, counts as failed. The deadline is absolute: 180 s
 * (HAL_WIFI_ATTEMPT_DEADLINE_MS) after this function accepted the request,
 * however many retries and timers ran. After a failed attempt the HAL keeps
 * retrying in the background unless the 3 attempt rule ended the retries. An
 * IP address event counts only while the driver is associated with these
 * credentials' network and the interface holds that address, so an event from
 * an earlier connection never completes a later attempt. The credentials
 * persist, so the device reconnects after a reboot.
 *
 * Replaces any current connection and retires the attempt of the previous
 * call. When this function fails after it stopped the old connection, it
 * reports DISCONNECTED through the callback and returns the error.
 *
 * @param[in] credentials Wi-Fi credentials, not kept after the call returns
 * @param[out] attempt_id The id of the new attempt, or 0 when the call fails
 * @return ESP_OK when the attempt started, ESP_ERR_INVALID_STATE before init,
 *         ESP_ERR_INVALID_ARG for a missing argument, an empty or unterminated
 *         SSID or password, a password of 1 to 7 bytes, or a 64-byte password
 *         with a character that is not a hex digit, ESP_ERR_NOT_FINISHED while
 *         a scan or another Wi-Fi driver call of the HAL runs (the old
 *         connection stays up), or the error of the failing Wi-Fi driver call
 *         (for example ESP_ERR_WIFI_PASSWORD)
 */
esp_err_t hal_wifi_connect(const hal_wifi_credentials_t *credentials, hal_wifi_attempt_id_t *attempt_id);

/**
 * @brief Take the result of an attempt
 *
 * Does not block, and any task may call it. A result is stored until this
 * function hands it out, so a missed callback loses nothing. It hands out each
 * result once: the next call for the same id returns NONE. While the attempt
 * is pending and its deadline has passed, the call ends it as FAILED. The
 * application state task calls it every 100 ms while a request waits. That is
 * a polling interval, not a bound on the delay before the client is told.
 *
 * @param attempt_id Id from hal_wifi_connect()
 * @return PENDING, or SUCCEEDED or FAILED once, or NONE
 */
hal_wifi_attempt_status_t hal_wifi_attempt_take(hal_wifi_attempt_id_t attempt_id);

/**
 * @brief Stop waiting for an attempt
 *
 * Retires the attempt and its stored result. The connection itself goes on as
 * background work. Does nothing for an id that is not current.
 *
 * @param attempt_id Id from hal_wifi_connect()
 */
void hal_wifi_attempt_cancel(hal_wifi_attempt_id_t attempt_id);

/**
 * @brief Get the current Wi-Fi state
 *
 * @return hal_wifi_state_t Current state
 */
hal_wifi_state_t hal_wifi_get_state(void);

/**
 * @brief Register the Wi-Fi state callback
 *
 * May be called before hal_network_init, so the callback sees the first state
 * change.
 *
 * @param[in] callback Callback function, NULL to unregister
 */
void hal_wifi_register_callback(hal_wifi_event_cb_t callback);

/**
 * @brief Security of a network that hal_wifi_connect() can join
 *
 * A network that supports several modes reports the strongest.
 */
typedef enum {
    HAL_WIFI_AUTH_OPEN,
    HAL_WIFI_AUTH_WPA2,
    HAL_WIFI_AUTH_WPA3,
} hal_wifi_auth_t;

/** Most networks hal_wifi_scan() returns. */
#define HAL_WIFI_SCAN_MAX 10

/**
 * @brief A network found by a scan
 */
typedef struct {
    char ssid[33];          /**< SSID, up to 32 bytes, NUL-terminated, empty for a hidden network */
    int8_t rssi;            /**< Signal strength in dBm */
    uint8_t channel;        /**< Primary channel */
    hal_wifi_auth_t auth;   /**< Security */
} hal_wifi_ap_t;

/**
 * @brief Scan for networks that hal_wifi_connect() can join
 *
 * Blocks for several seconds, so call it from its own task. Starts the Wi-Fi
 * driver when it is stopped, and waits up to 3.5 s for a connect attempt in
 * progress to end. While the scan runs, the HAL does not connect or retry, and
 * hal_wifi_connect() and hal_network_deinit() refuse. When the scan ends, the
 * HAL resumes the connection to the saved network if the station is not
 * connected. WEP and WPA-only networks are left out.
 *
 * A scan does not start while an attempt of hal_wifi_connect() is pending, or
 * while another scan or Wi-Fi driver call of the HAL runs. It changes nothing
 * then and returns ESP_ERR_NOT_FINISHED with an empty result.
 *
 * @param[out] aps Networks found
 * @param[out] count Number of entries in aps, 0 on failure
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before init,
 *         ESP_ERR_NOT_FINISHED when it did not start, ESP_ERR_INVALID_ARG for
 *         a NULL argument, or the error of the failing Wi-Fi driver call
 */
esp_err_t hal_wifi_scan(hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX], uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif /* HAL_NETWORK_H */
