/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_ble.h
 * @brief BLE GATT onboarding service (NimBLE)
 *
 * The device advertises one GATT service so that a phone can scan for WiFi
 * networks, send WiFi credentials and set the device id used for the NTAG424
 * SDM URL. All payloads are compact UTF-8 JSON objects.
 *
 * Service UUID 67cb0001-25fe-42d1-9fce-8bf97986312f. Each characteristic
 * UUID replaces the 0001 with the number below.
 *
 * | Number | Name        | Access        | Content                           |
 * |--------|-------------|---------------|-----------------------------------|
 * | 0002   | DEVICE_INFO | read          | {"name":..,"deviceId":..,         |
 * |        |             |               |  "version":..}                    |
 * | 0003   | WIFI_SCAN   | write, notify | any write starts a scan           |
 * | 0004   | WIFI_CONFIG | write, notify | {"ssid":"..","password":".."}     |
 * | 0006   | URL_SET     | write, notify | {"deviceId":".."}                 |
 *
 * Security: the three write characteristics require an encrypted link. The
 * device pairs with LE Secure Connections, Just Works (no passkey, no MITM
 * protection) and stores the bond. A write on an unencrypted link fails with
 * ATT error 0x05 (Insufficient Authentication) when the peer has no stored
 * key, or 0x0F (Insufficient Encryption) when it has one. Both make the phone
 * start pairing or encrypt the link. Writes use the acknowledged ATT write, so
 * errors reach the client. DEVICE_INFO is readable without pairing.
 *
 * The client must enable notifications (CCCD) on a characteristic before it
 * writes to it. A send on a characteristic without notifications enabled
 * fails with ESP_ERR_INVALID_STATE. The client must request an ATT MTU of at least 100 bytes. The device
 * cannot start the MTU exchange itself, because the NimBLE GATT client needs
 * the central role, which this firmware turns off. With the default 23-byte MTU
 * every scan entry is skipped.
 *
 * Writes are limited to 200 bytes. A write that is empty or over that limit
 * fails with ATT error 0x0D (Invalid Attribute Value Length). So does a
 * WIFI_CONFIG with an empty SSID, an SSID over 32 bytes or a password over 64
 * bytes, and a URL_SET with an empty deviceId or one over 31 bytes. A write
 * that is not valid JSON, or lacks a string "ssid" (WIFI_CONFIG) or
 * "deviceId" (URL_SET), or has a "password" that is not a string, fails with
 * ATT error 0x13 (Value Not Allowed). A WIFI_CONFIG without "password"
 * selects an open network.
 *
 * The advertised name is "TERMINAL-XXXX" with the last two Bluetooth MAC bytes
 * in hex, for example "TERMINAL-84F2". The advertising interval is 100 ms.
 *
 * The component needs CONFIG_BT_NIMBLE_SECURITY_ENABLE and CONFIG_BT_NIMBLE_SM_SC
 * (a build error without them) and nvs_flash_init() before hal_ble_init(). Bonds
 * survive a reboot only with CONFIG_BT_NIMBLE_NVS_PERSIST.
 *
 * Notification formats:
 * - WIFI_SCAN: one notification per network, each one JSON object
 *   {"ssid":"..","rssi":-52,"sec":"WPA2","ch":6}, then a terminator
 *   {"done":true,"count":N,"skipped":M}. "count" is the number of network
 *   notifications sent and "skipped" the number that did not fit in
 *   ATT MTU - 3 bytes. An empty scan sends only the terminator
 *   with count 0. The client treats the scan as complete on "done".
 * - WIFI_CONFIG: {"success":bool,"ip":"..","msg":".."} after the connect
 *   attempt.
 * - URL_SET: {"success":bool,"code":int,"message":".."} after the SDM
 *   provisioning attempt.
 */

#ifndef HAL_BLE_H
#define HAL_BLE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Buffer size for a device id, including the terminating NUL. */
#define HAL_BLE_DEVICE_ID_MAX 32

/**
 * @brief BLE events delivered to the event callback
 */
typedef enum {
    HAL_BLE_EVENT_WIFI_SCAN_REQ,  /**< Client requested a WiFi scan; data is NULL */
    HAL_BLE_EVENT_WIFI_CONFIG,    /**< Client sent WiFi config; data is a hal_ble_wifi_config_t */
    HAL_BLE_EVENT_URL_SET,        /**< Client sent a device id; data is a NUL-terminated string and data_len excludes the NUL */
} hal_ble_event_t;

/**
 * @brief WiFi configuration received via BLE
 */
typedef struct {
    char ssid[33];      /**< WiFi SSID (max 32 chars + null) */
    char password[65];  /**< WiFi password (max 64 chars + null) */
} hal_ble_wifi_config_t;

