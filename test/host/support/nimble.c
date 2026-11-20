/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <stdio.h>
#include "host/ble_hs.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
struct ble_hs_cfg_t ble_hs_cfg;
uint16_t host_mtu = 247;
int host_notify_count; uint16_t host_notify_conn; char host_notify_payload[600];
static struct os_mbuf pool[8]; static int pool_i;
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out)
{
    uint16_t n = om->len < max_len ? om->len : max_len;
    memcpy(flat, om->data, n);
    *out = n;
    return 0;
}
struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len)
{
    struct os_mbuf *m = &pool[pool_i++ % 8];
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
    (void)f;
    return 0;
}
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f)
{
    (void)f;
    return 0;
}
int ble_gap_adv_start(uint8_t a, const ble_addr_t *d, int32_t du, const struct ble_gap_adv_params *p, ble_gap_event_fn *cb, void *arg)
{
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
    return 0;
}
int ble_gap_conn_find(uint16_t c, struct ble_gap_conn_desc *o)
{
    return 0;
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
    return host_mtu;
}
int host_notify_fail_at;
int host_notify_enomem_once; void (*host_on_enomem)(void);
int ble_gatts_notify_custom(uint16_t conn, uint16_t h, struct os_mbuf *om)
{
    if (host_notify_enomem_once) {
        host_notify_enomem_once = 0;
        if (host_on_enomem) {
            host_on_enomem();
        } return BLE_HS_ENOMEM;
    } if (host_notify_fail_at && host_notify_count + 1 == host_notify_fail_at) {
        host_notify_count++;
        return 3;
    } host_notify_count++;
    host_notify_conn = conn;
    memcpy(host_notify_payload, om->data, om->len);
    host_notify_payload[om->len] = 0;
    return 0;
}
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *d)
{
    return 0;
}
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *d)
{
    return 0;
}
void ble_npl_event_init(struct ble_npl_event *ev, ble_npl_event_fn *fn, void *arg)
{
    ev->fn = fn;
    ev->arg = arg;
}
void ble_npl_eventq_put(struct ble_npl_eventq *q, struct ble_npl_event *ev)
{
    ev->fn(ev);
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
    return 0;
}
int nimble_port_deinit(void)
{
    return 0;
}
int nimble_port_stop(void)
{
    return 0;
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
