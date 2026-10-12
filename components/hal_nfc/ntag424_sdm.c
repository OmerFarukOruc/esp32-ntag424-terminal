/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ntag424_sdm.c
 * @brief NDEF URL builder and the SDM configuration flow for NTAG424 DNA
 *
 * One attempt is detect, select the NDEF application, authenticate with key 0,
 * WriteData of the NDEF file, then ChangeFileSettings to enable SDM. The
 * outer loop recovers the PN532 and retries.
 */

#include <string.h>
#include "hal_nfc_priv.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "hal_nfc";

#define SDM_MAX_ATTEMPTS        5
#define SDM_FIRST_DETECT_MS     15000   /* the user may still be placing the tag */
#define SDM_RETRY_DETECT_MS     5000    /* the tag should already be on the reader */
#define SDM_SELECT_RETRIES      3
#define SDM_PREFLIGHT_ATTEMPTS  3
#define SDM_PREFLIGHT_BACKOFF_MS 120
#define SDM_NDEF_FILE_NO        0x02

#define SDM_PICC_PLACEHOLDER    "00000000000000000000000000000000"
#define SDM_MAC_PLACEHOLDER     "0000000000000000"
/* "/<id>?picc_data=<32>&cmac=<16>" without the id */
#define SDM_URL_FIXED_LEN       (sizeof("/?picc_data=&cmac=") - 1 + sizeof(SDM_PICC_PLACEHOLDER) - 1 + \
                                 sizeof(SDM_MAC_PLACEHOLDER) - 1)
/* NLEN (2) + record header (4) + URI prefix code (1) */
#define NDEF_OVERHEAD_LEN       7

_Static_assert(sizeof(CONFIG_HAL_NFC_SDM_BASE_URL) - 1 + SDM_URL_FIXED_LEN + HAL_NFC_DEVICE_ID_MAX_LEN +
               NDEF_OVERHEAD_LEN <= NFC_MAX_NDEF_LEN,
               "CONFIG_HAL_NFC_SDM_BASE_URL is too long: the NDEF file must fit one WriteData frame");

/* Factory AES-128 key 0 of an NTAG424 DNA; see the key limitation in hal_nfc.h */
static const uint8_t FACTORY_KEY[16] = {0};

typedef struct {
    uint8_t msg[NFC_MAX_NDEF_LEN];
    size_t len;
    uint32_t picc_data_offset;
    uint32_t mac_offset;
} sdm_ndef_t;

static size_t append_text(uint8_t *msg, size_t pos, const char *text)
{
    size_t len = strlen(text);
    memcpy(&msg[pos], text, len);
    return pos + len;
}

/**
 * Build the NDEF file in one pass: NLEN (2 bytes) and one short URI record
 * with the SDM placeholders. Offsets count from byte 0 of the file data (the
 * NLEN byte), so they include the record header and the URI prefix
 * substitution. The static assert above guarantees the message fits.
 */
static void build_sdm_ndef(const char *device_id, sdm_ndef_t *ndef)
{
    static const struct {
        const char *text;
        uint8_t code;
    } prefixes[] = {
        { "https://www.", 0x02 },
        { "http://www.", 0x01 },
        { "https://", 0x04 },
        { "http://", 0x03 },
    };

    const char *base = CONFIG_HAL_NFC_SDM_BASE_URL;
    uint8_t prefix_code = 0x00;
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++) {
        size_t plen = strlen(prefixes[i].text);
        if (strncmp(base, prefixes[i].text, plen) == 0) {
            prefix_code = prefixes[i].code;
            base += plen;
            break;
        }
    }

    uint8_t *msg = ndef->msg;
    size_t pos = NDEF_OVERHEAD_LEN;
    pos = append_text(msg, pos, base);
    pos = append_text(msg, pos, "/");
    pos = append_text(msg, pos, device_id);
    pos = append_text(msg, pos, "?picc_data=");
    ndef->picc_data_offset = pos;
    pos = append_text(msg, pos, SDM_PICC_PLACEHOLDER);
    pos = append_text(msg, pos, "&cmac=");
    ndef->mac_offset = pos;
    pos = append_text(msg, pos, SDM_MAC_PLACEHOLDER);
    ndef->len = pos;

    size_t payload_len = pos - NDEF_OVERHEAD_LEN + 1;     /* URI prefix code plus the URL */
    msg[0] = 0x00;                                      /* NLEN */
    msg[1] = (uint8_t)(payload_len + 4);
    msg[2] = 0xD1;                                      /* MB ME SR, TNF well-known */
    msg[3] = 0x01;                                      /* type length */
    msg[4] = (uint8_t)payload_len;
    msg[5] = 0x55;                                      /* type "U" */
    msg[6] = prefix_code;

    ESP_LOGI(TAG, "SDM NDEF offsets: picc_data=%lu, mac_input=%lu, mac=%lu",
             (unsigned long)ndef->picc_data_offset, (unsigned long)ndef->mac_offset,
             (unsigned long)ndef->mac_offset);
}

