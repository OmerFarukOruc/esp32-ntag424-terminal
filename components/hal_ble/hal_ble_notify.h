/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "esp_err.h"
#include "hal_ble.h"

typedef struct {
    uint16_t val_handle;
    bool subscribed;  /* ble_gatts_notify_custom() does not check the CCCD, so the flag does */
} notify_chr_t;

esp_err_t nimble_to_esp(int rc);
bool ble_host_conn_of(hal_ble_session_t session, uint16_t *conn_handle);

void ble_notify_init(void);
void ble_notify_set_accepting(bool accepting);
void ble_notify_deinit_events(void);
esp_err_t ble_notify_json(notify_chr_t *chr, hal_ble_session_t session, cJSON *json, bool built);
