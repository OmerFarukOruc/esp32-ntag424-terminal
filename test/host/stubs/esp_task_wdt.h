/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include "esp_err.h"
esp_err_t esp_task_wdt_add(void *task);
esp_err_t esp_task_wdt_reset(void);
