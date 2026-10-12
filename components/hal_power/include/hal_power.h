/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_power.h
 * @brief Battery monitoring and deep sleep
 *
 * The battery connects to an ADC1 pin through a 1:2 voltage divider. The
 * channel is set in menuconfig (HAL power).
 */

#ifndef HAL_POWER_H
#define HAL_POWER_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Battery levels, classified by battery voltage
 *
 * A reading below 1000 mV means no battery is connected and reports
 * HAL_BATTERY_NORMAL.
 */
typedef enum {
    HAL_BATTERY_CRITICAL = 0,   /**< Below 3100 mV */
    HAL_BATTERY_LOW,            /**< 3100 mV to 3299 mV */
    HAL_BATTERY_MEDIUM,         /**< 3300 mV to 3599 mV */
    HAL_BATTERY_NORMAL,         /**< 3600 mV and above, or no battery */
} hal_battery_level_t;

/**
 * @brief Power state structure
 */
typedef struct {
    uint16_t voltage_mv;        /**< Battery voltage in millivolts */
    uint8_t percentage;         /**< Linear estimate, 0% at 3000 mV and 100% at 4200 mV */
    hal_battery_level_t level;  /**< Battery level category */
} hal_power_state_t;

/**
 * @brief Low battery callback function type
 *
 * Runs on the battery monitor task.
 *
 * @param level HAL_BATTERY_LOW or HAL_BATTERY_CRITICAL
 */
typedef void (*hal_low_battery_cb_t)(hal_battery_level_t level);

/**
 * @brief Initialize the power HAL and start the battery monitor task
 *
 * The monitor samples every 30 s and calls the low battery callback when the
 * level changes to LOW or CRITICAL. CRITICAL needs two consecutive samples. The
 * first sample comes one interval after init, so the callback can be
 * registered after this call.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM when the monitor task cannot
 *         start, or the ADC driver error, including a calibration failure
 *         on a chip without ADC calibration data
 */
esp_err_t hal_power_init(void);

/**
 * @brief Read the battery
 *
 * @param[out] state Pointer to store power state
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before init, or the ADC
 *         driver error when the read fails
 */
esp_err_t hal_power_get_state(hal_power_state_t *state);

/**
 * @brief Register the low battery callback
 *
 * @param[in] callback Callback for low battery events
 */
void hal_power_register_low_battery_cb(hal_low_battery_cb_t callback);

/**
 * @brief Enter deep sleep
 *
 * The caller stops Wi-Fi and Bluetooth first. No wake source is armed, so the
 * device wakes only on reset.
 */
void hal_power_deep_sleep(void) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif

#endif /* HAL_POWER_H */
