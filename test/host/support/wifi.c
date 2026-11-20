/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_timer.h"
esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t const IP_EVENT = "IP_EVENT";
int wifi_connect_calls, wifi_disconnect_calls, wifi_stop_calls, wifi_deinit_calls;
esp_err_t wifi_connect_result = ESP_OK, wifi_disconnect_result = ESP_OK;
wifi_config_t wifi_saved;
struct host_timer {
    esp_timer_create_args_t args;
    int armed;
    uint64_t us;
    int starts;
};
struct host_timer host_timers[8]; int host_timer_count;
esp_err_t esp_timer_create(const esp_timer_create_args_t *a, esp_timer_handle_t *out)
{
    struct host_timer *t = &host_timers[host_timer_count++];
    memset(t, 0, sizeof * t);
    t->args = *a;
    *out = t;
    return 0;
}
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    } t->armed = 1;
    t->us = us;
    t->starts++;
    return 0;
}
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us)
{
    return esp_timer_start_once(t, us);
}
esp_err_t esp_timer_stop(esp_timer_handle_t t)
{
    if (!t) {
        return ESP_ERR_INVALID_ARG;
    } t->armed = 0;
    return 0;
}
esp_err_t esp_timer_delete(esp_timer_handle_t t)
{
    return 0;
}
struct host_timer *host_timer_named(const char *name)
{
    for (int i = 0; i < host_timer_count; i++) if (!strcmp(host_timers[i].args.name, name)) {
            return &host_timers[i];
        } return NULL;
}
void host_timer_fire(struct host_timer *t)
{
    t->armed = 0;
    t->args.callback(t->args.arg);
    host_event_loop_run();
}
esp_err_t esp_event_loop_create_default(void)
{
    return 0;
}
static struct {
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t h;
} handlers[8]; static int nhandlers;
static struct {
    esp_event_base_t base;
    int32_t id;
    unsigned char data[64];
} pending[16]; static int npending;
int host_event_post_fails;
esp_err_t esp_event_handler_instance_register(esp_event_base_t b, int32_t id, esp_event_handler_t h, void *arg, esp_event_handler_instance_t *out)
{
    handlers[nhandlers].base = b;
    handlers[nhandlers].id = id;
    handlers[nhandlers++].h = h;
    *out = (void *)h;
    return 0;
}
esp_err_t esp_event_post(esp_event_base_t b, int32_t id, const void *data, size_t size, uint32_t ticks)
{
    if (host_event_post_fails) {
        host_event_post_fails--;
        return ESP_ERR_TIMEOUT;
    } pending[npending].base = b;
    pending[npending].id = id;
    if (data) {
        memcpy(pending[npending].data, data, size);
    } npending++;
    return 0;
}
void host_event_loop_run(void)
{
    for (int i = 0; i < npending; i++) for (int k = 0; k < nhandlers; k++) if (!strcmp(handlers[k].base, pending[i].base) && (handlers[k].id == ESP_EVENT_ANY_ID || handlers[k].id == pending[i].id)) {
                handlers[k].h(NULL, pending[i].base, pending[i].id, pending[i].data);
            } npending = 0;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t b, int32_t id, esp_event_handler_instance_t i)
{
    return 0;
}
esp_err_t esp_netif_init(void)
{
    return 0;
}
static int netif;
esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return (esp_netif_t *)&netif;
}
void esp_netif_destroy_default_wifi(void *n) {}
esp_err_t esp_wifi_init(const wifi_init_config_t *c)
{
    return 0;
}
esp_err_t esp_wifi_deinit(void)
{
    wifi_deinit_calls++;
    return 0;
}
esp_err_t esp_wifi_set_mode(wifi_mode_t m)
{
    return 0;
}
esp_err_t esp_wifi_start(void)
{
    return 0;
}
esp_err_t esp_wifi_stop(void)
{
    wifi_stop_calls++;
    return 0;
}
esp_err_t esp_wifi_connect(void)
{
    wifi_connect_calls++;
    return wifi_connect_result;
}
esp_err_t esp_wifi_disconnect(void)
{
    wifi_disconnect_calls++;
    return wifi_disconnect_result;
}
esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c)
{
    wifi_saved = *c;
    return 0;
}
esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c)
{
    *c = wifi_saved;
    return 0;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool b)
{
    return 0;
}
esp_err_t esp_wifi_scan_stop(void)
{
    return 0;
}
esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r)
{
    *n = 0;
    return 0;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *r)
{
    return ESP_ERR_WIFI_NOT_STARTED;
}
