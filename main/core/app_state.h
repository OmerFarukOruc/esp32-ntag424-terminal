/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef APP_STATE_H
#define APP_STATE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize storage, the HALs and the state task
 *
 * A failure in storage, the indicator, diagnostics, the event queue or the
 * state task ends the initialization at once. The peripheral steps (NFC,
 * Wi-Fi, button, BLE, device id, battery monitor) all run even when one fails;
 * the device then reaches the state machine in the ERROR state, with the button
 * working unless the button step itself failed. The state task exists before any callback that posts events to it.
 *
 * @return ESP_OK when every step succeeded, otherwise the first error
 */
esp_err_t app_state_init(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_STATE_H */
