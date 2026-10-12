/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_diagnostics.h
 * @brief Boot counter and post-mortem history kept in RTC memory
 *
 * The state transitions and tag-write attempts survive software resets and
 * deep sleep. After a panic, watchdog reset, or brown-out, init logs what the
 * previous run recorded. After a power-on the history starts empty. Records
 * made before hal_diagnostics_init are dropped.
 */

#ifndef HAL_DIAGNOSTICS_H
#define HAL_DIAGNOSTICS_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Restore or reset the RTC history, count the boot, and log the history
 *
 * The boot count lives in the "diagnostics" NVS namespace, which the NVS
 * partition must already be initialized for.
 *
 * @return ESP_OK on success
 */
esp_err_t hal_diagnostics_init(void);

/**
 * @brief Record a state machine transition in a ring of recent ones
 */
void hal_diagnostics_record_transition(uint8_t from_state, uint8_t to_state, uint8_t event);

/**
 * @brief Count an application event that was dropped because its queue was full
 *
 * Safe to call from any task. Logs a warning with the running total.
 */
void hal_diagnostics_record_queue_overflow(void);

/**
 * @brief One tag-write attempt
 *
 * The attempt succeeded when nfc_err and result_code are both 0.
 */
typedef struct {
    uint32_t attempt_id;
    int32_t nfc_err;
    int32_t result_code;
    bool ble_connected_start;
    bool ble_connected_end;
    uint32_t duration_ms;
} hal_diagnostics_url_set_t;

/**
 * @brief Record a tag-write attempt in a ring of recent ones
 */
void hal_diagnostics_record_url_set_attempt(const hal_diagnostics_url_set_t *attempt);

#ifdef __cplusplus
}
#endif

#endif /* HAL_DIAGNOSTICS_H */
