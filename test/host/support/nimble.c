/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/ble_hs.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "host.h"

#define MBUF_POOL 8
#define HOST_QUEUE_MAX 64

struct ble_hs_cfg_t ble_hs_cfg;
uint16_t host_mtu = 247;
int host_notify_count;
char host_notify_payload[HOST_NOTIFY_MAX][HOST_NOTIFY_LEN];
int host_notify_conn[HOST_NOTIFY_MAX];
int host_notify_fail_at;
bool host_notify_enomem_once;
bool host_notify_enomem_always;
void (*host_on_enomem)(void);
void (*host_before_alloc)(void);
int host_stalled_waits;
int host_conn_id;
int host_terminations;
int host_mbuf_live;
int host_port_stop_rc;
int host_port_deinit_rc;
int host_callout_init_rc;
int host_add_svcs_rc;
int host_callouts_released_armed;
int64_t host_put_delay_us;
int host_adv_starts;
static bool port_up;
int host_terminate_rc;
int host_notify_calls;
void (*host_on_enomem_always)(void);
static int notify_attempts;
static struct os_mbuf pool[MBUF_POOL];
static int pool_i;

static struct {
    bool live;
    uint16_t handle;
} conn;

static struct host_item {
    struct ble_npl_event *ev;
    ble_gap_event_fn *gap_fn;
    struct ble_gap_event gap;
} queue[HOST_QUEUE_MAX];
static int queue_head;
static int queue_count;
static ble_gap_event_fn *gap_fn;

void host_notify_reset(void)
{
    host_notify_count = 0;
    host_notify_fail_at = 0;
    host_notify_enomem_once = false;
    host_notify_enomem_always = false;
    host_on_enomem = NULL;
    host_before_alloc = NULL;
    host_terminations = 0;
    notify_attempts = 0;
    host_notify_calls = 0;
    host_on_enomem_always = NULL;
}

static void queue_put(struct host_item item)
{
    if (queue_count == HOST_QUEUE_MAX) {
        fprintf(stderr, "host event queue full\n");
        abort();
    }
    queue[(queue_head + queue_count++) % HOST_QUEUE_MAX] = item;
}

static void deliver_gap(struct host_item *item)
{
    struct ble_gap_event *ev = &item->gap;
    if (ev->type == BLE_GAP_EVENT_CONNECT && ev->connect.status == 0) {
        conn.live = true;
        conn.handle = ev->connect.conn_handle;
        host_conn_id++;
    } else if (ev->type == BLE_GAP_EVENT_DISCONNECT) {
        conn.live = false;
    }
    item->gap_fn(ev, NULL);
}

void host_run_host(void)
{
    if (host_in_host) {
        return;
    }
    host_in_host = true;
    while (queue_count > 0) {
        struct host_item item = queue[queue_head];
        queue_head = (queue_head + 1) % HOST_QUEUE_MAX;
        queue_count--;
        if (item.ev != NULL) {
            item.ev->queued = false;
            item.ev->fn(item.ev);
        } else {
            deliver_gap(&item);
        }
    }
    host_in_host = false;
}

static void run_host_unless_stalled(void)
{
    if (host_stalled_waits > 0) {
        host_stalled_waits--;
        return;
    }
    host_run_host();
}

void host_post_gap(ble_gap_event_fn *fn, struct ble_gap_event ev)
{
    gap_fn = fn;
    queue_put((struct host_item) {
        .gap_fn = fn, .gap = ev
    });
}

int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out)
{
    uint16_t n = om->len < max_len ? om->len : max_len;
    memcpy(flat, om->data, n);
    *out = n;
    return 0;
}

struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len)
{
    if (host_before_alloc != NULL) {
        void (*hook)(void) = host_before_alloc;
        host_before_alloc = NULL;
        hook();
        run_host_unless_stalled();
    }
    host_mbuf_live++;
    struct os_mbuf *m = &pool[pool_i++ % MBUF_POOL];
    memcpy(m->data, buf, len);
    m->len = len;
    return m;
}

int os_mbuf_append(struct os_mbuf *om, const void *d, uint16_t len)
{
    memcpy(om->data + om->len, d, len);
    om->len += len;
    return 0;
}

int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f)
{
    return 0;
}

int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f)
{
    return 0;
}

int ble_gap_adv_start(uint8_t a, const ble_addr_t *d, int32_t du, const struct ble_gap_adv_params *p, ble_gap_event_fn *cb, void *arg)
{
    host_adv_starts++;
    return 0;
}

int ble_gap_adv_stop(void)
{
    return 0;
}

int ble_gap_adv_active(void)
{
    return 0;
}

int ble_hs_synced(void)
{
    return 1;
}

int ble_gap_terminate(uint16_t c, uint8_t r)
{
    if (host_terminate_rc != 0) {
        int rc = host_terminate_rc;
        host_terminate_rc = 0;
        return rc;
    }
    if (!conn.live || c != conn.handle) {
        return BLE_HS_ENOTCONN;
    }
    host_terminations++;
    host_post_gap(gap_fn, (struct ble_gap_event) {
        .type = BLE_GAP_EVENT_DISCONNECT, .disconnect = { .reason = r }
    });
    return 0;
}

int ble_gap_conn_find(uint16_t c, struct ble_gap_conn_desc *o)
{
    return conn.live && c == conn.handle ? 0 : BLE_HS_ENOTCONN;
}

int ble_store_util_delete_peer(const ble_addr_t *a)
{
    return 0;
}

