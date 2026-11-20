/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Kept out of the files that call uart_read_bytes(), so the linker wraps those calls as in the firmware. */
#include <string.h>
#include "driver/uart.h"
#include "host.h"

#define BYTE_TIME_US_115200 87
#define LINE_BYTE 0x55

jmp_buf host_hang_jmp;
bool host_hang_armed;
int64_t host_hang_limit_us;
int64_t host_byte_gap_us;
long host_bytes_read;

int uart_read_bytes(uart_port_t port, void *buf, uint32_t length, TickType_t ticks)
{
    if (host_hang_armed && host_now_us > host_hang_limit_us) {
        host_hang_armed = false;
        longjmp(host_hang_jmp, 1);
    }
    int64_t wait_us = (int64_t)ticks * portTICK_PERIOD_MS * 1000;
    if (host_byte_gap_us > wait_us) {
        host_now_us += wait_us;
        return 0;
    }
    int64_t gap_us = host_byte_gap_us > 0 ? host_byte_gap_us : BYTE_TIME_US_115200;
    memset(buf, LINE_BYTE, length);
    host_now_us += gap_us * length;
    host_bytes_read += length;
    return (int)length;
}

int uart_write_bytes(uart_port_t p, const void *b, size_t n)
{
    return (int)n;
}

esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t t)
{
    return ESP_OK;
}

esp_err_t uart_flush_input(uart_port_t p)
{
    return ESP_OK;
}

esp_err_t uart_get_buffered_data_len(uart_port_t p, size_t *n)
{
    *n = 0;
    return ESP_OK;
}

esp_err_t uart_param_config(uart_port_t p, const uart_config_t *c)
{
    return ESP_OK;
}

esp_err_t uart_set_pin(uart_port_t p, int a, int b, int c, int d)
{
    return ESP_OK;
}

esp_err_t uart_driver_install(uart_port_t p, int a, int b, int c, void *d, int e)
{
    return ESP_OK;
}

esp_err_t uart_driver_delete(uart_port_t p)
{
    return ESP_OK;
}

bool uart_is_driver_installed(uart_port_t p)
{
    return true;
}
