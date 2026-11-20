/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <string.h>
#include HAL_BUTTON_SRC

extern int64_t host_now_us;
static int level = 1;
static char seen[256];
int gpio_get_level(gpio_num_t g)
{
    (void)g;
    return level;
}
esp_err_t gpio_config(const gpio_config_t *c)
{
    (void)c;
    return ESP_OK;
}
esp_err_t gpio_intr_enable(gpio_num_t g)
{
    (void)g;
    return ESP_OK;
}
esp_err_t gpio_intr_disable(gpio_num_t g)
{
    (void)g;
    return ESP_OK;
}
esp_err_t gpio_install_isr_service(int f)
{
    (void)f;
    return ESP_OK;
}
esp_err_t gpio_isr_handler_add(gpio_num_t g, gpio_isr_t h, void *a)
{
    (void)g;
    (void)h;
    (void)a;
    return ESP_OK;
}
esp_err_t gpio_isr_handler_remove(gpio_num_t g)
{
    (void)g;
    return ESP_OK;
}
esp_err_t gpio_reset_pin(gpio_num_t g)
{
    (void)g;
    return ESP_OK;
}
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *o)
{
    (void)a;
    *o = (esp_timer_handle_t)1;
    return ESP_OK;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    (void)t;
    (void)us;
    return ESP_OK;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    (void)t;
    (void)us;
    return ESP_OK;
}
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    (void)t;
    return ESP_OK;
}
esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    (void)t;
    return ESP_OK;
}
QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t s)
{
    (void)n;
    (void)s;
    return (QueueHandle_t)1;
}
static int queued;
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t w)
{
    (void)q; (void)w; const button_msg_t *m = item; queued++;
    const char *n = m->event == HAL_BUTTON_EVENT_PRESS ? "P" : m->event == HAL_BUTTON_EVENT_HOLD ? "H" : "R";
    snprintf(seen + strlen(seen), sizeof seen - strlen(seen), "%s%s%u", seen[0] ? "," : "", n, (unsigned)m->hold_ms);
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t q, void *i, TickType_t w)
{
    (void)q;
    (void)i;
    (void)w;
    return pdFALSE;
}
void vQueueDelete(QueueHandle_t q)
{
    (void)q;
}
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
void vTaskDelete(TaskHandle_t t)
{
    (void)t;
}
static void press(void)
{
    level = 0;
    debounce_timer_callback(NULL);
}
static void release(void)
{
    level = 1;
    debounce_timer_callback(NULL);
}
static void tick_at(int64_t ms_after_press, int64_t t0)
{
    host_now_us = t0 + ms_after_press * 1000;
    hold_timer_callback(NULL);
}

#include "check.h"
int main(void)
{
    host_now_us = 1000000; seen[0] = 0;
    int64_t t0 = host_now_us; press();
    tick_at(2000, t0); tick_at(4050, t0);
    host_now_us = t0 + 4100 * 1000; release();
    check("a late tick reports every crossed second in order", strcmp(seen, "P0,H1000,H2000,H3000,H4000,R4100") == 0, seen);

    host_now_us = 9000000; seen[0] = 0;
    t0 = host_now_us; press();
    tick_at(1000, t0); tick_at(2950, t0);
    host_now_us = t0 + 3020 * 1000; release();
    check("a release before the 3 s tick still reports 3 s first", strcmp(seen, "P0,H1000,H2000,H3000,R3020") == 0, seen);

    host_now_us = 20000000; seen[0] = 0;
    t0 = host_now_us; press();
    for (int ms = 100; ms <= 1500; ms += 100) {
        tick_at(ms, t0);
    }
    host_now_us = t0 + 1500 * 1000; release();
    check("ticks on time give one event per second", strcmp(seen, "P0,H1000,R1500") == 0, seen);
    return check_summary();
}
