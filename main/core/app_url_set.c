/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>

#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"

#include "hal_ble.h"
#include "hal_diagnostics.h"
#include "app_state_priv.h"

static const char *TAG = "app_url_set";

#define NVS_KEY_DEVICE_ID   "dev_id"
#define NFC_TASK_STACK      8192
#define NFC_TASK_PRIORITY   6
#define NFC_TIMEOUT_MS      30000

_Static_assert(HAL_NFC_DEVICE_ID_MAX_LEN < HAL_BLE_DEVICE_ID_MAX,
               "a device id accepted by hal_nfc must fit hal_ble");

/* Result codes the phone client parses. */
typedef enum {
    URL_SET_OK = 0,
    URL_SET_NO_TAG = 1001,
    URL_SET_UNSUPPORTED_TAG = 1002,
    URL_SET_PN532_UNRESPONSIVE = 1003,
    URL_SET_AUTH_FAILED = 1004,
    URL_SET_SDM_TIMEOUT = 1005,
    URL_SET_INVALID_DEVICE_ID = 1006,
    URL_SET_UNSUPPORTED_CAPABILITY = 1007,
    URL_SET_FAILED = 1099,
} url_set_code_t;

typedef struct {
    url_set_code_t code;
    const char *message;
} url_set_result_t;

#define RESULT(c, m) ((url_set_result_t){ .code = (c), .message = (m) })

/* The state task fills the job before it starts the tag task and reads it again when the task reports.
 * It starts no second write in between, because it stays in WRITING_TAG. */
static struct {
    char device_id[HAL_NFC_DEVICE_ID_MAX_LEN + 1];
    uint32_t attempt_id;
    int64_t started_us;
    bool ble_connected_start;
} s_job;

static url_set_result_t map_result(hal_nfc_err_t nfc_err)
{
    switch (nfc_err) {
    case HAL_NFC_OK:
        return RESULT(URL_SET_OK, "sdm_configured");
    case HAL_NFC_ERR_NO_TAG:
        return RESULT(URL_SET_NO_TAG, "tag_not_detected");
    case HAL_NFC_ERR_TAG_NOT_DESFIRE:
        return RESULT(URL_SET_UNSUPPORTED_TAG, "unsupported_tag");
    case HAL_NFC_ERR_PN532_UNRESPONSIVE:
        return RESULT(URL_SET_PN532_UNRESPONSIVE, "pn532_unresponsive");
    case HAL_NFC_ERR_AUTH:
        return RESULT(URL_SET_AUTH_FAILED, "tag_auth_failed");
    case HAL_NFC_ERR_SDM_TIMEOUT:
    case HAL_NFC_ERR_TIMEOUT:
        return RESULT(URL_SET_SDM_TIMEOUT, "sdm_timeout");
    case HAL_NFC_ERR_INVALID_PARAM:
        return RESULT(URL_SET_INVALID_DEVICE_ID, "invalid_device_id");
    case HAL_NFC_ERR_NOT_SUPPORTED:
        return RESULT(URL_SET_UNSUPPORTED_CAPABILITY, "unsupported_tag_capability");
    default:
        return RESULT(URL_SET_FAILED, "sdm_config_failed");
    }
}

static void send_result(url_set_result_t result)
{
    esp_err_t err = hal_ble_send_url_set_result(result.code == URL_SET_OK, result.code, result.message);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Failed to send the URL_SET result over BLE: %s", esp_err_to_name(err));
    }
}

static esp_err_t save_device_id(const char *id)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(APP_NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "NVS open failed");
    esp_err_t err = nvs_set_str(handle, NVS_KEY_DEVICE_ID, id);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

esp_err_t app_device_id_publish(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(APP_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored device id (namespace not created yet)");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS open failed");

    char id[HAL_BLE_DEVICE_ID_MAX];
    size_t len = sizeof(id);
    err = nvs_get_str(handle, NVS_KEY_DEVICE_ID, id, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No stored device id");
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "Reading the device id failed");

    ESP_LOGI(TAG, "Loaded device id: %s", id);
    return hal_ble_set_device_id(id);
}

static void nfc_config_task(void *arg)
{
    (void)arg;
    hal_nfc_err_t nfc_err = hal_nfc_ntag424_configure_sdm(s_job.device_id, NFC_TIMEOUT_MS);

    /* The state task waits for this event, so it waits for a queue slot too. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(app_post_event_wait(APP_EVENT_URL_SET_DONE, &nfc_err, sizeof(nfc_err)));
    vTaskDelete(NULL);
}

void app_url_set_reject(const char *reason)
{
    ESP_LOGW(TAG, "URL_SET rejected: %s", reason);
    send_result(RESULT(URL_SET_FAILED, reason));
}

bool app_url_set_start(const char *device_id, size_t len)
{
    if (!hal_nfc_device_id_valid(device_id, len)) {
        ESP_LOGW(TAG, "URL_SET rejected: invalid device id (%u bytes)", (unsigned)len);
        send_result(map_result(HAL_NFC_ERR_INVALID_PARAM));
        return false;
    }

    memcpy(s_job.device_id, device_id, len);
    s_job.device_id[len] = '\0';
    s_job.attempt_id++;
    s_job.started_us = esp_timer_get_time();
    s_job.ble_connected_start = hal_ble_is_connected();

    if (xTaskCreate(nfc_config_task, "nfc_cfg", NFC_TASK_STACK, NULL, NFC_TASK_PRIORITY, NULL) != pdPASS) {
        ESP_LOGE(TAG, "URL_SET attempt #%lu: creating the tag task failed", (unsigned long)s_job.attempt_id);
        send_result(RESULT(URL_SET_FAILED, "nfc_config_task_create_failed"));
        return false;
    }
    ESP_LOGI(TAG, "URL_SET attempt #%lu start: device_id=%s", (unsigned long)s_job.attempt_id, s_job.device_id);
    return true;
}

bool app_url_set_finish(hal_nfc_err_t nfc_err)
{
    bool success = (nfc_err == HAL_NFC_OK);
    url_set_result_t result = map_result(nfc_err);
    uint32_t duration_ms = (uint32_t)((esp_timer_get_time() - s_job.started_us) / 1000);
    bool ble_connected_end = hal_ble_is_connected();

    if (success) {
        /* The id belongs to the tag, so it is stored only after the tag accepted it. */
        esp_err_t err = save_device_id(s_job.device_id);
        if (err == ESP_OK) {
            err = hal_ble_set_device_id(s_job.device_id);
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Storing device id %s failed: %s", s_job.device_id, esp_err_to_name(err));
        }
    }

    if (ble_connected_end) {
        send_result(result);
    } else {
        ESP_LOGW(TAG, "URL_SET attempt #%lu: BLE disconnected before the result, not sent",
                 (unsigned long)s_job.attempt_id);
    }

    hal_diagnostics_record_url_set_attempt(&(const hal_diagnostics_url_set_t) {
        .attempt_id = s_job.attempt_id,
        .nfc_err = (int32_t)nfc_err,
        .result_code = result.code,
        .ble_connected_start = s_job.ble_connected_start,
        .ble_connected_end = ble_connected_end,
        .duration_ms = duration_ms,
    });
    ESP_LOGI(TAG, "URL_SET attempt #%lu done: %s (code %d, hal_nfc error %d), %lu ms",
             (unsigned long)s_job.attempt_id, result.message, (int)result.code, (int)nfc_err,
             (unsigned long)duration_ms);
    return success;
}