static void put_le24(uint8_t *out, uint32_t value)
{
    out[0] = value & 0xFF;
    out[1] = (value >> 8) & 0xFF;
    out[2] = (value >> 16) & 0xFF;
}

/**
 * ChangeFileSettings for the NDEF file in CommMode.Full: file option 0x40
 * (SDM on), access rights 0x00E0, SDMOptions 0xC1 (UID and read counter
 * mirrored, ASCII), SDMAccessRights 0xFF 0x22 (LSB first: RFU/CtrRet = F,
 * MetaRead/FileRead = key 2), then PICCDataOffset, SDMMACInputOffset and
 * SDMMACOffset as 3 byte LE values. SDMENCOffset and SDMENCLength are omitted
 * because SDMOptions bit 4 is clear.
 */
static hal_nfc_err_t change_file_settings_sdm(const sdm_ndef_t *ndef)
{
    uint8_t data[15];
    size_t n = 0;

    data[n++] = 0x40;
    data[n++] = 0x00;
    data[n++] = 0xE0;
    data[n++] = 0xC1;
    data[n++] = 0xFF;
    data[n++] = 0x22;
    put_le24(&data[n], ndef->picc_data_offset);
    n += 3;
    put_le24(&data[n], ndef->mac_offset);   /* SDMMACInputOffset equals SDMMACOffset */
    n += 3;
    put_le24(&data[n], ndef->mac_offset);
    n += 3;

    hal_nfc_err_t err = nfc_ev2_change_file_settings(SDM_NDEF_FILE_NO, data, n);
    if (err != HAL_NFC_OK) {
        ESP_LOGE(TAG, "ChangeFileSettings with SDM failed");
        return err;
    }

    ESP_LOGI(TAG, "SDM enabled on File 02");
    return HAL_NFC_OK;
}

static int64_t ms_between(int64_t from_us, int64_t to_us)
{
    return (to_us - from_us) / 1000;
}

/** AUTH, a non-DESFire tag and a missing NDEF application give the same answer on every retry */
static bool retry_is_futile(hal_nfc_err_t err)
{
    return err == HAL_NFC_ERR_AUTH || err == HAL_NFC_ERR_TAG_NOT_DESFIRE || err == HAL_NFC_ERR_NOT_SUPPORTED;
}

/** One pass over a tag; the tag is released on every exit after detection */
static hal_nfc_err_t sdm_attempt(const sdm_ndef_t *ndef, uint32_t detect_window_ms, int attempt_no)
{
    int64_t t_start = esp_timer_get_time();
    hal_nfc_err_t err = nfc_reader_detect_tag(detect_window_ms);
    if (err != HAL_NFC_OK) {
        return err;
    }
    int64_t t_detect = esp_timer_get_time();
    int64_t t_select = 0;
    int64_t t_write = 0;
    int64_t t_end = 0;
    const uint8_t *ats = NULL;

    if (!nfc_reader_tag_is_iso_dep()) {
        err = HAL_NFC_ERR_TAG_NOT_DESFIRE;
        goto release;
    }

    ats = pn532_ats(g_nfc.pn532);
    if (ats == NULL || ats[0] == 0) {
        /* A marginal read can lose the ATS, so retry instead of reporting a missing capability. */
        err = HAL_NFC_ERR_COMM;
        goto release;
    }

    /* No delay before the first SELECT: NTAG424 DNA has a tight frame waiting
     * time and a late command makes the PN532 report a protocol error (status
     * 0x0B). A retry resyncs the PN532 first. */
    for (int r = 0; r < SDM_SELECT_RETRIES; r++) {
        if (r > 0 && !nfc_reader_recover(NULL)) {
            err = HAL_NFC_ERR_PN532_UNRESPONSIVE;
            break;
        }
        err = nfc_select_ndef_app();
        if (err == HAL_NFC_OK || retry_is_futile(err)) {
            break;
        }
    }
    if (err != HAL_NFC_OK) {
        ESP_LOGW(TAG, "SDM config: SELECT NDEF APP failed (%d) on attempt %d", err, attempt_no);
        goto release;
    }
    t_select = esp_timer_get_time();

    err = nfc_ev2_authenticate(0, FACTORY_KEY);
    if (err != HAL_NFC_OK) {
        ESP_LOGW(TAG, "SDM config: authentication failed (%d)", err);
        goto release;
    }

    err = nfc_ev2_write_file(SDM_NDEF_FILE_NO, ndef->msg, ndef->len);
    if (err != HAL_NFC_OK) {
        ESP_LOGW(TAG, "SDM config: WriteData failed (%d)", err);
        goto release;
    }
    t_write = esp_timer_get_time();
    ESP_LOGI(TAG, "SDM config: NDEF URL written via DESFire WriteData (%zu bytes)", ndef->len);

    err = change_file_settings_sdm(ndef);
    if (err != HAL_NFC_OK) {
        ESP_LOGW(TAG, "SDM config: ChangeFileSettings failed (%d)", err);
        goto release;
    }

    t_end = esp_timer_get_time();
    ESP_LOGI(TAG, "SDM timing: attempt=%d detect=%lldms select=%lldms write=%lldms settings=%lldms total=%lldms",
             attempt_no, (long long)ms_between(t_start, t_detect), (long long)ms_between(t_detect, t_select),
             (long long)ms_between(t_select, t_write), (long long)ms_between(t_write, t_end),
             (long long)ms_between(t_start, t_end));

release:
    nfc_reader_release_tag();
    return err;
}

