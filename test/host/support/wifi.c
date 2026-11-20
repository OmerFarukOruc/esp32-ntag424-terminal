/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "host.h"

#define MAX_HANDLERS 8
#define MAX_PENDING 16
#define EVENT_DATA_MAX 64

esp_event_base_t const WIFI_EVENT = "WIFI_EVENT";
esp_event_base_t const IP_EVENT = "IP_EVENT";

int wifi_connect_calls;
int wifi_disconnect_calls;
esp_err_t wifi_connect_result;
esp_err_t wifi_disconnect_result;
int host_event_post_fails;

static wifi_config_t saved_config;
static bool started;
esp_err_t wifi_stop_result;
esp_err_t wifi_deinit_result;

static struct handler {
    bool used;
    esp_event_base_t base;
    int32_t id;
    esp_event_handler_t fn;
    void *arg;
} handlers[MAX_HANDLERS];

static struct {
    esp_event_base_t base;
    int32_t id;
    unsigned char data[EVENT_DATA_MAX];
} pending[MAX_PENDING];
static int pending_count;

void host_wifi_reset(void)
{
    wifi_connect_calls = 0;
    wifi_disconnect_calls = 0;
    wifi_connect_result = ESP_OK;
    wifi_disconnect_result = ESP_OK;
    wifi_stop_result = ESP_OK;
    wifi_deinit_result = ESP_OK;
    host_event_post_fails = 0;
    memset(&saved_config, 0, sizeof saved_config);
    started = false;
    pending_count = 0;
}

esp_err_t esp_event_loop_create_default(void)
{
    return ESP_OK;
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id, esp_event_handler_t fn, void *arg,
                                              esp_event_handler_instance_t *out)
{
    for (int i = 0; i < MAX_HANDLERS; i++) {
        if (!handlers[i].used) {
            handlers[i] = (struct handler) {
                .used = true, .base = base, .id = id, .fn = fn, .arg = arg
            };
            *out = &handlers[i];
            return ESP_OK;
        }
    }
    return ESP_ERR_NO_MEM;
}

esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id, esp_event_handler_instance_t inst)
{
    struct handler *h = inst;
    h->used = false;
    return ESP_OK;
}

esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void *data, size_t size, uint32_t ticks)
{
    if (host_event_post_fails > 0) {
        host_event_post_fails--;
        return ESP_ERR_TIMEOUT;
    }
    if (pending_count == MAX_PENDING || size > EVENT_DATA_MAX) {
        return ESP_ERR_NO_MEM;
    }
    pending[pending_count].base = base;
    pending[pending_count].id = id;
    if (data != NULL) {
        memcpy(pending[pending_count].data, data, size);
    }
    pending_count++;
    return ESP_OK;
}

void host_event_loop_run(void)
{
    for (int i = 0; i < pending_count; i++) {
        for (int k = 0; k < MAX_HANDLERS; k++) {
            struct handler *h = &handlers[k];
            if (h->used && strcmp(h->base, pending[i].base) == 0 && (h->id == ESP_EVENT_ANY_ID || h->id == pending[i].id)) {
                h->fn(h->arg, pending[i].base, pending[i].id, pending[i].data);
            }
        }
    }
    pending_count = 0;
}

esp_err_t esp_netif_init(void)
{
    return ESP_OK;
}

static int netif;

esp_netif_t *esp_netif_create_default_wifi_sta(void)
{
    return (esp_netif_t *)&netif;
}

void esp_netif_destroy_default_wifi(void *n)
{
}

esp_err_t esp_wifi_init(const wifi_init_config_t *c)
{
    return ESP_OK;
}

esp_err_t esp_wifi_deinit(void)
{
    return wifi_deinit_result;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t m)
{
    return ESP_OK;
}

esp_err_t esp_wifi_start(void)
{
    if (!started) {
        started = true;
        esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_START, NULL, 0, 0);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_stop(void)
{
    if (wifi_stop_result != ESP_OK) {
        return wifi_stop_result;
    }
    if (started) {
        started = false;
        esp_event_post(WIFI_EVENT, WIFI_EVENT_STA_STOP, NULL, 0, 0);
    }
    return ESP_OK;
}

esp_err_t esp_wifi_connect(void)
{
    wifi_connect_calls++;
    return wifi_connect_result;
}

esp_err_t esp_wifi_disconnect(void)
{
    wifi_disconnect_calls++;
    if (wifi_disconnect_result != ESP_OK) {
        return wifi_disconnect_result;
    }
    return started ? ESP_OK : ESP_ERR_WIFI_NOT_STARTED;
}

esp_err_t esp_wifi_set_config(wifi_interface_t i, wifi_config_t *c)
{
    saved_config = *c;
    return ESP_OK;
}

esp_err_t esp_wifi_get_config(wifi_interface_t i, wifi_config_t *c)
{
    *c = saved_config;
    return ESP_OK;
}

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t *c, bool block)
{
    return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t *n, wifi_ap_record_t *r)
{
    *n = 0;
    return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *r)
{
    return ESP_ERR_WIFI_NOT_STARTED;
}
