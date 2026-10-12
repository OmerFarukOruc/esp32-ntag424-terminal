/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_nfc_priv.h
 * @brief Internal interfaces shared by the hal_nfc source files
 */

#ifndef HAL_NFC_PRIV_H
#define HAL_NFC_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal_nfc.h"
#include "pn532.h"
#include "sdkconfig.h"

/** Largest ISO 7816 APDU the HAL builds or receives, including SW1 SW2 */
#define NFC_APDU_BUF_SIZE           256
/** Largest data field of a native DESFire command: CLA INS P1 P2 Lc ... Le */
#define NFC_DESFIRE_MAX_CMD_DATA    (NFC_APDU_BUF_SIZE - 6)
/** WriteData header: file number, 3 byte offset, 3 byte length */
#define NFC_WRITE_DATA_HEADER_LEN   7
/** Largest NDEF file content one WriteData frame can carry */
#define NFC_MAX_NDEF_LEN            (NFC_DESFIRE_MAX_CMD_DATA - NFC_WRITE_DATA_HEADER_LEN)

typedef struct {
    pn532_t *pn532;             /**< Reader handle, NULL after a failed re-init */
    SemaphoreHandle_t mutex;    /**< Serializes every PN532 conversation; NULL before init */
} nfc_ctx_t;

extern nfc_ctx_t g_nfc;

/* nfc_reader.c: PN532 link recovery and tag detection. Callers hold g_nfc.mutex. */

/** Open the PN532 with the menuconfig UART settings; NULL on failure */
pn532_t *nfc_reader_open(void);

/** Last PN532 error, PN532_ERR_NULL when the reader is closed */
pn532_err_t nfc_reader_last_error(void);

hal_nfc_err_t nfc_reader_err_to_hal(pn532_err_t err);

/**
 * Soft resync, then a hard re-init when it fails. *reinitialised, when not NULL,
 * tells whether the hard re-init ran.
 */
bool nfc_reader_recover(bool *reinitialised);

/** Wait up to timeout_ms (must be > 0) for a tag and activate it */
hal_nfc_err_t nfc_reader_detect_tag(uint32_t timeout_ms);

/** True when the activated tag reports ISO 14443-4 (SAK bit 5 set) */
bool nfc_reader_tag_is_iso_dep(void);

/** Release the activated tag so the next detect starts clean */
void nfc_reader_release_tag(void);

/* nfc_aes.c: AES-128 and CMAC through the PSA API. Outputs are zeroed on failure. */

/** CBC without padding; len must be a non-zero multiple of 16, in-place is not supported */
esp_err_t nfc_aes_cbc_encrypt(const uint8_t key[16], const uint8_t iv[16],
                              const uint8_t *in, size_t len, uint8_t *out);
esp_err_t nfc_aes_cbc_decrypt(const uint8_t key[16], const uint8_t iv[16],
                              const uint8_t *in, size_t len, uint8_t *out);

/** Full 16 byte AES-CMAC */
esp_err_t nfc_aes_cmac(const uint8_t key[16], const uint8_t *data, size_t len, uint8_t mac[16]);

/* desfire_ev2.c: ISO 7816 select and DESFire EV2 secure messaging */

/** ISO SELECT of the NDEF application by DF name */
hal_nfc_err_t nfc_select_ndef_app(void);

/** AuthenticateEV2First with an AES-128 key; fills the session on success */
hal_nfc_err_t nfc_ev2_authenticate(uint8_t key_no, const uint8_t key[16]);

/** WriteData (plain comm mode) of len <= NFC_MAX_NDEF_LEN bytes at offset 0 */
hal_nfc_err_t nfc_ev2_write_file(uint8_t file_no, const uint8_t *data, size_t len);

/** ChangeFileSettings in CommMode.Full; len is at most 31 bytes */
hal_nfc_err_t nfc_ev2_change_file_settings(uint8_t file_no, const uint8_t *settings, size_t len);

/** Forget the session keys */
void nfc_ev2_session_reset(void);

/* ntag424_sdm.c */

/** Run the SDM configuration; the caller holds g_nfc.mutex */
hal_nfc_err_t nfc_sdm_configure_locked(const char *device_id, uint32_t timeout_ms);

#endif /* HAL_NFC_PRIV_H */
