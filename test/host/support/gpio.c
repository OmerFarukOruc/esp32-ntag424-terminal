/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "driver/gpio.h"
#include "host.h"

int host_gpio_level = 1;
gpio_isr_t host_gpio_isr;

int gpio_get_level(gpio_num_t g)
{
    return host_gpio_level;
}

esp_err_t gpio_config(const gpio_config_t *c)
{
    return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t g)
{
    return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t g)
{
    return ESP_OK;
}

esp_err_t gpio_install_isr_service(int flags)
{
    return ESP_OK;
}

esp_err_t gpio_isr_handler_add(gpio_num_t g, gpio_isr_t isr, void *arg)
{
    host_gpio_isr = isr;
    return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t g)
{
    host_gpio_isr = NULL;
    return ESP_OK;
}

esp_err_t gpio_reset_pin(gpio_num_t g)
{
    return ESP_OK;
}

esp_err_t gpio_set_drive_capability(gpio_num_t g, gpio_drive_cap_t c)
{
    return ESP_OK;
}
