/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_MAC_BT } esp_mac_type_t;
esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t type);
