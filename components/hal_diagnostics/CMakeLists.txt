idf_component_register(
    SRCS "hal_diagnostics.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer nvs_flash
)
