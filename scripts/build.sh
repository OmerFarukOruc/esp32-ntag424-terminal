#!/usr/bin/env bash
# Build the firmware. Activates ESP-IDF first when idf.py is not on PATH.
set -euo pipefail

cd "$(dirname "$0")/.."

if [[ ! -f libs/ESP32-PN532/pn532.c ]]; then
    git submodule update --init
fi

if command -v idf.py >/dev/null 2>&1; then
    exec idf.py build
fi

activate="${IDF_ACTIVATE:-$HOME/.espressif/tools/activate_idf_v6.1.sh}"
if [[ ! -f "$activate" ]]; then
    echo "idf.py not found and $activate is missing." >&2
    echo "Install ESP-IDF v6.1: https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/get-started/" >&2
    echo "Or set IDF_ACTIVATE to the activation script of your installation." >&2
    exit 1
fi
# The activation script only works when sourced from an interactive-style shell
# name, so it runs in a fresh "bash -c" shell where $0 is "bash".
exec bash -c 'source "$1" >/dev/null && idf.py build' bash "$activate"
