/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdatomic.h>
#include <stdbool.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "pn532_io_deadline.h"

#define NO_PORT (-1)

static _Atomic int s_port = NO_PORT;
static _Atomic int64_t s_deadline_us;

void pn532_io_deadline_set(uart_port_t port, int64_t deadline_us)
{
    atomic_store(&s_deadline_us, deadline_us);
    atomic_store(&s_port, (int)port);
}

void pn532_io_deadline_clear(void)
{
    atomic_store(&s_port, NO_PORT);
}

int __real_uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait);
int __real_uart_write_bytes(uart_port_t uart_num, const void *src, size_t size);

static bool bounded(uart_port_t uart_num)
{
    return (int)uart_num == atomic_load(&s_port);
}

static TickType_t ticks_until(int64_t end_us)
{
    int64_t left_us = end_us - esp_timer_get_time();
    if (left_us <= 0) {
        return 0;
    }
    TickType_t ticks = pdMS_TO_TICKS((left_us + 999) / 1000);
    return ticks > 0 ? ticks : 1;
}

int __wrap_uart_read_bytes(uart_port_t uart_num, void *buf, uint32_t length, TickType_t ticks_to_wait)
{
    if (!bounded(uart_num)) {
        return __real_uart_read_bytes(uart_num, buf, length, ticks_to_wait);
    }

    int64_t deadline_us = atomic_load(&s_deadline_us);
    uint8_t *out = buf;
    uint32_t got = 0;
    while (got < length) {
        TickType_t wait = ticks_until(deadline_us);
        if (wait == 0) {
            break;
        }
        int n = __real_uart_read_bytes(uart_num, out + got, 1, wait < ticks_to_wait ? wait : ticks_to_wait);
        if (n <= 0) {
            return got > 0 ? (int)got : n;
        }
        got++;
    }
    return (int)got;
}

int __wrap_uart_write_bytes(uart_port_t uart_num, const void *src, size_t size)
{
    if (bounded(uart_num) && ticks_until(atomic_load(&s_deadline_us)) == 0) {
        return -1;
    }
    return __real_uart_write_bytes(uart_num, src, size);
}
