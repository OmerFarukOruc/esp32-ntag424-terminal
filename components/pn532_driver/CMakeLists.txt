# Builds libs/ESP32-PN532/pn532.c as an ESP-IDF component.
#
# The upstream CMakeLists.txt requires only the legacy "driver" component, which
# no longer exports esp_driver_uart and esp_driver_gpio since ESP-IDF v6.0, so
# its driver/uart.h include fails when the library is used directly. Its Kconfig
# is not read either, so the CONFIG_PN532_* debug options in pn532.c stay off.
# The submodule lives in libs/ so ESP-IDF does not register it as a component.
set(PN532_LIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../../libs/ESP32-PN532")

if(NOT EXISTS "${PN532_LIB_DIR}/pn532.c")
    message(FATAL_ERROR "Cannot find ESP32-PN532 library at ${PN532_LIB_DIR}. Run: git submodule update --init")
endif()

idf_component_register(
    SRCS "${PN532_LIB_DIR}/pn532.c"
    INCLUDE_DIRS "${PN532_LIB_DIR}/include"
    PRIV_REQUIRES esp_driver_uart esp_driver_gpio
)
