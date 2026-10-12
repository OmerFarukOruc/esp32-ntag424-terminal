/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_time.h
 * @brief SNTP time synchronization
 *
 * The system time zone stays UTC. Servers come from menuconfig (HAL time).
 * The re-sync interval is CONFIG_LWIP_SNTP_UPDATE_DELAY.
 */

#ifndef HAL_TIME_H
#define HAL_TIME_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start SNTP synchronization
 *
 * Call after the station has an IP address. Returns at once and the sync runs
 * in the background. A running sync restarts. The first call initializes the
 * SNTP client and later calls reuse it.
 *
 * @return ESP_OK on success, or the error of esp_netif_sntp_init or
 *         esp_netif_sntp_start
 */
esp_err_t hal_time_start_sync(void);

/**
 * @brief Stop SNTP synchronization and release the SNTP client
 *
 * Safe to call when no sync runs. The next hal_time_start_sync() initializes
 * the client again.
 */
void hal_time_stop_sync(void);

#ifdef __cplusplus
}
#endif

#endif /* HAL_TIME_H */
