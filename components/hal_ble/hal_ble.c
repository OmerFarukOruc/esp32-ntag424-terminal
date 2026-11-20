/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_ble.c
 * @brief BLE onboarding service on the NimBLE stack
 *
 * Characteristics:
 * - DEVICE_INFO (Read): device name, id and firmware version
 * - WIFI_SCAN (Write/Notify): trigger a scan, receive results
 * - WIFI_CONFIG (Write/Notify): send WiFi credentials, receive the result
 * - URL_SET (Write/Notify): send the device id, receive the SDM result
 *
 * The wire formats and the security model are in hal_ble.h.
 */

#include "hal_ble.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "cJSON.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#if !CONFIG_BT_NIMBLE_SECURITY_ENABLE || !CONFIG_BT_NIMBLE_SM_SC
#error "hal_ble needs CONFIG_BT_NIMBLE_SECURITY_ENABLE=y and CONFIG_BT_NIMBLE_SM_SC=y (see sdkconfig.defaults)"
#endif
#if !CONFIG_BT_NIMBLE_NVS_PERSIST
#warning "CONFIG_BT_NIMBLE_NVS_PERSIST is off: bonds are lost on every reboot"
#endif

static const char *TAG = "hal_ble";

/* Not declared in any NimBLE header; the library exports it. */
void ble_store_config_init(void);

/* Chosen, not from a source: a 32-byte SSID and a 64-byte password with the JSON keys need about 120 bytes. */
#define WRITE_MAX_LEN 200
/* Chosen, not from a source: a scan entry whose 32-byte SSID is all \u00XX escapes is about 240 bytes. */
#define NOTIFY_BUF_SIZE 256
/* Chosen, not from a source: NimBLE returns BLE_HS_ENOMEM while its buffer pool is empty. */
#define NOTIFY_SEND_ATTEMPTS 3
#define NOTIFY_RETRY_DELAY_MS 20
/* ATT notification header: 1 byte opcode and 2 bytes handle (Bluetooth Core Spec, Vol 3, Part F, 3.4.7.1). */
#define ATT_NOTIFY_OVERHEAD 3
/* Common profile error codes (Core Specification Supplement, Part B); NimBLE does not define them. */
#define ATT_ERR_WRITE_REQUEST_REJECTED 0xFC
#define ATT_ERR_CCCD_IMPROPER 0xFD
#define REPLY_MIN_MTU 100
/* The request characteristics: writes and notification switches both need an encrypted link. */
#define REQUEST_CHR_FLAGS (BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_NOTIFY | \
                           BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC)
#define NAME_PREFIX "TERMINAL"
#define NAME_SUFFIX_LEN 5  /* "-" and the last two Bluetooth MAC bytes in hex */
#define ADV_INTERVAL_MS 100
#define ADV_INTERVAL_SLACK_MS 10

/* Onboarding service UUIDs: 67cbXXXX-25fe-42d1-9fce-8bf97986312f; n is XXXX's low byte. */
#define ONBOARDING_UUID(n)                                                      \
    BLE_UUID128_INIT(0x2f, 0x31, 0x86, 0x79, 0xf9, 0x8b, 0xce, 0x9f,           \
                     0xd1, 0x42, 0xfe, 0x25, (n), 0x00, 0xcb, 0x67)

static const ble_uuid128_t onboarding_svc_uuid = ONBOARDING_UUID(0x01);
static const ble_uuid128_t device_info_chr_uuid = ONBOARDING_UUID(0x02);
static const ble_uuid128_t wifi_scan_chr_uuid = ONBOARDING_UUID(0x03);
static const ble_uuid128_t wifi_config_chr_uuid = ONBOARDING_UUID(0x04);
static const ble_uuid128_t url_set_chr_uuid = ONBOARDING_UUID(0x06);

typedef struct {
    uint16_t val_handle;
    atomic_bool subscribed;  /* ble_gatts_notify_custom() does not check the CCCD, so the flag does */
} notify_chr_t;

static atomic_bool s_initialized;
static atomic_bool s_adv_wanted;  /* True from start_advertising until stop_advertising */
static _Atomic hal_ble_session_t s_session = HAL_BLE_SESSION_NONE;
static uint16_t s_connection_count;
static uint8_t s_own_addr_type;
static struct ble_npl_event s_adv_event;

