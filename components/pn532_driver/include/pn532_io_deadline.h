/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pn532_io_deadline.h
 * @brief A deadline for every UART read the PN532 library makes
 *
 * The library waits for a frame preamble one byte at a time and restarts its
 * timeout on each byte, so a stream of other bytes keeps it reading forever.
 * The component links uart_read_bytes() through a wrapper: while a deadline
 * is set for a port, a read on that port reads one byte at a time, waits
 * up to its ticks_to_wait for each byte as uart_read_bytes() does for each
 * fragment, and ends at the deadline. A read at or after the deadline
 * returns 0 bytes, which the library treats as a timeout. Reads on other
 * ports are unchanged.
 */

#ifndef PN532_IO_DEADLINE_H
#define PN532_IO_DEADLINE_H

#include <stdint.h>
#include "driver/uart.h"

/**
 * @brief Bound reads on port until deadline_us, in esp_timer_get_time() time
 *
 * Call it only while holding the lock that serializes PN532 conversations,
 * and clear it before releasing that lock.
 */
void pn532_io_deadline_set(uart_port_t port, int64_t deadline_us);

/** @brief Remove the deadline */
void pn532_io_deadline_clear(void);

#endif
