/*
 * SPDX-FileCopyrightText: 2025-2026 Omer Faruk Oruc
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "esp_app_desc.h"
#include "esp_log.h"

#include "app_state.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "==========================================");
    ESP_LOGI(TAG, "ESP32 NTAG424 terminal");
    ESP_LOGI(TAG, "Firmware %s", esp_app_get_description()->version);
    ESP_LOGI(TAG, "==========================================");

    esp_err_t err = app_state_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize application: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "Device ready");
    ESP_LOGI(TAG, "- Hold button 3s for BLE onboarding");
    ESP_LOGI(TAG, "- Hold button 10s for FACTORY RESET");
    ESP_LOGI(TAG, "==========================================");
}
