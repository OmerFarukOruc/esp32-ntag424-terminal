/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
struct os_mbuf {
    uint8_t data[512];
    uint16_t len;
};
#define OS_MBUF_PKTLEN(om) ((om)->len)
int ble_hs_mbuf_to_flat(const struct os_mbuf *om, void *flat, uint16_t max_len, uint16_t *out_copy_len);
struct os_mbuf *ble_hs_mbuf_from_flat(const void *buf, uint16_t len);
int os_mbuf_append(struct os_mbuf *om, const void *data, uint16_t len);
typedef struct {
    uint8_t type;
} ble_uuid_t;
typedef struct {
    ble_uuid_t u;
    uint8_t value[16];
} ble_uuid128_t;
#define BLE_UUID128_INIT(...) { .u = { .type = 128 }, .value = { __VA_ARGS__ } }
struct ble_gatt_access_ctxt {
    uint8_t op;
    struct os_mbuf *om;
};
typedef int ble_gatt_access_fn(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg);
struct ble_gatt_chr_def {
    const ble_uuid_t *uuid;
    ble_gatt_access_fn *access_cb;
    void *arg;
    uint32_t flags;
    uint16_t *val_handle;
};
struct ble_gatt_svc_def {
    uint8_t type;
    const ble_uuid_t *uuid;
    const struct ble_gatt_svc_def **includes;
    const struct ble_gatt_chr_def *characteristics;
};
#define BLE_GATT_SVC_TYPE_PRIMARY 1
#define BLE_GATT_CHR_F_READ 0x0002
#define BLE_GATT_CHR_F_WRITE 0x0008
#define BLE_GATT_CHR_F_NOTIFY 0x0010
#define BLE_GATT_CHR_F_WRITE_ENC 0x1000
#define BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC 0x8000
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 0x05
#define BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN 0x0d
#define BLE_ATT_ERR_UNLIKELY 0x0e
#define BLE_ATT_ERR_INSUFFICIENT_ENC 0x0f
#define BLE_ATT_ERR_INSUFFICIENT_RES 0x11
#define BLE_ATT_ERR_VALUE_NOT_ALLOWED 0x13
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_EALREADY 2
#define BLE_HS_EINVAL 3
#define BLE_HS_ENOMEM 6
#define BLE_HS_ENOTCONN 7
#define BLE_HS_ENOTSYNCED 22
#define BLE_HS_ENOMEM_EVT 20
#define BLE_HS_FOREVER INT32_MAX
#define BLE_ERR_REM_USER_CONN_TERM 0x13
struct ble_hs_adv_fields {
    uint8_t flags;
    unsigned tx_pwr_lvl_is_present: 1;
    int8_t tx_pwr_lvl;
    const uint8_t *name;
    uint8_t name_len;
    unsigned name_is_complete: 1;
    const ble_uuid128_t *uuids128;
    uint8_t num_uuids128;
    unsigned uuids128_is_complete: 1;
};
#define BLE_HS_ADV_F_DISC_GEN 0x02
#define BLE_HS_ADV_F_BREDR_UNSUP 0x04
#define BLE_HS_ADV_TX_PWR_LVL_AUTO (-128)
struct ble_gap_adv_params {
    uint8_t conn_mode, disc_mode;
    uint16_t itvl_min, itvl_max;
};
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_GAP_ADV_ITVL_MS(t) ((t) * 1000 / 625)
typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;
struct ble_gap_conn_desc {
    ble_addr_t peer_id_addr;
};
struct ble_gap_event {
    uint8_t type;
    union {
        struct {
            int status;
            uint16_t conn_handle;
        } connect;
        struct {
            int reason;
            struct ble_gap_conn_desc conn;
        } disconnect;
        struct {
            uint16_t conn_handle;
            uint16_t attr_handle;
            uint8_t reason;
            uint8_t prev_notify: 1, cur_notify: 1, prev_indicate: 1, cur_indicate: 1;
        } subscribe;
        struct {
            uint16_t conn_handle;
            uint16_t channel_id;
            uint16_t value;
        } mtu;
        struct {
            int status;
            uint16_t conn_handle;
        } enc_change;
        struct {
            uint16_t conn_handle;
        } repeat_pairing;
    };
};
#define BLE_GAP_EVENT_CONNECT 0
#define BLE_GAP_EVENT_DISCONNECT 1
#define BLE_GAP_EVENT_SUBSCRIBE 14
#define BLE_GAP_EVENT_MTU 15
#define BLE_GAP_EVENT_ENC_CHANGE 10
#define BLE_GAP_EVENT_REPEAT_PAIRING 17
#define BLE_GAP_REPEAT_PAIRING_RETRY 1
typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);
int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f);
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f);
int ble_gap_adv_start(uint8_t own_addr_type, const ble_addr_t *direct, int32_t duration, const struct ble_gap_adv_params *p, ble_gap_event_fn *cb, void *arg);
int ble_gap_adv_stop(void);
int ble_gap_adv_active(void);
int ble_hs_synced(void);
int ble_gap_terminate(uint16_t conn, uint8_t reason);
int ble_gap_conn_find(uint16_t conn, struct ble_gap_conn_desc *out);
int ble_store_util_delete_peer(const ble_addr_t *addr);
int ble_hs_id_infer_auto(int privacy, uint8_t *out);
int ble_hs_id_copy_addr(uint8_t type, uint8_t *out, int *nrpa);
uint16_t ble_att_mtu(uint16_t conn);
int ble_gatts_notify_custom(uint16_t conn, uint16_t chr_val_handle, struct os_mbuf *om);
int ble_gatts_count_cfg(const struct ble_gatt_svc_def *defs);
int ble_gatts_add_svcs(const struct ble_gatt_svc_def *defs);
struct ble_npl_event;
typedef void ble_npl_event_fn(struct ble_npl_event *ev);
struct ble_npl_event {
    ble_npl_event_fn *fn;
    void *arg;
    bool queued;
    bool live;
};
struct ble_npl_eventq {
    int unused;
};
void ble_npl_event_init(struct ble_npl_event *ev, ble_npl_event_fn *fn, void *arg);
static inline void *ble_npl_event_get_arg(struct ble_npl_event *ev)
{
    return ev->arg;
}
void ble_npl_eventq_put(struct ble_npl_eventq *q, struct ble_npl_event *ev);
void ble_npl_event_deinit(struct ble_npl_event *ev);
typedef uint32_t ble_npl_time_t;
typedef int ble_npl_error_t;
enum { BLE_NPL_OK = 0 };
struct ble_npl_callout {
    struct ble_npl_event ev;
    bool armed;
};
int ble_npl_callout_init(struct ble_npl_callout *co, struct ble_npl_eventq *q, ble_npl_event_fn *fn, void *arg);
ble_npl_error_t ble_npl_callout_reset(struct ble_npl_callout *co, ble_npl_time_t ticks);
void ble_npl_callout_stop(struct ble_npl_callout *co);
void ble_npl_callout_deinit(struct ble_npl_callout *co);
ble_npl_time_t ble_npl_time_ms_to_ticks32(uint32_t ms);
typedef void ble_hs_sync_fn(void);
typedef void ble_hs_reset_fn(int reason);
struct ble_hs_cfg_t {
    ble_hs_sync_fn *sync_cb;
    ble_hs_reset_fn *reset_cb;
    void *store_status_cb;
    uint8_t sm_io_cap;
    unsigned sm_bonding: 1, sm_mitm: 1, sm_sc: 1;
    uint8_t sm_our_key_dist, sm_their_key_dist;
};
extern struct ble_hs_cfg_t ble_hs_cfg;
int ble_store_util_status_rr(void *event, void *arg);
#define BLE_SM_IO_CAP_NO_IO 3
#define BLE_SM_PAIR_KEY_DIST_ENC 1
#define BLE_SM_PAIR_KEY_DIST_ID 2
