/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include HAL_POWER_SRC
#include "check.h"

#define CRITICAL_MV 3000
#define NORMAL_MV 4000
#define READINGS 5
#define MAX_CALLS 8

static jmp_buf stop_jmp;
static const int *battery_mv;
static int reading;
static int calls;
static int call_reading[MAX_CALLS];
static int fail_left;
static int delivered_critical;

esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t c, int *raw)
{
    if (reading == READINGS) {
        longjmp(stop_jmp, 1);
    }
    *raw = battery_mv[reading] / VOLTAGE_DIVIDER_RATIO;
    return ESP_OK;
}

esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv)
{
    *mv = raw;
    reading++;
    return ESP_OK;
}

esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h)
{
    *h = (void *)1;
    return ESP_OK;
}

esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t c, const adc_oneshot_chan_cfg_t *x)
{
    return ESP_OK;
}

esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h)
{
    return ESP_OK;
}

esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *h)
{
    *h = (void *)1;
    return ESP_OK;
}

esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h)
{
    return ESP_OK;
}

void esp_deep_sleep_start(void)
{
    abort();
}

static esp_err_t cb(hal_battery_level_t level)
{
    if (calls < MAX_CALLS) {
        call_reading[calls] = reading;
    }
    calls++;
    if (fail_left > 0) {
        fail_left--;
        return ESP_ERR_TIMEOUT;
    }
    if (level == HAL_BATTERY_CRITICAL) {
        delivered_critical++;
    }
    return ESP_OK;
}

static void monitor(const int mv[READINGS], int failures)
{
    battery_mv = mv;
    reading = 0;
    calls = 0;
    memset(call_reading, 0, sizeof call_reading);
    fail_left = failures;
    delivered_critical = 0;
    if (!setjmp(stop_jmp)) {
        battery_monitor_task(NULL);
    }
}

int main(void)
{
    char d[200];
    static const int critical[READINGS] = { CRITICAL_MV, CRITICAL_MV, CRITICAL_MV, CRITICAL_MV, CRITICAL_MV };
    static const int one_dip[READINGS] = { CRITICAL_MV, NORMAL_MV, NORMAL_MV, NORMAL_MV, NORMAL_MV };
    hal_power_register_low_battery_cb(cb);

    monitor(critical, 1);
    snprintf(d, sizeof d, "5 critical readings, first delivery fails: calls on readings %d and %d of %d, %d calls, "
             "critical delivered=%d", call_reading[0], call_reading[1], reading, calls, delivered_critical);
    check("a critical level is offered on the 2nd critical reading, again on the next after a failed delivery, then never",
          calls == 2 && call_reading[0] == 2 && call_reading[1] == 3 && delivered_critical == 1, d);

    monitor(one_dip, 0);
    snprintf(d, sizeof d, "1 critical reading, then 4 normal: %d calls", calls);
    check("a single critical reading is not reported", calls == 0, d);
    return check_summary();
}
