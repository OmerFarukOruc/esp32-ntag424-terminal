/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_nfc.h
 * @brief NTAG424 DNA provisioning through a PN532 reader (UART, HSU mode)
 *
 * The component configures Secure Dynamic Messaging (SDM) on an NTAG424 DNA
 * tag. Pins, the UART port and the SDM base URL come from menuconfig (HAL NFC).
 *
 * Key limitation: the component authenticates with the factory all-zero
 * AES-128 key 0 and leaves the SDM keys at their factory all-zero value. It
 * never calls ChangeKey, because an interrupted key change can lock a tag for
 * good. Anyone who knows this firmware can decrypt picc_data and compute a
 * valid cmac, so SDM shows only that a tap happened, not that a genuine tag
 * produced it. A tag with a changed key 0 fails with HAL_NFC_ERR_AUTH.
 *
 * Built with the GPL-3.0 revk/ESP32-PN532 library, so a released binary is a
 * GPL-3.0 combined work.
 */

#ifndef HAL_NFC_H
#define HAL_NFC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest device id hal_nfc_ntag424_configure_sdm() accepts, in characters */
#define HAL_NFC_DEVICE_ID_MAX_LEN   31

/**
 * @brief NFC HAL error codes
 */
typedef enum {
    HAL_NFC_OK = 0,                 /**< Operation successful */
    HAL_NFC_ERR_NOT_INIT,           /**< HAL not initialized */
    HAL_NFC_ERR_NO_TAG,             /**< No tag detected */
    HAL_NFC_ERR_COMM,               /**< Communication, framing or crypto error */
    HAL_NFC_ERR_AUTH,               /**< Tag authentication failed */
    HAL_NFC_ERR_TIMEOUT,            /**< The reader is busy with another call, or the PN532 timed out or broke protocol */
    HAL_NFC_ERR_INVALID_PARAM,      /**< Invalid device id */
    HAL_NFC_ERR_NOT_SUPPORTED,      /**< Tag lacks a required capability */
    HAL_NFC_ERR_PN532_UNRESPONSIVE, /**< PN532 did not answer after recovery */
    HAL_NFC_ERR_TAG_NOT_DESFIRE,    /**< Detected tag is not ISO 14443-4 (NTAG424 DNA) */
    HAL_NFC_ERR_SDM_TIMEOUT,        /**< SDM configuration timed out before completion */
} hal_nfc_err_t;

/**
 * @brief Initialize the PN532 reader
 *
 * Uses the UART port and pins from menuconfig. Gives up after 5 seconds of
 * PN532 traffic that does not complete the setup.
 *
 * @return ESP_OK on success, ESP_ERR_NO_MEM or ESP_FAIL otherwise
 */
esp_err_t hal_nfc_init(void);

/**
 * @brief Configure NTAG424 DNA Secure Dynamic Messaging (SDM)
 *
 * One-shot onboarding operation. Waits for a tag, authenticates with key 0,
 * writes an NDEF URL record with SDM placeholders, and enables SDM on file 02
 * so each tap mirrors the encrypted UID and read counter plus a CMAC.
 *
 * After configuration, each phone tap reads
 * {CONFIG_HAL_NFC_SDM_BASE_URL}/{device_id}?picc_data={32hex}&cmac={16hex}
 *
 * The call blocks the calling task until the tag is configured, the attempts
 * run out, or timeout_ms passes. timeout_ms counts from the call and covers the
 * wait for the reader, PN532 recovery and every tag command. No tag command
 * starts after it passes; releasing the tag may take up to 200 ms more.
 *
 * @param[in] device_id Device identifier for the URL path: 1 to
 *            HAL_NFC_DEVICE_ID_MAX_LEN characters from [A-Za-z0-9_-]
 * @param[in] timeout_ms Overall timeout in milliseconds, greater than 0
 * @return HAL_NFC_OK on success, or
 *         HAL_NFC_ERR_NOT_INIT before hal_nfc_init(),
 *         HAL_NFC_ERR_INVALID_PARAM for a bad device_id or a timeout_ms of 0,
 *         HAL_NFC_ERR_TIMEOUT when another call holds the reader for over a second
 *         or for longer than timeout_ms,
 *         HAL_NFC_ERR_PN532_UNRESPONSIVE when the PN532 does not answer after recovery,
 *         HAL_NFC_ERR_NO_TAG when no tag appears and the attempts run out,
 *         HAL_NFC_ERR_SDM_TIMEOUT when timeout_ms passes first; when it passes
 *         after WriteData, the tag holds the new URL with SDM still off,
 *         HAL_NFC_ERR_TAG_NOT_DESFIRE for a tag without ISO 14443-4,
 *         HAL_NFC_ERR_NOT_SUPPORTED for a tag without the NDEF application, or
 *         whose file 02 key 0 may not reconfigure or that is too small; the tag is
 *         left unchanged,
 *         HAL_NFC_ERR_AUTH when key 0 is not the factory key,
 *         HAL_NFC_ERR_COMM for any other failure after the attempts run out
 *
 * @note Place the tag on the PN532 antenna before or while calling
 * @note See the key limitation in the file description
 */
hal_nfc_err_t hal_nfc_ntag424_configure_sdm(const char *device_id, uint32_t timeout_ms);

/**
 * @brief Check a device id against the rule hal_nfc_ntag424_configure_sdm() applies
 *
 * @param[in] id Candidate id, not necessarily NUL-terminated
 * @param[in] len Number of characters in id
 * @return true for 1 to HAL_NFC_DEVICE_ID_MAX_LEN characters from [A-Za-z0-9_-]
 */
bool hal_nfc_device_id_valid(const char *id, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* HAL_NFC_H */
