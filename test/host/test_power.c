/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include HAL_POWER_SRC

extern int64_t host_now_us;
static jmp_buf stop_jmp;
static int sample, samples_left, cb_calls, cb_fail_left, delivered_critical;
static const int *volts; static int nvolts;
esp_err_t adc_oneshot_read(adc_oneshot_unit_handle_t h, adc_channel_t c, int *raw)
{
    (void)h;
    (void)c;
    *raw = volts[sample < nvolts ? sample : nvolts - 1] / 2;
    return ESP_OK;
}
esp_err_t adc_cali_raw_to_voltage(adc_cali_handle_t h, int raw, int *mv)
{
    (void)h;
    *mv = raw;
    sample++;
    if (--samples_left < 0) {
        longjmp(stop_jmp, 1);
    } return ESP_OK;
}
esp_err_t adc_oneshot_new_unit(const adc_oneshot_unit_init_cfg_t *c, adc_oneshot_unit_handle_t *h)
{
    (void)c;
    *h = (void *)1;
    return ESP_OK;
}
esp_err_t adc_oneshot_config_channel(adc_oneshot_unit_handle_t h, adc_channel_t c, const adc_oneshot_chan_cfg_t *x)
{
    (void)h;
    (void)c;
    (void)x;
    return ESP_OK;
}
esp_err_t adc_oneshot_del_unit(adc_oneshot_unit_handle_t h)
{
    (void)h;
    return ESP_OK;
}
esp_err_t adc_cali_create_scheme_curve_fitting(const adc_cali_curve_fitting_config_t *c, adc_cali_handle_t *h)
{
    (void)c;
    *h = (void *)1;
    return ESP_OK;
}
esp_err_t adc_cali_delete_scheme_curve_fitting(adc_cali_handle_t h)
{
    (void)h;
    return ESP_OK;
}
void esp_deep_sleep_start(void) {}
BaseType_t xTaskCreate(TaskFunction_t f, const char *n, uint32_t s, void *a, UBaseType_t p, TaskHandle_t *o)
{
    (void)f;
    (void)n;
    (void)s;
    (void)a;
    (void)p;
    (void)o;
    return pdPASS;
}

static esp_err_t cb(hal_battery_level_t level)
{
    cb_calls++;
    if (cb_fail_left > 0) {
        cb_fail_left--;
        return ESP_ERR_TIMEOUT;
    }
    if (level == HAL_BATTERY_CRITICAL) {
        delivered_critical++;
    }
    return ESP_OK;
}

#include "check.h"
int main(void)
{
    char d[160];
    static const int critical[] = { 3000 };
    volts = critical; nvolts = 1;
    hal_power_register_low_battery_cb(cb);
    sample = 0; samples_left = 5 * ADC_SAMPLES; cb_calls = 0; cb_fail_left = 1; delivered_critical = 0;
    if (!setjmp(stop_jmp)) {
        battery_monitor_task(NULL);
    }
    snprintf(d, sizeof d, "5 critical readings, first delivery fails: callback calls=%d, critical delivered=%d", cb_calls, delivered_critical);
    check("a critical level whose delivery failed is delivered on the next reading", delivered_critical == 1, d);
    sample = 0; samples_left = 5 * ADC_SAMPLES; cb_calls = 0; cb_fail_left = 0; delivered_critical = 0;
    if (!setjmp(stop_jmp)) {
        battery_monitor_task(NULL);
    }
    snprintf(d, sizeof d, "callback calls=%d, critical delivered=%d", cb_calls, delivered_critical);
    check("a delivered critical level is sent once", cb_calls == 1 && delivered_critical == 1, d);
    return check_summary();
}
