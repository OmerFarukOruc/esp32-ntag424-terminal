/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"
#include PN532_SRC
#include "hal_nfc.h"
#include "hal_nfc_priv.h"
#include "pn532_io_deadline.h"
#include "check.h"
#include "host.h"

#define HANG_LIMIT_US (120LL * 1000 * 1000)
#define SLOW_BYTE_GAP_US 5000
#define SLOW_DEADLINE_US 100000
#define SLOW_BYTES_BY_DEADLINE (SLOW_DEADLINE_US / SLOW_BYTE_GAP_US)
#define LINE_SPEED_BYTE_GAP_US 87
#define ONE_TICK_US (portTICK_PERIOD_MS * 1000)
#define SILENT_LINE_GAP_US (60LL * 1000 * 1000)
#define REINIT_DEADLINE_US 300000
#define GET_FIRMWARE_VERSION 0x02
#define IN_DATA_EXCHANGE 0x40
#define LONGEST_APDU 256

static void arm_hang_guard(void)
{
    host_hang_limit_us = host_now_us + HANG_LIMIT_US;
    host_hang_armed = true;
    host_bytes_read = 0;
}

static pn532_t *open_test_reader(void)
{
    pn532_t *p = calloc(1, sizeof * p);
    p->uart = CONFIG_HAL_NFC_UART_NUM;
    p->mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(p->mutex);
    return p;
}

typedef struct {
    bool hung;
    int result;
    int64_t elapsed_us;
    size_t written;
} tx_run_t;

static tx_run_t send_request(pn532_t *p, int64_t deadline_us, uint8_t cmd, int len, const uint8_t *data)
{
    tx_run_t run = { 0 };
    host_bytes_written = 0;
    int64_t t0 = host_now_us;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, deadline_us);
    arm_hang_guard();
    run.hung = setjmp(host_hang_jmp) != 0;
    if (!run.hung) {
        run.result = pn532_tx(p, cmd, 0, NULL, len, data);
    }
    host_hang_armed = false;
    pn532_io_deadline_clear();
    p->pending = 0;
    run.elapsed_us = host_now_us - t0;
    run.written = host_bytes_written;
    return run;
}

static void describe(char *d, size_t n, const char *setup, tx_run_t run)
{
    snprintf(d, n, "%s: %s after %.1f ms, %zu bytes written, result=%d", setup, run.hung ? "still writing" : "returned",
             run.elapsed_us / 1e3, run.written, run.result);
}

static bool sent_whole(const uint8_t *data, size_t len)
{
    size_t logged = host_bytes_written < HOST_TX_LOG_MAX ? host_bytes_written : HOST_TX_LOG_MAX;
    for (size_t at = 0; at + len <= logged; at++) {
        if (memcmp(&host_tx_log[at], data, len) == 0) {
            return true;
        }
    }
    return false;
}

static void check_transmit_bounds(void)
{
    char d[200];
    pn532_t *p = open_test_reader();
    static const uint8_t frame[] = { 0x55, 0x55, 0x55, 0x00, 0x00, 0xFF, 0x02, 0xFE, 0xD4, GET_FIRMWARE_VERSION, 0x2A, 0x00 };

    tx_run_t run = send_request(p, host_now_us + 1000000, GET_FIRMWARE_VERSION, 0, NULL);
    describe(d, sizeof d, "deadline in 1 s", run);
    check("a command sent before its deadline goes out as the unchanged PN532 frame",
          !run.hung && run.written == sizeof frame && memcmp(host_tx_log, frame, sizeof frame) == 0, d);

    uint8_t apdu[LONGEST_APDU];
    for (size_t i = 0; i < sizeof apdu; i++) {
        apdu[i] = (uint8_t)(i * 7 + 1);
    }
    run = send_request(p, host_now_us + 1000000, IN_DATA_EXCHANGE, sizeof apdu, apdu);
    describe(d, sizeof d, "256 byte APDU, deadline in 1 s", run);
    check("the longest APDU before its deadline goes out whole", !run.hung && sent_whole(apdu, sizeof apdu), d);

    run = send_request(p, host_now_us - 1, GET_FIRMWARE_VERSION, 0, NULL);
    describe(d, sizeof d, "deadline already passed", run);
    check("a command after its deadline writes nothing and fails at once",
          !run.hung && run.result < 0 && run.written == 0 && run.elapsed_us <= ONE_TICK_US, d);
    pn532_end(p);

    host_bytes_written = 0;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, host_now_us - 1);
    int n = uart_write_bytes(CONFIG_HAL_NFC_UART_NUM + 1, frame, sizeof frame);
    pn532_io_deadline_clear();
    snprintf(d, sizeof d, "result=%d, %zu bytes written", n, host_bytes_written);
    check("a write on another port goes out after the reader's deadline",
          n == (int)sizeof frame && host_bytes_written == sizeof frame, d);
}