int ble_hs_id_infer_auto(int p, uint8_t *o)
{
    *o = 0;
    return 0;
}

int ble_hs_id_copy_addr(uint8_t t, uint8_t *o, int *n)
{
    return 0;
}

uint16_t ble_att_mtu(uint16_t c)
{
    return conn.live && c == conn.handle ? host_mtu : 0;
}

int ble_gatts_notify_custom(uint16_t c, uint16_t h, struct os_mbuf *om)
{
    host_mbuf_live--;
    host_notify_calls++;
    if (host_notify_enomem_always) {
        if (host_on_enomem_always != NULL) {
            host_on_enomem_always();
        }
        return BLE_HS_ENOMEM;
    }
    if (host_notify_enomem_once) {
        host_notify_enomem_once = false;
        if (host_on_enomem != NULL) {
            host_on_enomem();
        }
        return BLE_HS_ENOMEM;
    }
    notify_attempts++;
    if (notify_attempts == host_notify_fail_at) {
        return BLE_HS_ENOTCONN;
    }
    if (!conn.live || c != conn.handle) {
        return BLE_HS_ENOTCONN;
    }
    if (host_notify_count < HOST_NOTIFY_MAX && om->len < HOST_NOTIFY_LEN) {
        host_notify_conn[host_notify_count] = host_conn_id;
        memcpy(host_notify_payload[host_notify_count], om->data, om->len);
        host_notify_payload[host_notify_count][om->len] = '\0';
    }
    host_notify_count++;
    return 0;
}

int ble_gatts_count_cfg(const struct ble_gatt_svc_def *d)
{
    return 0;
}

int ble_gatts_add_svcs(const struct ble_gatt_svc_def *d)
{
    return host_add_svcs_rc;
}

void ble_npl_event_init(struct ble_npl_event *ev, ble_npl_event_fn *fn, void *arg)
{
    if (ev->live) {
        fprintf(stderr, "event initialized twice without ble_npl_event_deinit\n");
        abort();
    }
    ev->fn = fn;
    ev->arg = arg;
    ev->queued = false;
    ev->live = true;
}

void ble_npl_event_deinit(struct ble_npl_event *ev)
{
    ev->live = false;
    ev->queued = false;
}

#define MAX_CALLOUTS 4
static struct ble_npl_callout *callouts[MAX_CALLOUTS];

int ble_npl_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *q, ble_npl_event_fn *fn, void *arg)
{
    if (host_callout_init_rc != 0) {
        return host_callout_init_rc;
    }
    ble_npl_event_init(&co->ev, fn, arg);
    co->armed = false;
    for (int i = 0; i < MAX_CALLOUTS; i++) {
        if (callouts[i] == NULL || callouts[i] == co) {
            callouts[i] = co;
            return 0;
        }
    }
    abort();
}

ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks)
{
    co->armed = true;
    return 0;
}

void ble_npl_callout_stop(struct ble_npl_callout *co)
{
    co->armed = false;
}

void ble_npl_callout_deinit(struct ble_npl_callout *co)
{
    if (co->armed) {
        host_callouts_released_armed++;
    }
    co->armed = false;
    ble_npl_event_deinit(&co->ev);
}

ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms)
{
    return ms;
}

int host_fire_callouts(void)
{
    int fired = 0;
    for (int i = 0; i < MAX_CALLOUTS; i++) {
        if (callouts[i] != NULL && callouts[i]->armed) {
            callouts[i]->armed = false;
            ble_npl_eventq_put(NULL, &callouts[i]->ev);
            fired++;
        }
    }
    return fired;
}

void ble_npl_eventq_put(struct ble_npl_eventq *q, struct ble_npl_event *ev)
{
    if (!ev->live) {
        fprintf(stderr, "event put after ble_npl_event_deinit\n");
        abort();
    }
    host_now_us += host_put_delay_us;
    if (ev->queued) {
        return;
    }
    ev->queued = true;
    queue_put((struct host_item) {
        .ev = ev
    });
}

static struct ble_npl_eventq q;
struct ble_npl_eventq *nimble_port_get_dflt_eventq(void)
{
    return &q;
}

int ble_store_util_status_rr(void *e, void *a)
{
    return 0;
}

esp_err_t nimble_port_init(void)
{
    host_on_block = run_host_unless_stalled;
    port_up = true;
    return 0;
}

int nimble_port_deinit(void)
{
    if (!port_up) {
        fprintf(stderr, "nimble_port_deinit after the port was deinitialized\n");
        abort();
    }
    port_up = false;
    return host_port_deinit_rc;
}

int nimble_port_stop(void)
{
    if (host_port_stop_rc == 0) {
        queue_head = queue_count = 0;
    }
    return host_port_stop_rc;
}

void nimble_port_run(void) {}
void nimble_port_freertos_init(void (*fn)(void *)) {}
void nimble_port_freertos_deinit(void) {}
int ble_hs_util_ensure_addr(int p)
{
    return 0;
}

void ble_svc_gap_init(void) {}
void ble_svc_gatt_init(void) {}
int ble_svc_gap_device_name_set(const char *n)
{
    return 0;
}

void ble_store_config_init(void) {}
const esp_app_desc_t *esp_app_get_description(void)
{
    static esp_app_desc_t d = { "v-test" };
    return &d;
}

esp_err_t esp_read_mac(uint8_t *mac, esp_mac_type_t t)
{
    memset(mac, 0xAB, 6);
    return 0;
}
