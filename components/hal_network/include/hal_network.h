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
    char password[65];      /**< Password, empty or 8 to 64 bytes (64 means a hex PSK), NUL-terminated */
} hal_wifi_credentials_t;

/**
 * @brief Wi-Fi state callback
 *
 * Called once per state change, on the task that causes the change (usually the
 * system event task, which has a small stack). Keep it short and never block.
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
 * The registered callback stays registered for the next init.
 *
 * @return esp_err_t ESP_OK on success
 */
esp_err_t hal_network_deinit(void);

/**
 * @brief Connect to a Wi-Fi network
 *
 * Does not wait for the connection. It returns after the driver starts the
 * attempt, and the outcome arrives through the state callback: CONNECTED on
 * success, or DISCONNECTED when the attempts fail. Until the first IP address
 * with these credentials, 3 failed attempts whose reason means wrong
 * credentials or no matching access point end the retries (a few tens of
 * seconds). Any other failure reason, or a connection that worked before,
 * follows the 10 attempt rule (about 3 minutes). The credentials persist, so
 * the device reconnects after a reboot.
 *
 * Replaces any current connection. When this function fails after it stopped
 * the old connection, it reports DISCONNECTED through the callback and returns
 * the error.
 *
 * @param[in] credentials Wi-Fi credentials
 * @return ESP_OK when the attempt started, ESP_ERR_INVALID_STATE before init,
 *         ESP_ERR_INVALID_ARG for a missing, empty, or unterminated SSID or
 *         password, or a password of 1 to 7 bytes, or the error of the failing Wi-Fi driver call (for example
 *         ESP_ERR_WIFI_PASSWORD)
 */
esp_err_t hal_wifi_connect(const hal_wifi_credentials_t *credentials);

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
 * progress to end. While the scan runs, the HAL does not connect or retry. When
 * the scan ends, the HAL resumes the connection to the saved network if the
 * station is not connected. WEP and WPA-only networks are left out.
 *
 * @param[out] aps Networks found
 * @param[out] count Number of entries in aps, 0 on failure
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before init or while another
 *         scan runs, ESP_ERR_INVALID_ARG for a NULL argument, or the error of
 *         the failing Wi-Fi driver call
 */
esp_err_t hal_wifi_scan(hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX], uint16_t *count);

#ifdef __cplusplus
}
#endif

#endif /* HAL_NETWORK_H */
