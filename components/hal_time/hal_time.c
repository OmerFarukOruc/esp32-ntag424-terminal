/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_time.c
 * @brief SNTP time synchronization implementation
 */

#include "hal_time.h"

#include "esp_check.h"
#include "esp_netif_sntp.h"

#include <sys/time.h>
#include <time.h>

static const char *TAG = "hal_time";

#define SNTP_SERVER_PRIMARY   CONFIG_HAL_TIME_NTP_SERVER_PRIMARY
#define SNTP_SERVER_SECONDARY CONFIG_HAL_TIME_NTP_SERVER_SECONDARY

static void sntp_sync_cb(struct timeval *tv)
{
    struct tm timeinfo;
    char text[32];
    gmtime_r(&tv->tv_sec, &timeinfo);
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &timeinfo);
    ESP_LOGI(TAG, "Time synchronized: %s UTC", text);
}

/* Several SNTP sources need a second server slot, and the lwIP default has one. */
_Static_assert(CONFIG_LWIP_SNTP_MAX_SERVERS >= 2, "set CONFIG_LWIP_SNTP_MAX_SERVERS to at least 2");

static bool s_sntp_ready;

esp_err_t hal_time_start_sync(void)
{
    if (!s_sntp_ready) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_SERVER_PRIMARY);
        config.start = false;
        config.wait_for_sync = false;
        config.sync_cb = sntp_sync_cb;
        if (SNTP_SERVER_SECONDARY[0] != '\0') {
            config.num_of_servers = 2;
            config.servers[1] = SNTP_SERVER_SECONDARY;
        }

        ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&config), TAG, "SNTP init failed");
        s_sntp_ready = true;
    }

    ESP_RETURN_ON_ERROR(esp_netif_sntp_start(), TAG, "SNTP start failed");
    ESP_LOGI(TAG, "SNTP sync started");
    return ESP_OK;
}

void hal_time_stop_sync(void)
{
    esp_netif_sntp_deinit();
    s_sntp_ready = false;
}
