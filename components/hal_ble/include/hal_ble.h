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
 * Security: the three write characteristics, and their notification
 * switches (CCCD), require an encrypted link. The
 * device pairs with LE Secure Connections, Just Works (no passkey, no MITM
 * protection) and stores the bond. A write on an unencrypted link fails with
 * ATT error 0x05 (Insufficient Authentication) when the peer has no stored
 * key, or 0x0F (Insufficient Encryption) when it has one. Both make the phone
 * start pairing or encrypt the link. Writes use the acknowledged ATT write, so
 * errors reach the client. DEVICE_INFO is readable without pairing.
 *
 * The client must enable notifications (CCCD) on a characteristic before it
 * writes to it, and a write without them fails with ATT error 0xFD (Client
 * Characteristic Configuration Descriptor Improperly Configured). A send on a
 * characteristic whose notifications the client turned off later fails with
 * ESP_ERR_INVALID_STATE and ends the connection. The client must request an
 * ATT MTU of at least 100 bytes, and a write on a link with a smaller MTU
 * fails with ATT error 0xFC (Write Request Rejected). The device cannot start
 * the MTU exchange itself, because the NimBLE GATT client needs the central
 * role, which this firmware turns off. A write the application cannot queue
 * fails with ATT error 0x11 (Insufficient Resources). An acknowledged write
 * gets its reply or the end of its connection, except a repeated WIFI_SCAN
 * from the same session while its scan runs and a WIFI_CONFIG whose connect
 * attempt a short press ends. A request whose client disconnects before the
 * application handles it starts nothing and gets no reply. A sent reply means
 * the NimBLE stack accepted the notification, not that the phone received it.
 *
 * WIFI_SCAN accepts any write. WIFI_CONFIG and URL_SET writes are limited to
 * 200 bytes, and one that is empty or over that limit fails with ATT error
 * 0x0D (Invalid Attribute Value Length). So does a WIFI_CONFIG with an empty
 * SSID, an SSID over 32 bytes or a password over 64 bytes, and a URL_SET with
 * an empty deviceId or one over 31 bytes. A WIFI_CONFIG or URL_SET write that
 * is not one JSON object with only whitespace after it, repeats a top-level
 * member name (compared after unescaping and case-sensitively, so
 * "device\u0049d" repeats "deviceId"), or holds a NUL character (raw or as the
 * escape \u0000) fails with ATT error 0x13 (Value Not Allowed). So does a
 * WIFI_CONFIG without a string "ssid" or with a "password" that is not a
 * string, and a URL_SET without a string "deviceId". Other members whose
 * top-level names are unique are ignored, nested objects included. A
 * WIFI_CONFIG without "password" selects an open network.
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
 *   notifications sent and "skipped" the number not sent, because their
 *   JSON did not fit in ATT MTU - 3 bytes or a send failed. count + skipped
 *   is the number of networks found. An empty scan sends only the
 *   terminator with count 0. The client treats the scan as complete on "done".
 *   When the stack does not take a network notification within 250 ms, the
 *   device sends no terminator and ends the connection.
 * - WIFI_CONFIG: {"success":bool,"ip":"..","msg":".."} after the connect
 *   attempt.
 * - URL_SET: {"success":bool,"code":int,"message":".."} after the SDM
 *   provisioning attempt.
 *
 * The 250 ms limits count from the moment the NimBLE host queue takes the
 * request. The NimBLE port waits without a limit for a free slot in that
 * queue (npl_os_freertos.c, npl_freertos_eventq_put) and offers no call that
 * returns at once, so a stalled host with a full queue blocks the caller
 * longer.
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
 * @brief One BLE connection
 *
 * Every request carries the session of the connection that sent it. Each
 * connection gets a new session number, and no number repeats within one
 * boot. A reply sent with a session reaches only that connection, never a
 * client that connected later, even on the same connection handle: the NimBLE
 * host task checks the session and submits the notification in one step, so
 * no connect or disconnect event runs in between.
 */
typedef uint32_t hal_ble_session_t;

/** No connection */
#define HAL_BLE_SESSION_NONE UINT32_MAX

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
 * @param session The connection that sent the request; pass it to the reply
 * @param data Event-specific data (see hal_ble_event_t), NULL if not applicable
 * @param data_len Length of event data in bytes
 * @return ESP_OK when the application took the request and will reply. Any
 *         other value fails the write with ATT error 0x11.
 */
typedef esp_err_t (*hal_ble_event_cb_t)(hal_ble_event_t event, hal_ble_session_t session, const void *data,
                                        size_t data_len);

