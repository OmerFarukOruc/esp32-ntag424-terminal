# SPDX-FileCopyrightText: 2026 Omer Faruk Oruc
#
# SPDX-License-Identifier: Apache-2.0
#
# Usage: cmake -P test/host/test_sdm_url.cmake
cmake_minimum_required(VERSION 3.16)

if(DEFINED url)
    function(idf_component_register)
    endfunction()
    set(CMAKE_BUILD_EARLY_EXPANSION FALSE)
    set(CONFIG_HAL_NFC_SDM_BASE_URL "${url}")
    include("${CMAKE_CURRENT_LIST_DIR}/../../components/hal_nfc/CMakeLists.txt")
    return()
endif()

set(accepted
    "https://example.com/tap"
    "http://example.com"
    "https://example.com:1/tap"
    "https://example.com:443/tap"
    "https://example.com:65535"
    "https://example.com:00080/tap")
set(refused
    "https://example.com:0/tap"
    "https://example.com:000"
    "https://example.com:65536/tap"
    "https://example.com:99999999999999999999/tap"
    "https://example.com:/tap"
    "https://example.com/tap/"
    "https://example.com/tap?x=1"
    "https://example.com/tap#f"
    "ftp://example.com/tap")

set(failures 0)
set(checks 0)
foreach(expect accepted refused)
    foreach(case IN LISTS ${expect})
        math(EXPR checks "${checks} + 1")
        execute_process(COMMAND "${CMAKE_COMMAND}" "-Durl=${case}" -P "${CMAKE_CURRENT_LIST_FILE}"
                        RESULT_VARIABLE rc OUTPUT_QUIET ERROR_QUIET)
        if((expect STREQUAL "accepted" AND rc EQUAL 0) OR (expect STREQUAL "refused" AND NOT rc EQUAL 0))
            message("PASS  ${case} is ${expect}")
        else()
            message("FAIL  ${case} is ${expect}  (exit ${rc})")
            math(EXPR failures "${failures} + 1")
        endif()
    endforeach()
endforeach()
message("${failures} of ${checks} checks failed")
if(failures GREATER 0)
    message(FATAL_ERROR "SDM base URL checks failed")
endif()
