/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file nfc_reader.c
 * @brief PN532 link recovery and tag detection
 *
 * A failed APDU exchange can leave the PN532 in a protocol-error state where
 * every later command fails, so the recovery ladder is soft resync (drain,
 * wake-up, ping) first and a full UART teardown plus re-init second.
 */

#include "hal_nfc_priv.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "freertos/task.h"

static const char *TAG = "hal_nfc";

#define NFC_UART_NUM    ((uart_port_t)CONFIG_HAL_NFC_UART_NUM)
/** PN532 library baud code for 115200 bps */
#define PN532_BAUD_CODE 4
/** PN532 port 3 output bits in use */
#define PN532_P3_BITS   0

#define DETECT_POLL_DELAY_MS            80
#define DETECT_MISMATCH_LOG_INTERVAL    16
#define DETECT_MISMATCH_RECOVER_STREAK  4
#define DETECT_MISMATCH_MAX_REINITS     3
#define DETECT_TRANSIENT_RECOVER_ERRORS 8
#define DETECT_FATAL_RECOVER_ERRORS     5
#define DETECT_MAX_REINITS              2

/** SAK bit 5: the tag implements ISO 14443-4 (NTAG424 DNA reports 0x20) */
#define SAK_ISO_DEP     0x20

pn532_t *nfc_reader_open(void)
{
    ESP_LOGI(TAG, "Initializing PN532 via UART%d (TX=%d, RX=%d)", CONFIG_HAL_NFC_UART_NUM,
             CONFIG_HAL_NFC_TX_GPIO, CONFIG_HAL_NFC_RX_GPIO);
    return pn532_init(CONFIG_HAL_NFC_UART_NUM, PN532_BAUD_CODE,
                      CONFIG_HAL_NFC_TX_GPIO, CONFIG_HAL_NFC_RX_GPIO, PN532_P3_BITS);
}

pn532_err_t nfc_reader_last_error(void)
{
    return g_nfc.pn532 == NULL ? PN532_ERR_NULL : pn532_lasterr(g_nfc.pn532);
}

static void log_stage(const char *stage)
{
    pn532_err_t err = nfc_reader_last_error();
    ESP_LOGI(TAG, "PN532 stage=%s last_err=%s(%d)", stage, pn532_err_to_name(err), err);
}

hal_nfc_err_t nfc_reader_err_to_hal(pn532_err_t err)
{
    switch (err) {
    case PN532_ERR_OK:
        return HAL_NFC_OK;
    case PN532_ERR_TIMEOUT:
    case PN532_ERR_TIMEOUTACK:
    case PN532_ERR_STATUS_TIMEOUT:
    case PN532_ERR_STATUS_PROTOCOL:
        return HAL_NFC_ERR_TIMEOUT;
    case PN532_ERR_STATUS_MIFAREAUTH:
        return HAL_NFC_ERR_AUTH;
    case PN532_ERR_NULL:
        return HAL_NFC_ERR_NOT_INIT;
    default:
        return HAL_NFC_ERR_COMM;
    }
}

static void drain_pending(void)
{
    if (g_nfc.pn532 == NULL) {
        return;
    }

    uint8_t dummy[64];
    int drained = pn532_rx(g_nfc.pn532, 0, NULL, sizeof(dummy), dummy, 40);
    if (drained >= 0) {
        ESP_LOGI(TAG, "PN532 drained stale pending response (%d bytes)", drained);
    }
}

static void drain_uart_fifo(void)
{
    if (uart_is_driver_installed(NFC_UART_NUM)) {
        uart_flush_input(NFC_UART_NUM);
    }
}

static void send_wakeup(void)
{
    if (!uart_is_driver_installed(NFC_UART_NUM)) {
        return;
    }

    /* HSU wake-up preamble: long run of 0x00 followed by 0x55 0x55 0x55 */
    uint8_t wake_buf[30] = {0};
    wake_buf[27] = 0x55;
    wake_buf[28] = 0x55;
    wake_buf[29] = 0x55;

    uart_flush_input(NFC_UART_NUM);
    (void)uart_write_bytes(NFC_UART_NUM, (const char *)wake_buf, sizeof(wake_buf));
    (void)uart_wait_tx_done(NFC_UART_NUM, pdMS_TO_TICKS(100));
}

