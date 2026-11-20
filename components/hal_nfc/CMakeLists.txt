idf_component_register(
    SRCS "hal_nfc.c" "nfc_reader.c" "nfc_aes.c" "desfire_ev2.c" "ntag424_sdm.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_driver_uart esp_timer mbedtls pn532_driver
)

set(url_label "[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?")
set(url_host "${url_label}(\\.${url_label})*")
set(url_segment "/([A-Za-z0-9._~!*+,=:@&-]|%[0-9A-Fa-f][0-9A-Fa-f])+")
if(NOT CMAKE_BUILD_EARLY_EXPANSION AND
   NOT "${CONFIG_HAL_NFC_SDM_BASE_URL}" MATCHES "^https?://${url_host}(:[0-9]+)?(${url_segment})*$")
    message(FATAL_ERROR
        "CONFIG_HAL_NFC_SDM_BASE_URL \"${CONFIG_HAL_NFC_SDM_BASE_URL}\" must be an http:// or https:// URL "
        "with a host name, an optional port and path, and no query, fragment, space or trailing slash")
endif()
