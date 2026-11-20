#!/usr/bin/env bash
# Usage: test/host/run.sh [suite...]
# Builds each suite with the host gcc, AddressSanitizer and UBSan, runs it, and exits non-zero when a check fails.
# Needs gcc, the OpenSSL libcrypto headers, and managed_components/ from an idf.py build or reconfigure.
set -euo pipefail

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
host=$repo/test/host
cjson=$repo/managed_components/espressif__cjson/cJSON
read -ra crypto <<< "$(pkg-config --cflags --libs libcrypto 2>/dev/null || echo -lcrypto)"
out=$(mktemp -d)
trap 'rm -rf "${out:?}"' EXIT

cflags=(-std=gnu11 -g -Wall -Wextra -Wno-unused-parameter -Werror "-fsanitize=address,undefined"
        -fno-sanitize-recover=undefined -I "$host" -I "$host/stubs" -I "$host/support")
nfc=$repo/components/hal_nfc
nfc_inc=(-DESP_PLATFORM -I "$nfc/include" -I "$nfc" -I "$repo/components/pn532_driver/include"
         -I "$repo/libs/ESP32-PN532/include")

build() {
    local suite=$1
    shift
    case $suite in
    app_state)
        local inc=() d
        for d in "$repo"/components/*/include; do inc+=(-I "$d"); done
        gcc "${cflags[@]}" "${inc[@]}" -I "$repo/main/core" -DAPP_STATE_SRC="\"$repo/main/core/app_state.c\"" \
            "$host/test_app_state.c" "$repo/main/core/app_url_set.c" "$repo/main/core/app_wifi_scan.c" \
            "$host/support/app_platform.c" "$host/support/platform.c" "$@"
        ;;
    ble)
        [[ -f $cjson/cJSON.c ]] || { echo "missing $cjson; run idf.py reconfigure first" >&2; return 1; }
        gcc "${cflags[@]}" -I "$repo/components/hal_ble/include" -I "$repo/components/hal_ble" -I "$cjson" \
            -DHAL_BLE_SRC="\"$repo/components/hal_ble/hal_ble.c\"" "$host/test_ble.c" "$host/support/nimble.c" \
            "$host/support/platform.c" "$cjson/cJSON.c" -lm -Wl,--wrap=free "$@"
        ;;
    network)
        gcc "${cflags[@]}" -I "$repo/components/hal_network/include" \
            -DHAL_NET_SRC="\"$repo/components/hal_network/hal_network.c\"" "$host/test_network.c" \
            "$host/support/wifi.c" "$host/support/platform.c" "$@"
        ;;
    desfire)
        gcc "${cflags[@]}" "${nfc_inc[@]}" -DDESFIRE_SRC="\"$nfc/desfire_ev2.c\"" "$host/test_desfire.c" \
            "$host/support/crypto_openssl.c" "$host/support/nfc_reader_errors.c" "$host/support/platform.c" "${crypto[@]}" "$@"
        ;;
    sdm)
        gcc "${cflags[@]}" "${nfc_inc[@]}" "$host/test_sdm.c" "$host/support/uart.c" "$host/support/gpio.c" "$nfc/hal_nfc.c" \
            "$nfc/ntag424_sdm.c" "$nfc/desfire_ev2.c" "$host/support/nfc_reader_errors.c" "$host/support/platform.c" "$@"
        ;;
    pn532_io)
        # The vendored pn532.c prints uint32_t with %ld, which fits the ESP32 toolchain but not a 64-bit host.
        gcc "${cflags[@]}" -Wno-format "${nfc_inc[@]}" -DPN532_SRC="\"$repo/libs/ESP32-PN532/pn532.c\"" "$host/test_pn532_io.c" \
            "$host/support/uart.c" "$host/support/gpio.c" "$nfc/hal_nfc.c" "$nfc/nfc_reader.c" "$nfc/desfire_ev2.c" "$nfc/ntag424_sdm.c" \
            "$repo/components/pn532_driver/pn532_io_deadline.c" "$host/support/crypto_openssl.c" \
            "$host/support/platform.c" -Wl,--wrap=uart_read_bytes "${crypto[@]}" "$@"
        ;;
    power)
        gcc "${cflags[@]}" -I "$repo/components/hal_power/include" \
            -DHAL_POWER_SRC="\"$repo/components/hal_power/hal_power.c\"" "$host/test_power.c" \
            "$host/support/platform.c" "$@"
        ;;
    button)
        gcc "${cflags[@]}" -I "$repo/components/hal_button/include" \
            -DHAL_BUTTON_SRC="\"$repo/components/hal_button/hal_button.c\"" "$host/test_button.c" \
            "$host/support/gpio.c" "$host/support/platform.c" "$@"
        ;;
    *)
        echo "unknown suite $suite" >&2
        return 1
        ;;
    esac
}

suites=("$@")
[[ ${#suites[@]} -gt 0 ]] || suites=(app_state ble network desfire sdm pn532_io power button)
failed=()
checks=0
for suite in "${suites[@]}"; do
    echo "== $suite"
    if build "$suite" -o "$out/$suite" && "$out/$suite" | tee "$out/$suite.log"; then
        read -r _ _ count _ < <(grep -E '^[0-9]+ of [0-9]+ checks failed$' "$out/$suite.log")
        checks=$((checks + count))
        continue
    fi
    failed+=("$suite")
done

if [[ ${#failed[@]} -gt 0 ]]; then
    echo "failed suites: ${failed[*]}"
    exit 1
fi
echo "all ${#suites[@]} suites passed, $checks checks"