static notify_chr_t s_wifi_scan;
static notify_chr_t s_wifi_config;
static notify_chr_t s_url_set;
static notify_chr_t *const s_notify_chrs[] = {&s_wifi_scan, &s_wifi_config, &s_url_set};

static hal_ble_event_cb_t s_event_cb;
static char s_device_name[sizeof(NAME_PREFIX) + NAME_SUFFIX_LEN];
static char s_device_id[HAL_BLE_DEVICE_ID_MAX];
static portMUX_TYPE s_id_lock = portMUX_INITIALIZER_UNLOCKED;

/* Returns 0 when the client can receive the reply notification on chr, otherwise an ATT error code. */
static int reply_possible(uint16_t conn_handle, notify_chr_t *chr)
{
    if (!atomic_load(&chr->subscribed)) {
        ESP_LOGW(TAG, "Write refused: notifications are off on this characteristic");
        return ATT_ERR_CCCD_IMPROPER;
    }
    uint16_t mtu = ble_att_mtu(conn_handle);
    if (mtu < REPLY_MIN_MTU) {
        ESP_LOGW(TAG, "Write refused: ATT MTU %u is below %d", mtu, REPLY_MIN_MTU);
        return ATT_ERR_WRITE_REQUEST_REJECTED;
    }
    return 0;
}

/* Hands the request of the connected client to the application. Returns 0 when it was queued,
 * otherwise an ATT error code. */
static int admit(hal_ble_event_t event, const void *data, size_t data_len)
{
    esp_err_t err = s_event_cb(event, atomic_load(&s_session), data, data_len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Request %d not queued: %s", (int)event, esp_err_to_name(err));
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return 0;
}

static hal_ble_session_t session_make(uint16_t connection_count, uint16_t conn_handle)
{
    return ((hal_ble_session_t)connection_count << 16) | conn_handle;
}

static uint16_t session_conn_handle(hal_ble_session_t session)
{
    return (uint16_t)(session & 0xFFFF);
}

static int device_info_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);
static int wifi_scan_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);
static int wifi_config_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg);
static int url_set_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);
static int gap_event_handler(struct ble_gap_event *event, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &onboarding_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[])
        {
            {
                .uuid = &device_info_chr_uuid.u,
                .access_cb = device_info_chr_access,
                .flags = BLE_GATT_CHR_F_READ,
            }, {
                .uuid = &wifi_scan_chr_uuid.u,
                .access_cb = wifi_scan_chr_access,
                .flags = REQUEST_CHR_FLAGS,
                .val_handle = &s_wifi_scan.val_handle,
            }, {
                .uuid = &wifi_config_chr_uuid.u,
                .access_cb = wifi_config_chr_access,
                .flags = REQUEST_CHR_FLAGS,
                .val_handle = &s_wifi_config.val_handle,
            }, {
                .uuid = &url_set_chr_uuid.u,
                .access_cb = url_set_chr_access,
                .flags = REQUEST_CHR_FLAGS,
                .val_handle = &s_url_set.val_handle,
            },
            { 0 },
        },
    },
    { 0 },
};

static esp_err_t nimble_to_esp(int rc)
{
    switch (rc) {
    case 0:
        return ESP_OK;
    case BLE_HS_ENOMEM:
    case BLE_HS_ENOMEM_EVT:
        return ESP_ERR_NO_MEM;
    case BLE_HS_EINVAL:
        return ESP_ERR_INVALID_ARG;
    case BLE_HS_ENOTCONN:
    case BLE_HS_ENOTSYNCED:
        return ESP_ERR_INVALID_STATE;
    default:
        return ESP_FAIL;
    }
}

/* cJSON hands out strings that end at the first NUL, so a raw NUL or a \u0000 escape would make the
 * value read differ from the value sent. */
static bool has_nul(const char *text, size_t len)
{
    if (memchr(text, '\0', len) != NULL) {
        return true;
    }
    for (size_t i = 0; i + 1 < len; i++) {
        if (text[i] != '\\') {
            continue;
        }
        if (text[i + 1] == 'u' && i + 5 < len && memcmp(&text[i + 2], "0000", 4) == 0) {
            return true;
        }
        i++;
    }
    return false;
}

