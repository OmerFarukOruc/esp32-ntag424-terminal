/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
typedef void *adc_oneshot_unit_handle_t;
typedef int adc_channel_t;
typedef enum { ADC_UNIT_1 } adc_unit_t;
typedef enum { ADC_ATTEN_DB_12 = 3 } adc_atten_t;
typedef enum { ADC_BITWIDTH_12 = 12 } adc_bitwidth_t;
#define ADC_ULP_MODE_DISABLE 0
typedef struct {
    adc_unit_t unit_id;
    int clk_src;
    int ulp_mode;
} adc_oneshot_unit_init_cfg_t;
typedef struct {
    adc_atten_t atten;
    adc_bitwidth_t bitwidth;
} adc_oneshot_chan_cfg_t;
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *, adc_oneshot_unit_handle_t *);
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t, adc_channel_t, const adc_oneshot_chan_cfg_t *);
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t, adc_channel_t, int *);
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t);