/** GetFirmwareVersion round trip */
static bool ping(void)
{
    if (g_nfc.pn532 == NULL) {
        return false;
    }

    uint8_t fw_buf[8] = {0};
    int tx = pn532_tx(g_nfc.pn532, 0x02, 0, NULL, 0, NULL);
    if (tx < 0 && pn532_lasterr(g_nfc.pn532) == PN532_ERR_CMDPENDING) {
        drain_pending();
        tx = pn532_tx(g_nfc.pn532, 0x02, 0, NULL, 0, NULL);
    }
    if (tx < 0) {
        log_stage("ping_tx_failed");
        return false;
    }

    if (pn532_rx(g_nfc.pn532, 0, NULL, sizeof(fw_buf), fw_buf, 100) < 4) {
        log_stage("ping_rx_failed");
        return false;
    }
    return true;
}

static bool soft_resync(void)
{
    if (g_nfc.pn532 == NULL) {
        return false;
    }

    log_stage("soft_resync_begin");
    drain_pending();
    drain_uart_fifo();

    for (int pass = 0; pass < 2; pass++) {
        send_wakeup();
        vTaskDelay(pdMS_TO_TICKS(pass == 0 ? 40 : 80));
        if (ping()) {
            log_stage(pass == 0 ? "soft_resync_ok" : "soft_resync_ok_second_try");
            return true;
        }
    }

    drain_uart_fifo();
    log_stage("soft_resync_failed");
    return false;
}

/**
 * pn532_end() frees only the handle and leaves the UART driver installed, and
 * pn532_init() skips the driver install when one exists, so stale UART state
 * would survive a plain re-init. Delete the driver first.
 */