/* Checks that the reply on chr can reach the client, then reads the write into a stack buffer that is
 * wiped before return and parses it. The write must be one JSON value with nothing but whitespace after
 * it. Returns 0 or an ATT error code. */
static int parse_request(uint16_t conn_handle, notify_chr_t *chr, struct os_mbuf *om, cJSON **json)
{
    int att = reply_possible(conn_handle, chr);
    if (att != 0) {
        return att;
    }

    char buf[WRITE_MAX_LEN + 1];
    uint16_t len = OS_MBUF_PKTLEN(om);
    uint16_t copied = 0;

    if (len == 0 || len > WRITE_MAX_LEN) {
        att = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    } else if (ble_hs_mbuf_to_flat(om, buf, WRITE_MAX_LEN, &copied) != 0) {
        att = BLE_ATT_ERR_UNLIKELY;
    } else if (has_nul(buf, copied)) {
        ESP_LOGW(TAG, "Write holds a NUL character");
        att = BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    } else {
        buf[copied] = '\0';
        *json = cJSON_ParseWithOpts(buf, NULL, true);
        if (*json == NULL) {
            ESP_LOGW(TAG, "Write is not one JSON value (%u bytes)", (unsigned)copied);
            att = BLE_ATT_ERR_VALUE_NOT_ALLOWED;
        }
    }

    explicit_bzero(buf, sizeof(buf));
    return att;
}

