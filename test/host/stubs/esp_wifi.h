/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"
#define ESP_ERR_WIFI_NOT_INIT (ESP_ERR_WIFI_BASE + 1)
#define ESP_ERR_WIFI_NOT_STARTED (ESP_ERR_WIFI_BASE + 2)
#define ESP_ERR_WIFI_CONN (ESP_ERR_WIFI_BASE + 7)
#define ESP_ERR_WIFI_STATE (ESP_ERR_WIFI_BASE + 8)
typedef enum { WIFI_AUTH_OPEN, WIFI_AUTH_WEP, WIFI_AUTH_WPA_PSK, WIFI_AUTH_WPA2_PSK, WIFI_AUTH_WPA_WPA2_PSK, WIFI_AUTH_ENTERPRISE, WIFI_AUTH_WPA3_PSK, WIFI_AUTH_WPA2_WPA3_PSK } wifi_auth_mode_t;
enum { WPA3_SAE_PWE_BOTH = 2 };
typedef struct {
    uint8_t ssid[32];
    uint8_t password[64];
    struct {
        wifi_auth_mode_t authmode;
    } threshold;
    int sae_pwe_h2e;
} wifi_sta_config_t;
typedef union {
    wifi_sta_config_t sta;
} wifi_config_t;
typedef enum { WIFI_IF_STA } wifi_interface_t;
typedef enum { WIFI_MODE_STA = 1 } wifi_mode_t;
typedef struct {
    int unused;
} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() { 0 }
typedef enum { WIFI_SCAN_TYPE_ACTIVE } wifi_scan_type_t;
typedef struct {
    bool show_hidden;
    wifi_scan_type_t scan_type;
} wifi_scan_config_t;
typedef struct {
    uint8_t ssid[33];
    int8_t rssi;
    uint8_t primary;
    wifi_auth_mode_t authmode;
} wifi_ap_record_t;
typedef struct {
    uint8_t ssid[32];
    uint8_t reason;
} wifi_event_sta_disconnected_t;
enum { WIFI_EVENT_STA_START = 2, WIFI_EVENT_STA_STOP = 3, WIFI_EVENT_STA_CONNECTED = 4, WIFI_EVENT_STA_DISCONNECTED = 5 };
enum { WIFI_REASON_ASSOC_LEAVE = 8, WIFI_REASON_MIC_FAILURE = 14, WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15, WIFI_REASON_NO_AP_FOUND = 201, WIFI_REASON_AUTH_FAIL = 202, WIFI_REASON_HANDSHAKE_TIMEOUT = 204, WIFI_REASON_CONNECTION_FAIL = 205, WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY = 210, WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD = 211 };
esp_err_t esp_wifi_init(const wifi_init_config_t *c);
esp_err_t esp_wifi_deinit(void);
esp_err_t esp_wifi_set_mode(wifi_mode_t m);
esp_err_t esp_wifi_start(void);
esp_err_t esp_wifi_stop(void);
esp_err_t esp_wifi_connect(void);
esp_err_t esp_wifi_disconnect(void);
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c);
esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c);
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool block);
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *r);
#define WIFI_REASON_UNSPECIFIED 1
