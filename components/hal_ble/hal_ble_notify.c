/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "hal_ble_notify.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include <stdatomic.h>
#include <string.h>

static const char *TAG = "hal_ble";

/* Chosen, not from a source: a scan entry whose 32-byte SSID is all \u00XX escapes is about 240 bytes. */
#define NOTIFY_BUF_SIZE 256
/* Chosen, not from a source: NimBLE returns BLE_HS_ENOMEM while its buffer pool is empty. */
#define NOTIFY_SEND_ATTEMPTS 3
#define NOTIFY_RETRY_DELAY_MS 20
/* Chosen, not from a source: the application has at most one reply in flight per request type, and 250 ms bounds
 * how long a send blocks the state task once the host queue took it. */
#define NOTIFY_JOBS 4
#define NOTIFY_WAIT_MS 250
/* ATT notification header: 1 byte opcode and 2 bytes handle (Bluetooth Core Spec, Vol 3, Part F, 3.4.7.1). */
#define ATT_NOTIFY_OVERHEAD 3

typedef struct {
    struct ble_npl_event event;
    StaticSemaphore_t done_buf;
    SemaphoreHandle_t done;
    atomic_int refs;
    atomic_bool cancelled;
    notify_chr_t *chr;
    hal_ble_session_t session;
    size_t len;
    esp_err_t result;
    char text[NOTIFY_BUF_SIZE];
} notify_job_t;

enum { JOB_FREE = 0, JOB_HELD_BY_PRODUCER_AND_HOST = 2 };

static atomic_bool s_accepting;
static notify_job_t s_jobs[NOTIFY_JOBS];

static esp_err_t notify_on_host(const notify_job_t *job)
{
    uint16_t conn_handle;
    ESP_RETURN_ON_FALSE(ble_host_conn_of(job->session, &conn_handle), ESP_ERR_INVALID_STATE, TAG,
                        "The client that sent the request is gone");
    ESP_RETURN_ON_FALSE(job->chr->subscribed, ESP_ERR_INVALID_STATE, TAG,
                        "Client has not enabled notifications on this characteristic");
    uint16_t mtu = ble_att_mtu(conn_handle);
    size_t max_len = mtu > ATT_NOTIFY_OVERHEAD ? mtu - ATT_NOTIFY_OVERHEAD : 0;
    ESP_RETURN_ON_FALSE(job->len <= max_len, ESP_ERR_INVALID_SIZE, TAG,
                        "Notification of %zu bytes exceeds ATT MTU - 3 (%zu bytes)", job->len, max_len);

    struct os_mbuf *om = ble_hs_mbuf_from_flat(job->text, job->len);
    ESP_RETURN_ON_FALSE(om != NULL, ESP_ERR_NO_MEM, TAG, "No mbuf for notification");
    int rc = ble_gatts_notify_custom(conn_handle, job->chr->val_handle, om);
    ESP_RETURN_ON_FALSE(rc == 0, nimble_to_esp(rc), TAG, "Failed to send notification: %d", rc);
    return ESP_OK;
}

static void notify_job_run(struct ble_npl_event *ev)
{
    notify_job_t *job = ble_npl_event_get_arg(ev);
    if (!atomic_load(&job->cancelled)) {
        job->result = notify_on_host(job);
        xSemaphoreGive(job->done);
    }
    atomic_fetch_sub(&job->refs, 1);
}

static notify_job_t *notify_job_claim(void)
{
    for (size_t i = 0; i < NOTIFY_JOBS; i++) {
        int unused = JOB_FREE;
        if (atomic_compare_exchange_strong(&s_jobs[i].refs, &unused, JOB_HELD_BY_PRODUCER_AND_HOST)) {
            xSemaphoreTake(s_jobs[i].done, 0);
            atomic_store(&s_jobs[i].cancelled, false);
            return &s_jobs[i];
        }
    }
    return NULL;
}

static esp_err_t notify_once(notify_chr_t *chr, hal_ble_session_t session, const char *text, size_t len,
                             int64_t *deadline_us)
{
    ESP_RETURN_ON_FALSE(atomic_load(&s_accepting), ESP_ERR_INVALID_STATE, TAG, "BLE is stopping");
    notify_job_t *job = notify_job_claim();
    ESP_RETURN_ON_FALSE(job != NULL, ESP_ERR_NO_MEM, TAG, "Every notification job is in use");

    job->chr = chr;
    job->session = session;
    job->len = len;
    memcpy(job->text, text, len);
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &job->event);
    if (*deadline_us == 0) {
        *deadline_us = esp_timer_get_time() + NOTIFY_WAIT_MS * 1000;
    }

    int64_t left_us = *deadline_us - esp_timer_get_time();
    TickType_t wait = left_us > 0 ? pdMS_TO_TICKS((left_us + 999) / 1000) : 0;
    esp_err_t ret = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(job->done, wait) == pdTRUE) {
        ret = job->result;
    } else {
        atomic_store(&job->cancelled, true);
        ESP_LOGW(TAG, "The host task did not send the notification within %d ms", NOTIFY_WAIT_MS);
    }
    atomic_fetch_sub(&job->refs, 1);
    return ret;
}

void ble_notify_init(void)
{
    for (size_t i = 0; i < NOTIFY_JOBS; i++) {
        ble_npl_event_init(&s_jobs[i].event, notify_job_run, &s_jobs[i]);
        s_jobs[i].done = xSemaphoreCreateBinaryStatic(&s_jobs[i].done_buf);
        atomic_store(&s_jobs[i].refs, JOB_FREE);
    }
}

void ble_notify_set_accepting(bool accepting)
{
    atomic_store(&s_accepting, accepting);
}

void ble_notify_deinit_events(void)
{
    for (size_t i = 0; i < NOTIFY_JOBS; i++) {
        ble_npl_event_deinit(&s_jobs[i].event);
    }
}

esp_err_t ble_notify_json(notify_chr_t *chr, hal_ble_session_t session, cJSON *json, bool built)
{
    char text[NOTIFY_BUF_SIZE];
    esp_err_t ret = ESP_OK;

    ESP_GOTO_ON_FALSE(built, ESP_ERR_NO_MEM, out, TAG, "Failed to build notification");
    ESP_GOTO_ON_FALSE(cJSON_PrintPreallocated(json, text, sizeof(text), false), ESP_ERR_INVALID_SIZE, out, TAG,
                      "Notification does not fit %u bytes", (unsigned)sizeof(text));

    int64_t deadline_us = 0;
    for (int attempt = 1;; attempt++) {
        ret = notify_once(chr, session, text, strlen(text), &deadline_us);
        if (ret != ESP_ERR_NO_MEM || attempt == NOTIFY_SEND_ATTEMPTS ||
                esp_timer_get_time() + NOTIFY_RETRY_DELAY_MS * 1000 >= deadline_us) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }

out:
    cJSON_Delete(json);
    return ret;
}
