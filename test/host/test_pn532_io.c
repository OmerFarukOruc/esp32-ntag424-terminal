/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/semphr.h"
#include PN532_SRC
#include "hal_nfc.h"
#include "hal_nfc_priv.h"
#include "pn532_io_deadline.h"

extern int64_t host_now_us;
extern jmp_buf hang_jmp;
extern int64_t hang_limit_us;
extern long bytes_read;

#include "check.h"

int main(void)
{
    char d[160];
    hang_limit_us = host_now_us + 120LL * 1000 * 1000;
    int64_t t0 = host_now_us;
    esp_err_t e = ESP_OK;
    int hung = setjmp(hang_jmp);
    if (!hung) {
        e = hal_nfc_init();
    }
    snprintf(d, sizeof d, "%s after %.1f s simulated, %ld bytes read, init=%s", hung ? "still reading" : "returned",
             (host_now_us - t0) / 1e6, bytes_read, hung ? "-" : esp_err_to_name(e));
    check("hal_nfc_init on a babbling line returns an error", !hung && e != ESP_OK, d);

    pn532_t *p = calloc(1, sizeof * p);
    p->uart = CONFIG_HAL_NFC_UART_NUM; p->mutex = xSemaphoreCreateBinary();
    g_nfc.pn532 = p; g_nfc.mutex = xSemaphoreCreateMutex();
    bytes_read = 0;
    t0 = host_now_us; hang_limit_us = host_now_us + 120LL * 1000 * 1000;
    hal_nfc_err_t r = HAL_NFC_OK;
    hung = setjmp(hang_jmp);
    if (!hung) {
        r = hal_nfc_ntag424_configure_sdm("SHOP", 3000);
    }
    snprintf(d, sizeof d, "%s after %.1f s simulated, %ld bytes read, result=%d", hung ? "still reading" : "returned",
             (host_now_us - t0) / 1e6, bytes_read, hung ? 0 : r);
    check("configure_sdm(3000 ms) on a babbling line returns within 3200 ms", !hung && host_now_us - t0 <= 3200000, d);
    extern int64_t dribble_gap_us;
    uint8_t frame[200];
    dribble_gap_us = 5000;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, host_now_us + 100000);
    t0 = host_now_us;
    int n = uart_read_bytes(CONFIG_HAL_NFC_UART_NUM, frame, sizeof frame, 10);
    pn532_io_deadline_clear();
    dribble_gap_us = 0;
    snprintf(d, sizeof d, "200-byte read, one byte per 5 ms, deadline in 100 ms: %d bytes after %.0f ms", n, (host_now_us - t0) / 1e3);
    check("a multi-byte read of a slow line ends at the deadline", host_now_us - t0 <= 101000, d);
    dribble_gap_us = 87;
    pn532_io_deadline_set(CONFIG_HAL_NFC_UART_NUM, host_now_us + 1000000);
    t0 = host_now_us;
    n = uart_read_bytes(CONFIG_HAL_NFC_UART_NUM, frame, sizeof frame, 10);
    pn532_io_deadline_clear();
    dribble_gap_us = 0;
    snprintf(d, sizeof d, "200-byte frame at 115200 bps, 10 ms wait: %d bytes after %.1f ms", n, (host_now_us - t0) / 1e3);
    check("a whole frame at line speed is read", n == 200, d);
    return check_summary();
}
