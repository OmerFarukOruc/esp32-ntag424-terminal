idf_component_register(
    SRCS "main.c" "core/app_state.c" "core/app_url_set.c" "core/app_wifi_scan.c"
    INCLUDE_DIRS "core"
    PRIV_REQUIRES hal_nfc hal_indicator hal_network hal_power hal_button hal_ble hal_time hal_diagnostics
                  esp_timer nvs_flash esp_app_format
)
