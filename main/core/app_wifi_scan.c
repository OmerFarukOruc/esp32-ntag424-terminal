/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "hal_ble.h"
#include "hal_network.h"
#include "app_state_priv.h"

static const char *TAG = "app_wifi_scan";

#define SCAN_TASK_STACK     5120
#define SCAN_TASK_PRIORITY  5

static bool s_scanning;

/* The security names the phone client parses. */
static const char *const s_security_names[] = {
    [HAL_WIFI_AUTH_OPEN] = "OPEN",
    [HAL_WIFI_AUTH_WPA2] = "WPA2",
    [HAL_WIFI_AUTH_WPA3] = "WPA3",
};

static void wifi_scan_task(void *arg)
{
    hal_ble_session_t session = (hal_ble_session_t)(uintptr_t)arg;
    hal_wifi_ap_t aps[HAL_WIFI_SCAN_MAX];
    hal_ble_wifi_scan_result_t results[HAL_WIFI_SCAN_MAX];
    uint16_t count = 0;

    esp_err_t err = hal_wifi_scan(aps, &count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
    }

    for (uint16_t i = 0; i < count; i++) {
        memset(&results[i], 0, sizeof(results[i]));
        strlcpy(results[i].ssid, aps[i].ssid, sizeof(results[i].ssid));
        strlcpy(results[i].security, s_security_names[aps[i].auth], sizeof(results[i].security));
        results[i].rssi = aps[i].rssi;
        results[i].channel = aps[i].channel;
        ESP_LOGI(TAG, "  [%u] %s (ch:%u, rssi:%d, %s)", (unsigned)(i + 1), results[i].ssid,
                 results[i].channel, results[i].rssi, results[i].security);
    }

    /* A failed scan reports an empty result, so the client does not wait for it. */
    ESP_LOGI(TAG, "Scan complete, found %u networks", (unsigned)count);
    err = hal_ble_send_wifi_scan_results(session, results, count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send scan results: %s", esp_err_to_name(err));
    }

    /* The state task waits for this event before it stops the radio, so it waits for a queue slot too. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_post_event_wait(APP_EVENT_WIFI_SCAN_DONE, NULL, 0));
    vTaskDelete(NULL);
}

bool app_wifi_scan_running(void)
{
    return s_scanning;
}

void app_wifi_scan_done(void)
{
    s_scanning = false;
}

void app_wifi_scan_start(hal_ble_session_t session)
{
    if (s_scanning) {
        ESP_LOGW(TAG, "Scan request ignored: a scan is already running");
        return;
    }
    s_scanning = true;

    if (xTaskCreate(wifi_scan_task, "wifi_scan", SCAN_TASK_STACK, (void *)(uintptr_t)session, SCAN_TASK_PRIORITY,
                    NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create the scan task");
        s_scanning = false;
        hal_ble_send_wifi_scan_results(session, NULL, 0);
    }
}