static void check_recovery_after_deadline(void)
{
    char d[200];
    pn532_t *p = open_test_reader();
    g_nfc.pn532 = p;
    g_nfc.deadline_us = host_now_us - 1;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, g_nfc.deadline_us);
    host_bytes_written = 0;
    int64_t t0 = host_now_us;
    bool reinitialised = false;
    bool recovered = nfc_reader_recover(&reinitialised);
    pn532_io_deadline_clear();
    g_nfc.deadline_us = 0;
    snprintf(d, sizeof d, "recovered=%d after %.1f ms, %zu bytes written, reader %s", recovered, (host_now_us - t0) / 1e3,
             host_bytes_written, g_nfc.pn532 == p ? "kept" : "closed");
    check("recovery after the deadline sends nothing, keeps the open reader and reports no re-init",
          !recovered && !reinitialised && host_bytes_written == 0 && g_nfc.pn532 == p && host_now_us - t0 <= ONE_TICK_US, d);
    g_nfc.pn532 = pn532_end(g_nfc.pn532);
}

static void check_reinit_stops_at_deadline(void)
{
    char d[200];
    g_nfc.pn532 = open_test_reader();
    host_byte_gap_us = SILENT_LINE_GAP_US;
    host_uart_installs = 0;
    int64_t t0 = host_now_us;
    g_nfc.deadline_us = t0 + REINIT_DEADLINE_US;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, g_nfc.deadline_us);
    bool reinitialised = false;
    bool recovered = nfc_reader_recover(&reinitialised);
    pn532_io_deadline_clear();
    snprintf(d, sizeof d, "recovered=%d re-init=%d after %.1f ms, UART driver installs=%d, reader %s", recovered,
             reinitialised, (host_now_us - t0) / 1e3, host_uart_installs, g_nfc.pn532 == NULL ? "closed" : "open");
    check("a re-init whose settling delay reaches the deadline opens no new reader",
          !recovered && reinitialised && host_uart_installs == 0 && g_nfc.pn532 == NULL &&
          host_now_us - t0 <= REINIT_DEADLINE_US + ONE_TICK_US, d);
    g_nfc.pn532 = pn532_end(g_nfc.pn532);
    g_nfc.deadline_us = 0;
    host_byte_gap_us = 0;
    host_uart_installed = true;
}

int main(void)
{
    char d[200];
    int64_t t0 = host_now_us;
    esp_err_t e = ESP_OK;
    arm_hang_guard();
    bool hung = setjmp(host_hang_jmp) != 0;
    if (!hung) {
        e = hal_nfc_init();
    }
    host_hang_armed = false;
    snprintf(d, sizeof d, "%s after %.1f s simulated, %ld bytes read, init=%s", hung ? "still reading" : "returned",
             (host_now_us - t0) / 1e6, host_bytes_read, hung ? "-" : esp_err_to_name(e));
    check("hal_nfc_init on a babbling line returns an error", !hung && e != ESP_OK, d);

    g_nfc.pn532 = open_test_reader();
    g_nfc.mutex = xSemaphoreCreateMutex();
    t0 = host_now_us;
    hal_nfc_err_t r = HAL_NFC_OK;
    arm_hang_guard();
    hung = setjmp(host_hang_jmp) != 0;
    if (!hung) {
        r = hal_nfc_ntag424_configure_sdm("SHOP", 3000);
    }
    host_hang_armed = false;
    snprintf(d, sizeof d, "%s after %.1f s simulated, %ld bytes read, result=%d", hung ? "still reading" : "returned",
             (host_now_us - t0) / 1e6, host_bytes_read, hung ? -1 : (int)r);
    check("configure_sdm(3000 ms) on a babbling line times out within 3200 ms",
          !hung && r == HAL_NFC_ERR_SDM_TIMEOUT && host_now_us - t0 <= 3200000, d);
    g_nfc.pn532 = pn532_end(g_nfc.pn532);

    uint8_t frame[200];
    host_byte_gap_us = SLOW_BYTE_GAP_US;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, host_now_us + SLOW_DEADLINE_US);
    t0 = host_now_us;
    int n = uart_read_bytes(CONFIG_HAL_NFC_UART_NUM, frame, sizeof frame, 10);
    pn532_io_deadline_clear();
    snprintf(d, sizeof d, "200-byte read, one byte per 5 ms, deadline in 100 ms: %d bytes after %.1f ms", n,
             (host_now_us - t0) / 1e3);
    check("a multi-byte read of a slow line returns the bytes that arrived by the deadline",
          n == SLOW_BYTES_BY_DEADLINE && host_now_us - t0 == SLOW_DEADLINE_US, d);

    host_byte_gap_us = LINE_SPEED_BYTE_GAP_US;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, host_now_us + 1000000);
    t0 = host_now_us;
    n = uart_read_bytes(CONFIG_HAL_NFC_UART_NUM, frame, sizeof frame, 10);
    pn532_io_deadline_clear();
    host_byte_gap_us = 0;
    snprintf(d, sizeof d, "200-byte frame at 115200 bps, 10 ms wait: %d bytes after %.1f ms", n, (host_now_us - t0) / 1e3);
    check("a whole frame at line speed is read", n == (int)sizeof frame, d);

    check_transmit_bounds();
    check_recovery_after_deadline();
    check_reinit_stops_at_deadline();
    return check_summary();
}
