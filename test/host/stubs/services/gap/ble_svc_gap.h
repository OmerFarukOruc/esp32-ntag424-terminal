/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "host/ble_hs.h"
void ble_svc_gap_init(void);
int ble_svc_gap_device_name_set(const char *name);
