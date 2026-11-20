/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "host/ble_hs.h"
esp_err_t nimble_port_init(void);
int nimble_port_deinit(void);
int nimble_port_stop(void);
void nimble_port_run(void);
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void);