/** Milliseconds left of the overall timeout; INT64_MAX when timeout_ms is 0 */
static int64_t remaining_ms(int64_t start_us, uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return INT64_MAX;
    }
    return ((int64_t)timeout_ms * 1000 - (esp_timer_get_time() - start_us)) / 1000;
}

/** Recovery attempts with doubling backoff before the first detect */
static bool preflight(void)
{
    uint32_t backoff_ms = SDM_PREFLIGHT_BACKOFF_MS;

    for (int attempt = 1; attempt <= SDM_PREFLIGHT_ATTEMPTS; attempt++) {
        if (nfc_reader_recover(NULL)) {
            return true;
        }
        ESP_LOGW(TAG, "SDM config: preflight recovery attempt %d/%d failed", attempt, SDM_PREFLIGHT_ATTEMPTS);
        if (attempt < SDM_PREFLIGHT_ATTEMPTS) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms *= 2;
        }
    }
    return false;
}

hal_nfc_err_t nfc_sdm_configure_locked(const char *device_id, uint32_t timeout_ms)
{
    sdm_ndef_t ndef;
    build_sdm_ndef(device_id, &ndef);
    ESP_RETURN_ON_FALSE(preflight(), HAL_NFC_ERR_PN532_UNRESPONSIVE, TAG,
                        "SDM config: PN532 unavailable after recovery");

    const int64_t start_us = esp_timer_get_time();
    hal_nfc_err_t err = HAL_NFC_ERR_NO_TAG;

    for (int attempt = 1; attempt <= SDM_MAX_ATTEMPTS; attempt++) {
        if (attempt > 1) {
            ESP_LOGW(TAG, "SDM config: Retry %d/%d - recovering PN532", attempt, SDM_MAX_ATTEMPTS);
            if (!nfc_reader_recover(NULL)) {
                ESP_LOGE(TAG, "SDM config: PN532 recovery failed");
                err = HAL_NFC_ERR_PN532_UNRESPONSIVE;
                continue;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        /* Recovery can take seconds, so size the detect window afterwards */
        int64_t left_ms = remaining_ms(start_us, timeout_ms);
        if (left_ms <= 0) {
            ESP_LOGE(TAG, "SDM config: Overall timeout reached before attempt %d", attempt);
            err = HAL_NFC_ERR_SDM_TIMEOUT;
            break;
        }
        uint32_t window_ms = attempt == 1 ? SDM_FIRST_DETECT_MS : SDM_RETRY_DETECT_MS;
        if (left_ms < (int64_t)window_ms) {
            window_ms = (uint32_t)left_ms;
        }

        err = sdm_attempt(&ndef, window_ms, attempt);
        if (err == HAL_NFC_OK || retry_is_futile(err)) {
            break;
        }
    }

    nfc_ev2_session_reset();

    if (err == HAL_NFC_OK) {
        ESP_LOGI(TAG, "NTAG424 DNA SDM configured successfully for device: %s", device_id);
        return HAL_NFC_OK;
    }

    if (err == HAL_NFC_ERR_TIMEOUT || (!retry_is_futile(err) && remaining_ms(start_us, timeout_ms) <= 0)) {
        err = HAL_NFC_ERR_SDM_TIMEOUT;
    }
    ESP_LOGE(TAG, "SDM config: Failed, last_err=%d", err);
    return err;
}
