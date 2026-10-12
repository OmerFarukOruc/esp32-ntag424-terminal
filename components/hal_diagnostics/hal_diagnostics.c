/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file hal_diagnostics.c
 * @brief RTC-backed history of state transitions and tag-write attempts
 */

#include "hal_diagnostics.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include <string.h>

static const char *TAG = "hal_diag";

#define TRANSITION_RING_SIZE    32
#define URL_SET_RING_SIZE       8
#define NVS_NAMESPACE           "diagnostics"
#define NVS_KEY_BOOT_COUNT      "boot_cnt"

typedef struct {
    uint8_t from_state;
    uint8_t to_state;
    uint8_t trigger_event;
    uint8_t padding;
    uint32_t timestamp_ms;
} transition_entry_t;

typedef struct {
    uint32_t attempt_id;
    uint32_t timestamp_ms;
    uint32_t duration_ms;
    int32_t nfc_err;
    int32_t result_code;
    uint8_t ble_connected_start;
    uint8_t ble_connected_end;
} url_set_entry_t;

typedef struct {
    uint8_t next;   /* slot the next record goes to */
    uint8_t count;  /* valid records, at most the ring size */
} ring_pos_t;

/* The magic changes with the layout, so an image from an older build is discarded. */
#define RTC_MAGIC (0xD1A60000u ^ ((uint32_t)sizeof(transition_entry_t) << 12) ^ \
                   ((uint32_t)sizeof(url_set_entry_t) << 8) ^ (TRANSITION_RING_SIZE << 4) ^ URL_SET_RING_SIZE)

static RTC_NOINIT_ATTR transition_entry_t s_rtc_transitions[TRANSITION_RING_SIZE];
static RTC_NOINIT_ATTR ring_pos_t s_rtc_transition_pos;
static RTC_NOINIT_ATTR url_set_entry_t s_rtc_url_sets[URL_SET_RING_SIZE];
static RTC_NOINIT_ATTR ring_pos_t s_rtc_url_set_pos;
static RTC_NOINIT_ATTR uint32_t s_rtc_magic;

static uint32_t s_queue_overflow_count = 0;
static bool s_initialized = false;

static bool ring_valid(const ring_pos_t *pos, uint8_t size)
{
    return pos->next < size && pos->count <= size;
}

static uint8_t ring_push(ring_pos_t *pos, uint8_t size)
{
    uint8_t slot = pos->next;
    pos->next = (slot + 1) % size;
    if (pos->count < size) {
        pos->count++;
    }
    return slot;
}

/* Slot of the i-th oldest record. */
static uint8_t ring_slot(const ring_pos_t *pos, uint8_t size, uint8_t i)
{
    uint8_t start = (pos->count < size) ? 0 : pos->next;
    return (start + i) % size;
}

static uint32_t uptime_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void reset_history(void)
{
    memset(s_rtc_transitions, 0, sizeof(s_rtc_transitions));
    memset(s_rtc_url_sets, 0, sizeof(s_rtc_url_sets));
    memset(&s_rtc_transition_pos, 0, sizeof(s_rtc_transition_pos));
    memset(&s_rtc_url_set_pos, 0, sizeof(s_rtc_url_set_pos));
    s_rtc_magic = RTC_MAGIC;
}

static void log_recovered_history(void)
{
    for (uint8_t i = 0; i < s_rtc_transition_pos.count; i++) {
        const transition_entry_t *e = &s_rtc_transitions[ring_slot(&s_rtc_transition_pos, TRANSITION_RING_SIZE, i)];
        ESP_LOGW(TAG, "Previous run: state %u -> %u (event %u) at %lu ms", e->from_state, e->to_state,
                 e->trigger_event, (unsigned long)e->timestamp_ms);
    }
    for (uint8_t i = 0; i < s_rtc_url_set_pos.count; i++) {
        const url_set_entry_t *e = &s_rtc_url_sets[ring_slot(&s_rtc_url_set_pos, URL_SET_RING_SIZE, i)];
        ESP_LOGW(TAG, "Previous run: tag write #%lu %s nfc_err=%ld result=%ld took %lu ms",
                 (unsigned long)e->attempt_id, (e->nfc_err == 0 && e->result_code == 0) ? "succeeded" : "failed",
                 (long)e->nfc_err, (long)e->result_code, (unsigned long)e->duration_ms);
    }
}

