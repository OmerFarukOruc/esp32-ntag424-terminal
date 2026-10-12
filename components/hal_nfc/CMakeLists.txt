idf_component_register(
    SRCS "hal_nfc.c" "nfc_reader.c" "nfc_aes.c" "desfire_ev2.c" "ntag424_sdm.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_uart esp_timer mbedtls pn532_driver
)
