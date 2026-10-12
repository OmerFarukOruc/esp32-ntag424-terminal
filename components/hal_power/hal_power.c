/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_power.c
 * @brief Power HAL implementation with ADC battery monitoring
 */

#include "hal_power.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "hal_power";

#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ((adc_channel_t)CONFIG_HAL_POWER_ADC_CHANNEL)
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12
#define BATTERY_ADC_BITWIDTH    ADC_BITWIDTH_12

#define ADC_SAMPLES             16
#define VOLTAGE_DIVIDER_RATIO   2
#define BATTERY_FULL_MV         4200
#define BATTERY_EMPTY_MV        3000
#define BATTERY_CRITICAL_MV     3100
#define BATTERY_LOW_MV          3300
#define BATTERY_MEDIUM_MV       3600
#define BATTERY_ABSENT_MV       1000

#define MONITOR_TASK_STACK      3072
#define MONITOR_TASK_PRIORITY   5
#define MONITOR_INTERVAL_MS     30000

static bool s_initialized = false;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static adc_cali_handle_t s_cali_handle = NULL;
static hal_low_battery_cb_t s_low_battery_callback = NULL;

static esp_err_t read_battery_voltage_mv(uint16_t *battery_mv)
{
    /* Averaging several reads smooths the ADC noise on a high-impedance divider. */
    int raw_sum = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        int sample = 0;
        ESP_RETURN_ON_ERROR(adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL, &sample), TAG, "ADC read failed");
        raw_sum += sample;
    }
    int raw = raw_sum / ADC_SAMPLES;

    int pin_mv = 0;
    ESP_RETURN_ON_ERROR(adc_cali_raw_to_voltage(s_cali_handle, raw, &pin_mv), TAG, "ADC conversion failed");

    *battery_mv = (uint16_t)(pin_mv * VOLTAGE_DIVIDER_RATIO);
    ESP_LOGD(TAG, "ADC raw=%d, pin_mv=%d, battery_mv=%u", raw, pin_mv, *battery_mv);
    return ESP_OK;
}

static hal_battery_level_t voltage_to_level(uint16_t voltage_mv)
{
    if (voltage_mv < BATTERY_ABSENT_MV) {
        return HAL_BATTERY_NORMAL;
    }
    if (voltage_mv < BATTERY_CRITICAL_MV) {
        return HAL_BATTERY_CRITICAL;
    }
    if (voltage_mv < BATTERY_LOW_MV) {
        return HAL_BATTERY_LOW;
    }
    if (voltage_mv < BATTERY_MEDIUM_MV) {
        return HAL_BATTERY_MEDIUM;
    }
    return HAL_BATTERY_NORMAL;
}

static uint8_t voltage_to_percentage(uint16_t voltage_mv)
{
    if (voltage_mv >= BATTERY_FULL_MV) {
        return 100;
    }
    if (voltage_mv <= BATTERY_EMPTY_MV) {
        return 0;
    }
    return (uint8_t)(((uint32_t)(voltage_mv - BATTERY_EMPTY_MV) * 100) / (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
}

static esp_err_t read_state(hal_power_state_t *state)
{
    ESP_RETURN_ON_ERROR(read_battery_voltage_mv(&state->voltage_mv), TAG, "Battery read failed");
    state->percentage = voltage_to_percentage(state->voltage_mv);
    state->level = voltage_to_level(state->voltage_mv);
    return ESP_OK;
}

static void battery_monitor_task(void *arg)
{
    (void)arg;
    hal_battery_level_t reported = HAL_BATTERY_NORMAL;
    hal_battery_level_t previous = HAL_BATTERY_NORMAL;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(MONITOR_INTERVAL_MS));

        hal_power_state_t state;
        if (read_state(&state) != ESP_OK) {
            continue;
        }

        /* Critical ends in a deep sleep that only a reset undoes, so a single reading below the
         * threshold, such as a sag under radio load, waits for a second one. */
        hal_battery_level_t last = previous;
        previous = state.level;
        if (state.level == reported || (state.level == HAL_BATTERY_CRITICAL && last != HAL_BATTERY_CRITICAL)) {
            continue;
        }

        ESP_LOGI(TAG, "Battery level changed: %d -> %d", reported, state.level);
        reported = state.level;
        if (s_low_battery_callback && (state.level == HAL_BATTERY_LOW || state.level == HAL_BATTERY_CRITICAL)) {
            s_low_battery_callback(state.level);
        }
    }
}

esp_err_t hal_power_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;
    const adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&init_config, &s_adc_handle), TAG, "ADC unit init failed");

    const adc_oneshot_chan_cfg_t chan_config = {
        .bitwidth = BATTERY_ADC_BITWIDTH,
        .atten = BATTERY_ADC_ATTEN,
    };
    ESP_GOTO_ON_ERROR(adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL, &chan_config), fail, TAG,
                      "ADC channel config failed");

    /* A guessed full scale could misplace the critical threshold, so a chip without calibration data
     * fails init and the battery monitor stays off visibly. */
    const adc_cali_curve_fitting_config_t cali_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .chan = BATTERY_ADC_CHANNEL,
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = BATTERY_ADC_BITWIDTH,
    };
    ESP_GOTO_ON_ERROR(adc_cali_create_scheme_curve_fitting(&cali_config, &s_cali_handle), fail, TAG,
                      "ADC calibration failed");

    hal_power_state_t initial_state;
    ESP_GOTO_ON_ERROR(read_state(&initial_state), fail, TAG, "Initial battery read failed");

    ESP_GOTO_ON_FALSE(xTaskCreate(battery_monitor_task, "batt_mon", MONITOR_TASK_STACK, NULL,
                                  MONITOR_TASK_PRIORITY, NULL) == pdPASS,
                      ESP_ERR_NO_MEM, fail, TAG, "Failed to create monitor task");

    s_initialized = true;
    ESP_LOGI(TAG, "Initialized, battery: %u%% (%umV)", initial_state.percentage, initial_state.voltage_mv);
    return ESP_OK;

fail:
    if (s_cali_handle != NULL) {
        adc_cali_delete_scheme_curve_fitting(s_cali_handle);
        s_cali_handle = NULL;
    }
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = NULL;
    return ret;
}

esp_err_t hal_power_get_state(hal_power_state_t *state)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "Not initialized");
    ESP_RETURN_ON_FALSE(state != NULL, ESP_ERR_INVALID_ARG, TAG, "State pointer is NULL");
    return read_state(state);
}

void hal_power_register_low_battery_cb(hal_low_battery_cb_t callback)
{
    s_low_battery_callback = callback;
}

void hal_power_deep_sleep(void)
{
    ESP_LOGI(TAG, "Entering deep sleep");
    esp_deep_sleep_start();
}