static esp_err_t bump_boot_count(uint32_t *boot_count)
{
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle), TAG, "NVS open failed");

    uint32_t count = 0;
    esp_err_t ret = nvs_get_u32(handle, NVS_KEY_BOOT_COUNT, &count);
    ESP_GOTO_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_NVS_NOT_FOUND, ret, out, TAG, "Boot count read failed");
    ret = ESP_OK;

    count++;
    ESP_GOTO_ON_ERROR(nvs_set_u32(handle, NVS_KEY_BOOT_COUNT, count), out, TAG, "Boot count write failed");
    ESP_GOTO_ON_ERROR(nvs_commit(handle), out, TAG, "Boot count commit failed");
    *boot_count = count;
out:
    nvs_close(handle);
    return ret;
}

esp_err_t hal_diagnostics_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_reset_reason_t reason = esp_reset_reason();

    /* RTC_NOINIT memory holds undefined data after power-on, so the magic alone is not proof. */
    if (reason == ESP_RST_POWERON || s_rtc_magic != RTC_MAGIC ||
            !ring_valid(&s_rtc_transition_pos, TRANSITION_RING_SIZE) ||
            !ring_valid(&s_rtc_url_set_pos, URL_SET_RING_SIZE)) {
        reset_history();
        ESP_LOGI(TAG, "RTC history initialized");
    } else {
        ESP_LOGI(TAG, "RTC history recovered (%u transitions, %u tag-write attempts)",
                 s_rtc_transition_pos.count, s_rtc_url_set_pos.count);
        if (reason == ESP_RST_PANIC || reason == ESP_RST_INT_WDT || reason == ESP_RST_TASK_WDT ||
                reason == ESP_RST_WDT || reason == ESP_RST_BROWNOUT) {
            log_recovered_history();
        }
    }

    uint32_t boot_count = 0;
    esp_err_t err = bump_boot_count(&boot_count);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Boot count unavailable: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "Boot #%lu, reset reason: %d", (unsigned long)boot_count, (int)reason);

    s_initialized = true;
    return ESP_OK;
}

void hal_diagnostics_record_transition(uint8_t from_state, uint8_t to_state, uint8_t event)
{
    if (!s_initialized) {
        return;
    }

    transition_entry_t *entry = &s_rtc_transitions[ring_push(&s_rtc_transition_pos, TRANSITION_RING_SIZE)];
    *entry = (transition_entry_t) {
        .from_state = from_state,
        .to_state = to_state,
        .trigger_event = event,
        .timestamp_ms = uptime_ms(),
    };
}

void hal_diagnostics_record_queue_overflow(void)
{
    uint32_t total = __atomic_add_fetch(&s_queue_overflow_count, 1, __ATOMIC_RELAXED);

    /* Log on powers of two so a stuck queue cannot flood the console. */
    if ((total & (total - 1)) == 0) {
        ESP_LOGW(TAG, "Event queue overflow (%lu so far)", (unsigned long)total);
    }
}

void hal_diagnostics_record_url_set_attempt(const hal_diagnostics_url_set_t *attempt)
{
    if (!s_initialized) {
        return;
    }

    url_set_entry_t *entry = &s_rtc_url_sets[ring_push(&s_rtc_url_set_pos, URL_SET_RING_SIZE)];
    *entry = (url_set_entry_t) {
        .attempt_id = attempt->attempt_id,
        .timestamp_ms = uptime_ms(),
        .duration_ms = attempt->duration_ms,
        .nfc_err = attempt->nfc_err,
        .result_code = attempt->result_code,
        .ble_connected_start = attempt->ble_connected_start,
        .ble_connected_end = attempt->ble_connected_end,
    };
}