static bool reinit(void)
{
    log_stage("reinit_begin");

    if (g_nfc.pn532 != NULL) {
        pn532_end(g_nfc.pn532);
        g_nfc.pn532 = NULL;
    }
    if (uart_is_driver_installed(NFC_UART_NUM)) {
        uart_driver_delete(NFC_UART_NUM);
    }

    for (int pass = 0; pass < 2 && g_nfc.pn532 == NULL; pass++) {
        if (pass > 0) {
            ESP_LOGW(TAG, "PN532 re-init first attempt failed, retrying once");
            if (uart_is_driver_installed(NFC_UART_NUM)) {
                uart_driver_delete(NFC_UART_NUM);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(pass == 0 ? 200 : 250));
        g_nfc.pn532 = nfc_reader_open();
    }
    if (g_nfc.pn532 == NULL) {
        log_stage("reinit_init_failed");
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(100));
    if (!ping()) {
        log_stage("reinit_ping_failed");
        pn532_end(g_nfc.pn532);
        g_nfc.pn532 = NULL;
        return false;
    }
    log_stage("reinit_ok");
    return true;
}

bool nfc_reader_recover(bool *reinitialised)
{
    if (reinitialised != NULL) {
        *reinitialised = false;
    }
    if (soft_resync()) {
        return true;
    }
    ESP_LOGW(TAG, "Soft resync failed, hard re-initializing");
    if (reinitialised != NULL) {
        *reinitialised = true;
    }
    return reinit();
}

void nfc_reader_release_tag(void)
{
    (void)pn532_release(g_nfc.pn532, 0);
}

bool nfc_reader_tag_is_iso_dep(void)
{
    return (pn532_sak(g_nfc.pn532) & SAK_ISO_DEP) != 0;
}

typedef struct {
    int consec_errors;
    int mismatch_streak;
    int reinits;
} detect_state_t;

/** Soft resync, then a counted hard re-init; false when the PN532 stays dead */
static bool detect_recover(detect_state_t *st, const char *why, uint32_t settle_ms)
{
    ESP_LOGW(TAG, "detect: %s, recovering", why);
    bool reinitialised;
    bool recovered = nfc_reader_recover(&reinitialised);
    if (reinitialised) {
        st->reinits++;
    }
    if (!recovered) {
        ESP_LOGE(TAG, "detect: PN532 recovery failed");
        return false;
    }

    st->mismatch_streak = 0;
    st->consec_errors = 0;
    vTaskDelay(pdMS_TO_TICKS(settle_ms));
    return true;
}

hal_nfc_err_t nfc_reader_detect_tag(uint32_t timeout_ms)
{
    ESP_RETURN_ON_FALSE(timeout_ms > 0, HAL_NFC_ERR_INVALID_PARAM, TAG, "Detect needs a timeout");

    const int64_t start_us = esp_timer_get_time();
    const int64_t timeout_us = (int64_t)timeout_ms * 1000;
    detect_state_t st = {0};
    int attempt = 0;

    for (;;) {
        if (esp_timer_get_time() - start_us >= timeout_us) {
            ESP_LOGW(TAG, "Tag detection timed out after %d attempts", attempt);
            return HAL_NFC_ERR_NO_TAG;
        }

        int cards = pn532_Cards(g_nfc.pn532);
        attempt++;

        if (attempt <= 5 || attempt % 10 == 0) {
            pn532_err_t pn_err = nfc_reader_last_error();
            ESP_LOGI(TAG, "detect attempt %d: cards=%d, pn532_err=%s(%d)",
                     attempt, cards, pn532_err_to_name(pn_err), pn_err);
        }

        if (cards > 0) {
            break;
        }

        if (cards == 0) {
            st.consec_errors = 0;
            st.mismatch_streak = 0;
            vTaskDelay(pdMS_TO_TICKS(DETECT_POLL_DELAY_MS));
            continue;
        }

        st.consec_errors++;
        pn532_err_t pn_err = nfc_reader_last_error();
        bool transient = pn_err == PN532_ERR_CMDMISMATCH || pn_err == PN532_ERR_TIMEOUT ||
                         pn_err == PN532_ERR_TIMEOUTACK || pn_err == PN532_ERR_NOTPENDING;

        if (!transient) {
            st.mismatch_streak = 0;
            if (st.consec_errors >= DETECT_FATAL_RECOVER_ERRORS && st.reinits < DETECT_MAX_REINITS) {
                if (!detect_recover(&st, pn532_err_to_name(pn_err), 150)) {
                    return HAL_NFC_ERR_PN532_UNRESPONSIVE;
                }
            } else {
                drain_pending();
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            vTaskDelay(pdMS_TO_TICKS(DETECT_POLL_DELAY_MS));
            continue;
        }

        if (pn_err == PN532_ERR_CMDMISMATCH) {
            st.mismatch_streak++;
            if (st.mismatch_streak == 1 || st.mismatch_streak % DETECT_MISMATCH_LOG_INTERVAL == 0) {
                ESP_LOGW(TAG, "detect attempt %d: CMDMISMATCH indicates PN532 state desync (streak=%d)",
                         attempt, st.mismatch_streak);
            }
        } else if (pn_err == PN532_ERR_NOTPENDING) {
            st.mismatch_streak = 0;
        }

        drain_pending();
        drain_uart_fifo();

        if (st.mismatch_streak >= DETECT_MISMATCH_RECOVER_STREAK &&
                st.reinits < DETECT_MISMATCH_MAX_REINITS) {
            if (!detect_recover(&st, "CMDMISMATCH streak", 120)) {
                return HAL_NFC_ERR_PN532_UNRESPONSIVE;
            }
        } else if (st.consec_errors >= DETECT_TRANSIENT_RECOVER_ERRORS &&
                   st.reinits < DETECT_MAX_REINITS) {
            if (!detect_recover(&st, "prolonged transient errors", 120)) {
                return HAL_NFC_ERR_PN532_UNRESPONSIVE;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(DETECT_POLL_DELAY_MS));
        }
    }

    const char *type = pn532_type(g_nfc.pn532);
    ESP_LOGI(TAG, "Tag detected: ATQA=0x%04X SAK=0x%02X type=%s (%lld ms)",
             pn532_atqa(g_nfc.pn532), pn532_sak(g_nfc.pn532), type != NULL ? type : "Unknown",
             (long long)((esp_timer_get_time() - start_us) / 1000));
    return HAL_NFC_OK;
}
