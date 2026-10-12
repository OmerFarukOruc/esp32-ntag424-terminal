idf_component_register(
    SRCS "hal_indicator.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_rmt esp_driver_ledc esp_timer
)
