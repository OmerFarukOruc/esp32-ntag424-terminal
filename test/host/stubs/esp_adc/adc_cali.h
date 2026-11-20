/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
typedef void *adc_cali_handle_t;
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t, int, int *);
