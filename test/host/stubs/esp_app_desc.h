/*
 * SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
typedef struct {
    char version[32];
} esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
