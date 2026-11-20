/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
/* Kept out of the files that call uart_read_bytes(), so the linker wraps those calls as in the firmware. */
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/semphr.h"
extern int64_t host_now_us;
jmp_buf hang_jmp;
int64_t hang_limit_us;
long bytes_read;

int64_t dribble_gap_us;
int uart_read_bytes(uart_port_t port, void *buf, uint32_t length, TickType_t ticks)
{
    (void)port;
    if (host_now_us > hang_limit_us) {
        longjmp(hang_jmp, 1);
    }
    if (dribble_gap_us) {
        /* ESP-IDF waits up to ticks for each fragment; here every byte arrives alone after dribble_gap_us. */
        uint32_t got = 0;
        int64_t wait_us = (int64_t)ticks * 1000;
        while (got < length && dribble_gap_us <= wait_us) {
            host_now_us += dribble_gap_us;
            ((uint8_t *)buf)[got++] = 0x55;
        }
        if (got < length) {
            host_now_us += wait_us;
        }
        bytes_read += got;
        return (int)got;
    }
    memset(buf, 0x55, length);
    host_now_us += 87 * (int64_t)length;   /* one byte at 115200 bps */
    bytes_read += length;
    return (int)length;
}
int uart_write_bytes(uart_port_t p, const void *b, size_t n)
{
    (void)p;
    (void)b;
    return (int)n;
}
esp_err_t uart_wait_tx_done(uart_port_t p, TickType_t t)
{
    (void)p;
    (void)t;
    return ESP_OK;
}
esp_err_t uart_flush_input(uart_port_t p)
{
    (void)p;
    return ESP_OK;
}
esp_err_t uart_get_buffered_data_len(uart_port_t p, size_t *n)
{
    (void)p;
    *n = 0;
    return ESP_OK;
}
esp_err_t uart_param_config(uart_port_t p, const uart_config_t *c)
{
    (void)p;
    (void)c;
    return ESP_OK;
}
esp_err_t uart_set_pin(uart_port_t p, int a, int b, int c, int d)
{
    (void)p;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return ESP_OK;
}
esp_err_t uart_driver_install(uart_port_t p, int a, int b, int c, void *d, int e)
{
    (void)p;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
    return ESP_OK;
}
esp_err_t uart_driver_delete(uart_port_t p)
{
    (void)p;
    return ESP_OK;
}
bool uart_is_driver_installed(uart_port_t p)
{
    (void)p;
    return true;
}
esp_err_t gpio_reset_pin(gpio_num_t g)
{
    (void)g;
    return ESP_OK;
}
esp_err_t gpio_set_drive_capability(gpio_num_t g, gpio_drive_cap_t c)
{
    (void)g;
    (void)c;
    return ESP_OK;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return malloc(1);
}
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return malloc(1);
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t w)
{
    (void)s;
    (void)w;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{
    (void)s;
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t s)
{
    free(s);
}
void esp_fill_random(void *buf, size_t len)
{
    memset(buf, 0x11, len);
}
