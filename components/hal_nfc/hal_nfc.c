/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_nfc.c
 * @brief Public entry points: reader setup, device id check and the SDM configuration lock
 */

#include <string.h>
#include "hal_nfc_priv.h"

static const char *TAG = "hal_nfc";

/** How long an entry point waits for a running conversation to finish */
#define MUTEX_TIMEOUT_MS 1000

nfc_ctx_t g_nfc;

esp_err_t hal_nfc_init(void)
{
    if (g_nfc.mutex != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    g_nfc.mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_nfc.mutex != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create mutex");

    g_nfc.pn532 = nfc_reader_open();
    if (g_nfc.pn532 == NULL) {
        ESP_LOGE(TAG, "Failed to initialize PN532");
        vSemaphoreDelete(g_nfc.mutex);
        g_nfc.mutex = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "NFC HAL initialized successfully");
    return ESP_OK;
}

/* The id goes into the URL path on the tag, so only unreserved characters are allowed. */
bool hal_nfc_device_id_valid(const char *id, size_t len)
{
    if (id == NULL || len == 0 || len > HAL_NFC_DEVICE_ID_MAX_LEN) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = id[i];
        bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

hal_nfc_err_t hal_nfc_ntag424_configure_sdm(const char *device_id, uint32_t timeout_ms)
{
    if (g_nfc.mutex == NULL) {
        return HAL_NFC_ERR_NOT_INIT;
    }
    if (device_id == NULL ||
            !hal_nfc_device_id_valid(device_id, strnlen(device_id, HAL_NFC_DEVICE_ID_MAX_LEN + 1))) {
        return HAL_NFC_ERR_INVALID_PARAM;
    }
    if (xSemaphoreTake(g_nfc.mutex, pdMS_TO_TICKS(MUTEX_TIMEOUT_MS)) != pdTRUE) {
        return HAL_NFC_ERR_TIMEOUT;
    }

    hal_nfc_err_t err = nfc_sdm_configure_locked(device_id, timeout_ms);

    xSemaphoreGive(g_nfc.mutex);
    return err;
}
