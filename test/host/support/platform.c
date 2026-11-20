/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/task.h"
int host_log_enabled = 0;
const char *esp_err_to_name(esp_err_t e)
{
    static char b[16];
    snprintf(b, sizeof b, "err%d", e);
    return b;
}
int64_t host_now_us;
int64_t esp_timer_get_time(void)
{
    return host_now_us;
}
void vTaskDelay(TickType_t ticks)
{
    host_now_us += (int64_t)ticks * 1000;
}