/**
 * @brief Initialize the BLE subsystem
 *
 * The device id reported by DEVICE_INFO starts as the last two Bluetooth MAC
 * bytes in hex. Call hal_ble_set_device_id() to replace it. nvs_flash_init()
 * must have succeeded before this call.
 *
 * @param event_cb Event callback, must not be NULL
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL callback,
 *         ESP_ERR_INVALID_STATE when already initialized or after a failed
 *         port deinit, ESP_ERR_NO_MEM when the NimBLE port has no timer left,
 *         another error code otherwise
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
 * The NimBLE host task ends the link of the client connected at the call,
 * after the call returns, and a notification sent just before may not reach
 * the client. A client that connects later stays connected. Advertising
 * resumes after the disconnect when it is still requested.
 *
 * @return ESP_OK
 */
esp_err_t hal_ble_disconnect(void);

/**
 * @brief Check whether the connection of a session is still up
 *
 * @param session Session from the event callback
 * @return true while that connection lasts, false after it ended
 */
bool hal_ble_session_is_current(hal_ble_session_t session);

/**
 * @brief Send WiFi scan results to the client that asked for them
 *
 * Sends one WIFI_SCAN notification per network and a final terminator (see
 * the notification formats above). Each notification waits up to 250 ms for
 * the NimBLE host task. Do not call from the event callback or the host task.
 *
 * @param session Session of the WIFI_SCAN request
 * @param results Array of scan results, may be NULL when count is 0
 * @param count Number of results in array, 0 reports an empty scan
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when that client is gone or
 *         has notifications off, ESP_ERR_INVALID_SIZE when the MTU is
 *         too small for the terminator, ESP_ERR_NO_MEM when the stack has no
 *         buffer or every notification job is in use, ESP_ERR_TIMEOUT when
 *         the host task did not answer in time, the first send error
 *         otherwise. The terminator is sent after a failed result, except
 *         after ESP_ERR_TIMEOUT. When the terminator is not sent, the device
 *         ends the connection if it still lasts.
 */
esp_err_t hal_ble_send_wifi_scan_results(hal_ble_session_t session, const hal_ble_wifi_scan_result_t *results,
                                         size_t count);

/**
 * @brief Send the WiFi configuration result to the client that sent it
 *
 * Waits up to 250 ms for the NimBLE host task. When the result is not sent
 * and the connection still lasts, the device ends it.
 *
 * @param session Session of the WIFI_CONFIG request
 * @param success true if WiFi connected successfully
 * @param ip_addr IP address if connected (NULL sends "")
 * @param message Status message
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when that client is gone
 *         or has notifications off, ESP_ERR_NO_MEM when the stack has no
 *         buffer, ESP_ERR_TIMEOUT when the host task did not answer in time,
 *         another error code otherwise
 */
esp_err_t hal_ble_send_wifi_config_result(hal_ble_session_t session, bool success, const char *ip_addr,
                                          const char *message);

/**
 * @brief Send the URL_SET / SDM provisioning result to the client that sent it
 *
 * Payload is a compact JSON object:
 * {"success":bool,"code":int,"message":"..."}
 *
 * Waits up to 250 ms for the NimBLE host task. When the result is not sent
 * and the connection still lasts, the device ends it.
 *
 * @param session Session of the URL_SET request
 * @param success true if SDM configured successfully
 * @param code Stable application-facing result code
 * @param message Human-readable result message
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE when that client is gone
 *         or has notifications off, ESP_ERR_NO_MEM when the stack has no
 *         buffer, ESP_ERR_TIMEOUT when the host task did not answer in time,
 *         another error code otherwise
 */
esp_err_t hal_ble_send_url_set_result(hal_ble_session_t session, bool success, int32_t code, const char *message);

/**
 * @brief Deinitialize the BLE subsystem
 *
 * Stops the NimBLE host task, which stops advertising and ends the
 * connection, and frees resources. Do not call it while a hal_ble_send_*()
 * call runs on another task. When the host task does not stop, BLE keeps
 * working and the call can be repeated. When the host task stopped but the
 * NimBLE port does not deinit, BLE stays off until restart: the NimBLE port
 * cannot run its deinit twice, so a repeated call returns ESP_FAIL at once.
 * hal_ble_init(), the two advertising calls and the three send calls then
 * return ESP_ERR_INVALID_STATE; hal_ble_set_device_id() and
 * hal_ble_disconnect() return ESP_OK and hal_ble_session_is_current() returns
 * false, as after a successful deinit.
 *
 * @return ESP_OK on success, ESP_FAIL when the host task did not stop or the
 *         port did not deinit now or before
 */
esp_err_t hal_ble_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_BLE_H */
