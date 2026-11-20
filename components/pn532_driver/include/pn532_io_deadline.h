/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pn532_io_deadline.h
 * @brief A deadline for every UART read and write the PN532 library makes
 *
 * The library waits for a frame preamble one byte at a time and restarts its
 * timeout on each byte, so a stream of other bytes keeps it reading forever.
 * The component links uart_read_bytes() and uart_write_bytes() through
 * wrappers. While a deadline is set for a port:
 *
 * - A read on that port reads one byte at a time, waits up to its
 *   ticks_to_wait for each byte as uart_read_bytes() does for each fragment,
 *   and ends at the deadline. A read at or after the deadline returns 0
 *   bytes, which the library treats as a timeout.
 * - A write at or after the deadline returns -1 and sends nothing, so no
 *   command reaches the PN532 after it. A write before the deadline is
 *   unchanged. The library sets up the UART at 115200 baud without flow
 *   control, so the bytes leave at line rate whatever the PN532 does, and the
 *   longest frame, a 256 byte APDU, takes about 23 ms. The library's wait for
 *   the bytes to leave ends after that line time too.
 *
 * Calls on other ports are unchanged. The library also takes its own mutex
 * with no time limit before each command. Callers hold the lock that
 * serializes PN532 conversations, and every command the library starts is
 * followed by its pn532_rx(), which gives the mutex back, so that wait ends
 * at once.
 */

#ifndef PN532_IO_DEADLINE_H
#define PN532_IO_DEADLINE_H

#include <stdint.h>
#include "driver/uart.h"

/**
 * @brief Bound reads and writes on port until deadline_us, in esp_timer_get_time() time
 *
 * Call it only while holding the lock that serializes PN532 conversations,
 * and clear it before releasing that lock.
 */
void pn532_io_deadline_set(uart_port_t port, int64_t deadline_us);

/** @brief Remove the deadline */
void pn532_io_deadline_clear(void);

#endif
