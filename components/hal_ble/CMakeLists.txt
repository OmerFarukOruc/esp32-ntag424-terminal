idf_component_register(
    SRCS "hal_ble.c" "hal_ble_notify.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES bt esp_app_format esp_timer
)
