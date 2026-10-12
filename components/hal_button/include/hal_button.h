/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_button.h
 * @brief Button with debounce and hold detection
 *
 * The pin, polarity, pull-up and debounce time come from menuconfig
 * (HAL button). A hold event fires once per second of hold, from 1 s up to 10 s.
 */

#ifndef HAL_BUTTON_H
#define HAL_BUTTON_H

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Button events
 */
typedef enum {
    HAL_BUTTON_EVENT_PRESS,       /**< Button pressed (after debounce) */
    HAL_BUTTON_EVENT_RELEASE,     /**< Button released */
    HAL_BUTTON_EVENT_HOLD,        /**< Held for another second, up to 10 s, after which hold tracking stops */
} hal_button_event_t;

/**
 * @brief Button event callback
 *
 * Runs on the HAL's own task, so it may take a short time but must not block
 * for long.
 *
 * @param event The button event
 * @param hold_duration_ms Hold time in milliseconds: a whole number of seconds (1000 to 10000) for HOLD, in total for RELEASE, 0 for PRESS
 */
typedef void (*hal_button_event_cb_t)(hal_button_event_t event, uint32_t hold_duration_ms);

/**
 * @brief Initialize the button
 *
 * @param[in] event_cb Callback for every button event, must not be NULL
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for a NULL callback, or the
 *         error of the failing driver call
 */
esp_err_t hal_button_init(hal_button_event_cb_t event_cb);

#ifdef __cplusplus
}
#endif

#endif /* HAL_BUTTON_H */
