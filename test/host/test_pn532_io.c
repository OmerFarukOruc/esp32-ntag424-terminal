/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
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

static void arm_hang_guard(void)
{
    host_hang_limit_us = host_now_us + HANG_LIMIT_US;
    host_hang_armed = true;
    host_bytes_read = 0;
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

    pn532_t *p = calloc(1, sizeof * p);
    p->uart = CONFIG_HAL_NFC_UART_NUM;
    p->mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(p->mutex);
    g_nfc.pn532 = p;
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
    return check_summary();
}