/* Fills out from the "ssid" and "password" members. Returns 0 or an ATT error code. */
static int parse_wifi_config(const cJSON *json, hal_ble_wifi_config_t *out)
{
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *pwd = cJSON_GetObjectItemCaseSensitive(json, "password");
    if (!cJSON_IsString(ssid) || (pwd != NULL && !cJSON_IsString(pwd))) {
        ESP_LOGW(TAG, "WiFi config needs a string ssid and, when present, a string password");
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }

    size_t ssid_len = strlen(ssid->valuestring);
    size_t pwd_len = pwd != NULL ? strlen(pwd->valuestring) : 0;
    if (ssid_len == 0 || ssid_len >= sizeof(out->ssid) || pwd_len >= sizeof(out->password)) {
        ESP_LOGW(TAG, "Invalid WiFi config: ssid %zu bytes, password %zu bytes", ssid_len, pwd_len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    memcpy(out->ssid, ssid->valuestring, ssid_len);
    if (pwd_len > 0) {
        memcpy(out->password, pwd->valuestring, pwd_len);
    }
    return 0;
}

/* Copies the "deviceId" member, with its NUL, into out (HAL_BLE_DEVICE_ID_MAX bytes).
 * Returns 0 or an ATT error code. */
static int parse_device_id(const cJSON *json, char *out)
{
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(json, "deviceId");
    if (!cJSON_IsString(id)) {
        ESP_LOGW(TAG, "URL_SET needs a string deviceId");
        return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }

    size_t len = strlen(id->valuestring);
    if (len == 0 || len >= HAL_BLE_DEVICE_ID_MAX) {
        ESP_LOGW(TAG, "URL_SET deviceId has %zu bytes", len);
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    memcpy(out, id->valuestring, len + 1);
    return 0;
}

/* The parsed password stays in the cJSON string until it is freed. */
static void wipe_passwords(const cJSON *json)
{
    for (const cJSON *child = json->child; child != NULL; child = child->next) {
        if (cJSON_IsString(child) && child->string != NULL && strcmp(child->string, "password") == 0) {
            explicit_bzero(child->valuestring, strlen(child->valuestring));
        }
    }
}

static int device_info_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    char device_id[HAL_BLE_DEVICE_ID_MAX];
    portENTER_CRITICAL(&s_id_lock);
    strlcpy(device_id, s_device_id, sizeof(device_id));
    portEXIT_CRITICAL(&s_id_lock);

    cJSON *json = cJSON_CreateObject();
    char *json_str = NULL;
    if (json != NULL &&
            cJSON_AddStringToObject(json, "name", s_device_name) &&
            cJSON_AddStringToObject(json, "deviceId", device_id) &&
            cJSON_AddStringToObject(json, "version", esp_app_get_description()->version)) {
        json_str = cJSON_PrintUnformatted(json);
    }
    cJSON_Delete(json);
    if (json_str == NULL) {
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    int rc = os_mbuf_append(ctxt->om, json_str, strlen(json_str));
    cJSON_free(json_str);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int wifi_scan_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    int att = reply_possible(conn_handle, &s_wifi_scan);
    if (att != 0) {
        return att;
    }
    ESP_LOGI(TAG, "WiFi scan requested via BLE");
    return admit(HAL_BLE_EVENT_WIFI_SCAN_REQ, NULL, 0);
}

static int wifi_config_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                                  struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    cJSON *json = NULL;
    int att = parse_request(conn_handle, &s_wifi_config, ctxt->om, &json);
    if (att != 0) {
        return att;
    }

    hal_ble_wifi_config_t config = {0};
    att = parse_wifi_config(json, &config);
    wipe_passwords(json);
    cJSON_Delete(json);

    if (att == 0) {
        ESP_LOGD(TAG, "WiFi config received: ssid %zu bytes", strlen(config.ssid));
        att = admit(HAL_BLE_EVENT_WIFI_CONFIG, &config, sizeof(config));
    }
    explicit_bzero(&config, sizeof(config));
    return att;
}

static int url_set_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    cJSON *json = NULL;
    int att = parse_request(conn_handle, &s_url_set, ctxt->om, &json);
    if (att != 0) {
        return att;
    }

    char id[HAL_BLE_DEVICE_ID_MAX];
    att = parse_device_id(json, id);
    cJSON_Delete(json);
    if (att != 0) {
        return att;
    }

    ESP_LOGI(TAG, "URL_SET: deviceId=%s", id);
    return admit(HAL_BLE_EVENT_URL_SET, id, strlen(id));
}

static esp_err_t adv_start(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    /* NimBLE takes the name as uint8_t, so the char array needs a cast. */
    fields.name = (const uint8_t *)s_device_name;
    fields.name_len = strlen(s_device_name);
    fields.name_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    ESP_RETURN_ON_FALSE(rc == 0, nimble_to_esp(rc), TAG, "Failed to set advertising fields: %d", rc);

    struct ble_hs_adv_fields rsp_fields = {0};
    rsp_fields.uuids128 = &onboarding_svc_uuid;
    rsp_fields.num_uuids128 = 1;
    rsp_fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    ESP_RETURN_ON_FALSE(rc == 0, nimble_to_esp(rc), TAG, "Failed to set scan response fields: %d", rc);

    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_INTERVAL_MS + ADV_INTERVAL_SLACK_MS),
    };

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
    ESP_RETURN_ON_FALSE(rc == 0, nimble_to_esp(rc), TAG, "Failed to start advertising: %d", rc);

    ESP_LOGI(TAG, "BLE advertising started");
    return ESP_OK;
}

/* Brings the controller in line with s_adv_wanted. It runs only on the host task, so two callers
 * cannot interleave a check and a start. A connected client blocks advertising (MAX_CONNECTIONS is 1);
 * the disconnect event runs this again. */
static void adv_reconcile(void)
{
    if (!ble_hs_synced()) {
        return;
    }

    bool active = ble_gap_adv_active();
    if (!atomic_load(&s_adv_wanted)) {
        if (active && ble_gap_adv_stop() == 0) {
            ESP_LOGI(TAG, "BLE advertising stopped");
        }
        return;
    }
    if (!active && atomic_load(&s_session) == HAL_BLE_SESSION_NONE) {
        (void)adv_start();
    }
}

