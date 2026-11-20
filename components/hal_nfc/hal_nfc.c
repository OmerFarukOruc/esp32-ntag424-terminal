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
#include "esp_timer.h"
#include "hal_nfc_priv.h"
#include "freertos/task.h"
#include "pn532_io_deadline.h"

static const char *TAG = "hal_nfc";

/** How long an entry point waits for a running conversation to finish */
#define MUTEX_TIMEOUT_MS 1000
/* Chosen, not from a source: pn532_init() needs a few hundred milliseconds when the reader answers. */
#define INIT_IO_LIMIT_MS 5000
/* Chosen, not from a source: InRelease is one short PN532 command. */
#define CLEANUP_IO_MS 200

nfc_ctx_t g_nfc;

static int64_t deadline_after_ms(uint32_t ms)
{
    return esp_timer_get_time() + (int64_t)ms * 1000;
}

static int64_t deadline_left_us(void)
{
    int64_t left_us = g_nfc.deadline_us - esp_timer_get_time();
    return left_us > 0 ? left_us : 0;
}

static void deadline_begin(int64_t deadline_us)
{
    g_nfc.deadline_us = deadline_us;
    pn532_io_deadline_set(NFC_UART_NUM, deadline_us);
}

static void deadline_end(void)
{
    pn532_io_deadline_clear();
    g_nfc.deadline_us = 0;
}

bool nfc_deadline_passed(void)
{
    return deadline_left_us() == 0;
}

int64_t nfc_deadline_left_ms(void)
{
    return deadline_left_us() / 1000;
}

void nfc_delay_ms(uint32_t ms)
{
    int64_t cap_ms = (deadline_left_us() + 999) / 1000;
    if (cap_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(ms < cap_ms ? ms : (uint32_t)cap_ms));
    }
}

void nfc_deadline_allow_cleanup(void)
{
    int64_t cleanup_us = deadline_after_ms(CLEANUP_IO_MS);
    pn532_io_deadline_set(NFC_UART_NUM, cleanup_us > g_nfc.deadline_us ? cleanup_us : g_nfc.deadline_us);
}

esp_err_t hal_nfc_init(void)
{
    if (g_nfc.mutex != NULL) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    g_nfc.mutex = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(g_nfc.mutex != NULL, ESP_ERR_NO_MEM, TAG, "Failed to create mutex");

    xSemaphoreTake(g_nfc.mutex, portMAX_DELAY);
    deadline_begin(deadline_after_ms(INIT_IO_LIMIT_MS));
    g_nfc.pn532 = nfc_reader_open();
    deadline_end();
    xSemaphoreGive(g_nfc.mutex);
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
    if (device_id == NULL || timeout_ms == 0 ||
            !hal_nfc_device_id_valid(device_id, strnlen(device_id, HAL_NFC_DEVICE_ID_MAX_LEN + 1))) {
        return HAL_NFC_ERR_INVALID_PARAM;
    }
    const int64_t deadline_us = deadline_after_ms(timeout_ms);
    uint32_t wait_ms = timeout_ms < MUTEX_TIMEOUT_MS ? timeout_ms : MUTEX_TIMEOUT_MS;
    if (xSemaphoreTake(g_nfc.mutex, pdMS_TO_TICKS(wait_ms)) != pdTRUE) {
        return HAL_NFC_ERR_TIMEOUT;
    }

    deadline_begin(deadline_us);
    hal_nfc_err_t err = nfc_sdm_configure_locked(device_id);
    deadline_end();

    xSemaphoreGive(g_nfc.mutex);
    return err;
}