/**
 * @brief WiFi scan result entry
 */
typedef struct {
    char ssid[33];      /**< Network SSID */
    int8_t rssi;        /**< Signal strength in dBm */
    char security[16];  /**< Security type: OPEN, WPA2 or WPA3 */
    uint8_t channel;    /**< WiFi channel */
} hal_ble_wifi_scan_result_t;

/**
 * @brief BLE event callback function type
 *
 * The callback runs on the NimBLE host task. It must return quickly and must
 * not call hal_ble_send_*(), because those calls can wait. `data` is valid
 * only during the call, and the credentials in it are wiped when the callback
 * returns, so copy what you need.
 *
 * @param event The BLE event that occurred
 * @param data Event-specific data (see hal_ble_event_t), NULL if not applicable
 * @param data_len Length of event data in bytes
 */
typedef void (*hal_ble_event_cb_t)(hal_ble_event_t event, const void *data, size_t data_len);

/**
 * @brief Initialize the BLE subsystem
 *
 * The device id reported by DEVICE_INFO starts as the last two Bluetooth MAC
 * bytes in hex. Call hal_ble_set_device_id() to replace it. nvs_flash_init()
 * must have succeeded before this call.
 *
 * @param event_cb Event callback, must not be NULL
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL callback,
 *         ESP_ERR_INVALID_STATE when already initialized, another error code
 *         otherwise
 */
esp_err_t hal_ble_init(hal_ble_event_cb_t event_cb);

/**
 * @brief Set the device id reported by DEVICE_INFO
 *
 * hal_ble does not keep the id across reboots. The owner of the stored id
 * calls this after hal_ble_init() and whenever the id changes.
 *
 * @param id NUL-terminated id, 1 to HAL_BLE_DEVICE_ID_MAX - 1 bytes
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for an empty or over-long id
 */
esp_err_t hal_ble_set_device_id(const char *id);

/**
 * @brief Start BLE advertising
 *
 * Asks the host task to advertise with the onboarding service UUID. Advertising
 * resumes after each disconnect until hal_ble_stop_advertising() is called, and
 * starts once the host stack has synced and no client is connected. The call
 * returns before advertising runs; a failure to start is logged on the host task.
 *
 * @return ESP_OK when queued, ESP_ERR_INVALID_STATE before init
 */
esp_err_t hal_ble_start_advertising(void);

/**
 * @brief Stop BLE advertising and cancel automatic re-advertising
 *
 * The change runs on the host task after the call returns. A connected client
 * stays connected until hal_ble_disconnect().
 *
 * @return ESP_OK when queued, ESP_ERR_INVALID_STATE before init
 */
esp_err_t hal_ble_stop_advertising(void);

/**
 * @brief Disconnect the connected client
 *
 * Returns ESP_OK at once when no client is connected. The link ends after the
 * call returns, and a notification sent just before may not reach the client.
 * Advertising resumes after the disconnect when it is still requested.
 *
 * @return ESP_OK, or an error code when the disconnect request failed
 */
esp_err_t hal_ble_disconnect(void);

/**
 * @brief Check if a client is connected
 *
 * @return true if connected, false otherwise
 */
bool hal_ble_is_connected(void);

/**
 * @brief Send WiFi scan results to the connected client
 *
 * Sends one WIFI_SCAN notification per network and a final terminator (see
 * the notification formats above). Do not call from the event callback.
 *
 * @param results Array of scan results, may be NULL when count is 0
 * @param count Number of results in array, 0 reports an empty scan
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE without a connection or
 *         without notifications enabled, ESP_ERR_INVALID_SIZE when the MTU is
 *         too small for the terminator, the first send error otherwise. The
 *         terminator is sent even after a failed result.
 */
esp_err_t hal_ble_send_wifi_scan_results(const hal_ble_wifi_scan_result_t *results, size_t count);

/**
 * @brief Send WiFi configuration result to connected client
 *
 * @param success true if WiFi connected successfully
 * @param ip_addr IP address if connected (NULL sends "")
 * @param message Status message
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t hal_ble_send_wifi_config_result(bool success, const char *ip_addr, const char *message);

/**
 * @brief Send URL_SET / SDM provisioning result to connected client
 *
 * Payload is a compact JSON object:
 * {"success":bool,"code":int,"message":"..."}
 *
 * @param success true if SDM configured successfully
 * @param code Stable application-facing result code
 * @param message Human-readable result message
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t hal_ble_send_url_set_result(bool success, int32_t code, const char *message);

/**
 * @brief Deinitialize the BLE subsystem
 *
 * Stops advertising, disconnects clients, and frees resources.
 *
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t hal_ble_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_BLE_H */
