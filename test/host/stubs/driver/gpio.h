/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
typedef int gpio_drive_cap_t;
#define GPIO_DRIVE_CAP_3 3
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_INTR_DISABLE, GPIO_INTR_ANYEDGE = 3 } gpio_int_type_t;
typedef enum { GPIO_MODE_INPUT = 1 } gpio_mode_t;
typedef struct {
    unsigned long long pin_bit_mask;
    gpio_mode_t mode;
    gpio_pullup_t pull_up_en;
    gpio_pulldown_t pull_down_en;
    gpio_int_type_t intr_type;
} gpio_config_t;
typedef void (*gpio_isr_t)(void *);
#define GPIO_IS_VALID_GPIO(g) ((g) >= 0 && (g) < 49)
#define GPIO_IS_VALID_OUTPUT_GPIO(g) GPIO_IS_VALID_GPIO(g)
int gpio_get_level(gpio_num_t);
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_intr_enable(gpio_num_t);
esp_err_t gpio_intr_disable(gpio_num_t);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_isr_handler_add(gpio_num_t, gpio_isr_t, void *);
esp_err_t gpio_isr_handler_remove(gpio_num_t);
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t gpio_set_drive_capability(gpio_num_t, gpio_drive_cap_t);