static void adv_event_cb(struct ble_npl_event *ev)
{
    adv_reconcile();
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            ESP_LOGI(TAG, "BLE connected, conn_handle=%d", event->connect.conn_handle);
            s_connection_count++;
            atomic_store(&s_session, session_make(s_connection_count, event->connect.conn_handle));
        } else {
            ESP_LOGW(TAG, "BLE connection failed, status=%d", event->connect.status);
            adv_reconcile();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "BLE disconnected, reason=%d", event->disconnect.reason);
        atomic_store(&s_session, HAL_BLE_SESSION_NONE);
        for (size_t i = 0; i < sizeof(s_notify_chrs) / sizeof(s_notify_chrs[0]); i++) {
            atomic_store(&s_notify_chrs[i]->subscribed, false);
        }
        adv_reconcile();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscribe: conn_handle=%d attr_handle=%d notify:%d->%d",
                 event->subscribe.conn_handle, event->subscribe.attr_handle,
                 event->subscribe.prev_notify, event->subscribe.cur_notify);
        for (size_t i = 0; i < sizeof(s_notify_chrs) / sizeof(s_notify_chrs[0]); i++) {
            if (s_notify_chrs[i]->val_handle == event->subscribe.attr_handle) {
                atomic_store(&s_notify_chrs[i]->subscribed, event->subscribe.cur_notify != 0);
            }
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU update: conn_handle=%d, mtu=%d",
                 event->mtu.conn_handle, event->mtu.value);
        break;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "Encryption change: conn_handle=%d, status=%d",
                 event->enc_change.conn_handle, event->enc_change.status);
        if (event->enc_change.status != 0) {
            ESP_LOGW(TAG, "Encryption failed, terminating connection");
            ble_gap_terminate(event->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The peer lost its bond; drop ours so the new pairing can proceed. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    default:
        ESP_LOGD(TAG, "GAP event: %d", event->type);
        break;
    }

    return 0;
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to ensure address: %d", rc);
        return;
    }

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to infer address type: %d", rc);
        return;
    }

    uint8_t addr[6] = {0};
    rc = ble_hs_id_copy_addr(s_own_addr_type, addr, NULL);
    if (rc == 0) {
        ESP_LOGI(TAG, "BLE address: %02x:%02x:%02x:%02x:%02x:%02x",
                 addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    }

    adv_reconcile();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason=%d", reason);
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static esp_err_t gatt_svc_init(void)
{
    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svr_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, nimble_to_esp(rc), TAG, "Failed to count GATT config: %d", rc);

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    ESP_RETURN_ON_FALSE(rc == 0, nimble_to_esp(rc), TAG, "Failed to add GATT services: %d", rc);

    return ESP_OK;
}

esp_err_t hal_ble_init(hal_ble_event_cb_t event_cb)
{
    ESP_RETURN_ON_FALSE(event_cb != NULL, ESP_ERR_INVALID_ARG, TAG, "event_cb is NULL");
    ESP_RETURN_ON_FALSE(!atomic_load(&s_initialized), ESP_ERR_INVALID_STATE, TAG, "already initialized");

    s_event_cb = event_cb;

    uint8_t mac[6];
    ESP_RETURN_ON_ERROR(esp_read_mac(mac, ESP_MAC_BT), TAG, "Failed to read BT MAC");
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X", mac[4], mac[5]);
    snprintf(s_device_name, sizeof(s_device_name), NAME_PREFIX "-%02X%02X", mac[4], mac[5]);

    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "Failed to init NimBLE port");
    ble_npl_event_init(&s_adv_event, adv_event_cb, NULL);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* LE Secure Connections, Just Works pairing, bonded */
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    esp_err_t err = gatt_svc_init();
    if (err != ESP_OK) {
        nimble_port_deinit();
        return err;
    }

    int rc = ble_svc_gap_device_name_set(s_device_name);
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to set device name: %d", rc);
    }

    ble_store_config_init();
    nimble_port_freertos_init(ble_host_task);

    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG, "BLE HAL initialized, device name: %s", s_device_name);
    return ESP_OK;
}

esp_err_t hal_ble_set_device_id(const char *id)
{
    ESP_RETURN_ON_FALSE(id != NULL && id[0] != '\0' && strlen(id) < sizeof(s_device_id),
                        ESP_ERR_INVALID_ARG, TAG, "invalid device id");
    portENTER_CRITICAL(&s_id_lock);
    strlcpy(s_device_id, id, sizeof(s_device_id));
    portEXIT_CRITICAL(&s_id_lock);
    return ESP_OK;
}

