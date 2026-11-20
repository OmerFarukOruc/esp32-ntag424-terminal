/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
typedef int uart_port_t;
typedef struct {
    int baud_rate, data_bits, parity, stop_bits, flow_ctrl, rx_flow_ctrl_thresh, source_clk;
} uart_config_t;
#define UART_DATA_8_BITS 3
#define UART_PARITY_DISABLE 0
#define UART_STOP_BITS_1 1
#define UART_HW_FLOWCTRL_DISABLE 0
#define UART_SCLK_DEFAULT 0
#define UART_PIN_NO_CHANGE (-1)
#define UART_NUM_MAX 3
esp_err_t uart_param_config(uart_port_t, const uart_config_t *);
esp_err_t uart_set_pin(uart_port_t, int, int, int, int);
esp_err_t uart_driver_install(uart_port_t, int, int, int, void *, int);
esp_err_t uart_driver_delete(uart_port_t);
bool uart_is_driver_installed(uart_port_t);
int uart_read_bytes(uart_port_t, void *, uint32_t, TickType_t);
int uart_write_bytes(uart_port_t, const void *, size_t);
esp_err_t uart_wait_tx_done(uart_port_t, TickType_t);
esp_err_t uart_flush_input(uart_port_t);
esp_err_t uart_get_buffered_data_len(uart_port_t, size_t *);
