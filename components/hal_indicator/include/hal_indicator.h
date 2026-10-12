/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_indicator.h
 * @brief Hardware Abstraction Layer for the status LED and the buzzer
 *
 * The LED is one WS2812 pixel driven through RMT. The buzzer is driven through
 * LEDC. Pins, buzzer type (active or passive) and active-low polarity are set
 * in menuconfig under "Indicator (hal_indicator)".
 *
 * All functions are safe to call from any task after hal_indicator_init().
 * hal_led_set_pattern() can block for a few milliseconds while the previous
 * LED frame finishes, so do not call it from an ISR.
 */

#ifndef HAL_INDICATOR_H
#define HAL_INDICATOR_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief LED pattern identifiers
 */
typedef enum {
    LED_PATTERN_IDLE = 0,           /**< Dim white pulse, 2 s cycle */
    LED_PATTERN_SCANNING,           /**< Blue pulse, 1 s cycle */
    LED_PATTERN_SUCCESS,            /**< Green solid */
    LED_PATTERN_NETWORK_ERROR,      /**< Red blink, 1 Hz */
    LED_PATTERN_LOW_BATTERY,        /**< Red slow blink, 0.25 Hz */
    LED_PATTERN_RECONNECTING,       /**< Brown blink, 1 Hz: WiFi reconnecting */
    LED_PATTERN_OFF,                /**< LED off */
    LED_PATTERN_MAX
} hal_led_pattern_t;

/**
 * @brief Buzzer sound identifiers
 */
typedef enum {
    BUZZER_SOUND_TAG_DETECTED = 0,  /**< 1 short beep */
    BUZZER_SOUND_SUCCESS,           /**< 2 short beeps */
    BUZZER_SOUND_NETWORK_ERROR,     /**< 2 long beeps */
    BUZZER_SOUND_LOW_BATTERY,       /**< 3 warning beeps */
    BUZZER_SOUND_BUTTON_PRESS,      /**< 1 very short beep */
    BUZZER_SOUND_MAX
} hal_buzzer_sound_t;

/**
 * @brief Initialize the indicator HAL (LED + buzzer)
 *
 * After init the LED is off and the buzzer is silent.
 *
 * @return ESP_OK on success, or the error of the failing driver call
 */
esp_err_t hal_indicator_init(void);

/**
 * @brief Set LED pattern
 *
 * @param[in] pattern LED pattern to display
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE before init,
 *         ESP_ERR_INVALID_ARG for an unknown pattern, or the RMT error
 *         when the LED could not be updated
 */
esp_err_t hal_led_set_pattern(hal_led_pattern_t pattern);

/**
 * @brief Queue a buzzer sound
 *
 * The sound plays on the buzzer task. The queue holds 5 sounds.
 *
 * @param[in] sound Buzzer sound to play
 * @return ESP_OK when queued, ESP_ERR_INVALID_STATE before init,
 *         ESP_ERR_INVALID_ARG for an unknown sound, ESP_ERR_TIMEOUT when
 *         the queue is full and the sound was dropped
 */
esp_err_t hal_buzzer_play(hal_buzzer_sound_t sound);

#ifdef __cplusplus
}
#endif

#endif /* HAL_INDICATOR_H */