/* Sets what the host task should do and wakes it. */
static esp_err_t adv_request(bool wanted)
{
    ESP_RETURN_ON_FALSE(atomic_load(&s_initialized), ESP_ERR_INVALID_STATE, TAG, "not initialized");

    atomic_store(&s_adv_wanted, wanted);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_adv_event);
    return ESP_OK;
}

esp_err_t hal_ble_start_advertising(void)
{
    return adv_request(true);
}

esp_err_t hal_ble_stop_advertising(void)
{
    return adv_request(false);
}

esp_err_t hal_ble_disconnect(void)
{
    hal_ble_session_t session = atomic_load(&s_session);
    if (session == HAL_BLE_SESSION_NONE) {
        return ESP_OK;
    }

    int rc = ble_gap_terminate(session_conn_handle(session), BLE_ERR_REM_USER_CONN_TERM);
    ESP_RETURN_ON_FALSE(rc == 0 || rc == BLE_HS_ENOTCONN, nimble_to_esp(rc), TAG, "Failed to disconnect: %d", rc);
    return ESP_OK;
}

bool hal_ble_session_is_current(hal_ble_session_t session)
{
    return session != HAL_BLE_SESSION_NONE && atomic_load(&s_session) == session;
}

/* Returns the object when it was built whole, otherwise frees it and returns NULL. */
static cJSON *built_or_null(cJSON *json, bool built)
{
    if (!built) {
        cJSON_Delete(json);
        return NULL;
    }
    return json;
}

/* Serializes json (taking ownership) into the ATT payload limit and notifies the client of session.
 * A NULL json means the caller failed to build it. Returns ESP_ERR_INVALID_SIZE when the object does
 * not fit in ATT MTU - 3 bytes and ESP_ERR_INVALID_STATE when that client is gone or has no
 * subscription. Waits briefly when the stack is out of buffers, so it must not run on the NimBLE
 * host task. */
static esp_err_t notify_json(notify_chr_t *chr, hal_ble_session_t session, cJSON *json)
{
    char text[NOTIFY_BUF_SIZE];
    esp_err_t ret = ESP_OK;
    uint16_t conn = session_conn_handle(session);

    ESP_GOTO_ON_FALSE(json != NULL, ESP_ERR_NO_MEM, out, TAG, "Failed to build notification");
    ESP_GOTO_ON_FALSE(hal_ble_session_is_current(session), ESP_ERR_INVALID_STATE, out, TAG,
                      "The client that sent the request is gone");
    ESP_GOTO_ON_FALSE(atomic_load(&chr->subscribed), ESP_ERR_INVALID_STATE, out, TAG,
                      "Client has not enabled notifications on this characteristic");

    uint16_t mtu = ble_att_mtu(conn);
    ESP_GOTO_ON_FALSE(mtu != 0, ESP_ERR_INVALID_STATE, out, TAG, "BLE connection lost");
    size_t max_len = mtu > ATT_NOTIFY_OVERHEAD ? mtu - ATT_NOTIFY_OVERHEAD : 0;
    ESP_GOTO_ON_FALSE(cJSON_PrintPreallocated(json, text, sizeof(text), false), ESP_ERR_INVALID_SIZE, out, TAG,
                      "Notification does not fit %u bytes", (unsigned)sizeof(text));
    size_t len = strlen(text);
    ESP_GOTO_ON_FALSE(len <= max_len, ESP_ERR_INVALID_SIZE, out, TAG,
                      "Notification of %zu bytes exceeds ATT MTU - 3 (%zu bytes)", len, max_len);

    for (int attempt = 1; attempt <= NOTIFY_SEND_ATTEMPTS; attempt++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(text, len);
        ESP_GOTO_ON_FALSE(om != NULL, ESP_ERR_NO_MEM, out, TAG, "No mbuf for notification");

        int rc = ble_gatts_notify_custom(conn, chr->val_handle, om);
        if (rc != BLE_HS_ENOMEM || attempt == NOTIFY_SEND_ATTEMPTS) {
            ESP_GOTO_ON_FALSE(rc == 0, nimble_to_esp(rc), out, TAG, "Failed to send notification: %d", rc);
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }

out:
    cJSON_Delete(json);
    return ret;
}

esp_err_t hal_ble_send_wifi_scan_results(hal_ble_session_t session, const hal_ble_wifi_scan_result_t *results,
                                         size_t count)
{
    ESP_RETURN_ON_FALSE(atomic_load(&s_initialized), ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(results != NULL || count == 0, ESP_ERR_INVALID_ARG, TAG, "results is NULL");
    ESP_RETURN_ON_FALSE(hal_ble_session_is_current(session), ESP_ERR_INVALID_STATE, TAG,
                        "The client that asked for the scan is gone");

    size_t sent = 0;
    size_t skipped = 0;
    esp_err_t first_err = ESP_OK;
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        bool built = item != NULL &&
                     cJSON_AddStringToObject(item, "ssid", results[i].ssid) &&
                     cJSON_AddNumberToObject(item, "rssi", results[i].rssi) &&
                     cJSON_AddStringToObject(item, "sec", results[i].security) &&
                     cJSON_AddNumberToObject(item, "ch", results[i].channel);
        esp_err_t err = notify_json(&s_wifi_scan, session, built_or_null(item, built));
        if (err == ESP_ERR_INVALID_SIZE) {
            skipped++;
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "scan result %zu of %zu not sent: %s", i + 1, count, esp_err_to_name(err));
            first_err = err;
            skipped += count - i;
            break;
        }
        sent++;
    }

    /* The client waits for the terminator, so it goes out even after a failed result. */
    cJSON *done = cJSON_CreateObject();
    bool built = done != NULL &&
                 cJSON_AddTrueToObject(done, "done") &&
                 cJSON_AddNumberToObject(done, "count", (double)sent) &&
                 cJSON_AddNumberToObject(done, "skipped", (double)skipped);
    esp_err_t done_err = notify_json(&s_wifi_scan, session, built_or_null(done, built));
    if (first_err != ESP_OK) {
        return first_err;
    }
    ESP_RETURN_ON_ERROR(done_err, TAG, "scan terminator not sent");

    ESP_LOGI(TAG, "Sent WiFi scan results: %zu networks, %zu skipped", sent, skipped);
    return ESP_OK;
}

esp_err_t hal_ble_send_wifi_config_result(hal_ble_session_t session, bool success, const char *ip_addr,
                                          const char *message)
{
    ESP_RETURN_ON_FALSE(atomic_load(&s_initialized), ESP_ERR_INVALID_STATE, TAG, "not initialized");

    cJSON *json = cJSON_CreateObject();
    bool built = json != NULL &&
                 cJSON_AddBoolToObject(json, "success", success) &&
                 cJSON_AddStringToObject(json, "ip", ip_addr ? ip_addr : "") &&
                 cJSON_AddStringToObject(json, "msg", message ? message : "");
    return notify_json(&s_wifi_config, session, built_or_null(json, built));
}

esp_err_t hal_ble_send_url_set_result(hal_ble_session_t session, bool success, int32_t code, const char *message)
{
    ESP_RETURN_ON_FALSE(atomic_load(&s_initialized), ESP_ERR_INVALID_STATE, TAG, "not initialized");

    cJSON *json = cJSON_CreateObject();
    bool built = json != NULL &&
                 cJSON_AddBoolToObject(json, "success", success) &&
                 cJSON_AddNumberToObject(json, "code", code) &&
                 cJSON_AddStringToObject(json, "message", message ? message : "");
    return notify_json(&s_url_set, session, built_or_null(json, built));
}

esp_err_t hal_ble_deinit(void)
{
    if (!atomic_load(&s_initialized)) {
        return ESP_OK;
    }

    hal_ble_stop_advertising();
    hal_ble_disconnect();
    atomic_store(&s_session, HAL_BLE_SESSION_NONE);

    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to stop NimBLE port: %d", rc);
    }

    rc = nimble_port_deinit();
    if (rc != 0) {
        ESP_LOGW(TAG, "Failed to deinit NimBLE port: %d", rc);
    }

    atomic_store(&s_initialized, false);
    ESP_LOGI(TAG, "BLE HAL deinitialized");
    return ESP_OK;
}
