# Agent notes

ESP-IDF v6.1 firmware for an ESP32-S3 that writes an NTAG424 DNA SDM URL through a PN532 and onboards over BLE. [README.md](README.md) has the setup, the BLE protocol, and the SDM URL format. Read the section you change before you edit code.

## Done means

A change is done when both commands pass in the worktree you changed.

```sh
./scripts/build.sh
pre-commit run --all-files
```

`./scripts/build.sh` needs ESP-IDF v6.1 and activates it when `idf.py` is not on PATH. The hooks are listed in `.pre-commit-config.yaml`, and `pre-commit run --all-files` runs only the pre-commit stage. The commit-msg hook rejects a commit message that is not Conventional Commits, and the build runs again on push. CI fails when the build changes `dependencies.lock`, so commit the refreshed file with any manifest change. Commit messages carry no tool attribution lines.

CI also builds each `sdkconfig.ci.*` file, which flips the options the default build leaves on one side of an `#if`. When you add an `#if CONFIG_...` branch, put the option that reaches it into one of those files, and build it locally with `idf.py -B build-<name> -D SDKCONFIG=build-<name>/sdkconfig -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.ci.<name>" build`. To find a build failure in a log, grep `': (error|warning):|^FAILED'`, because a bare `error` also matches object files such as `error.c.obj`.

Hardware behaviour cannot be checked in CI. Mark a claim about the PN532, the tag, BLE pairing, or timing as unverified until [docs/hardware-test-runbook.md](docs/hardware-test-runbook.md) has run on a board.

## Layout

- `main/core/` holds the state machine. `app_state_priv.h` is the interface between its files.
- `components/hal_*/` are the hardware layers. The header in `include/` owns the contract of each one, and `hal_ble.h` owns the BLE wire format. Change the header and the doc in the same commit.
- `components/pn532_driver/` builds `libs/ESP32-PN532` as a component. The library is a GPL-3.0 submodule. Leave it unmodified.
- Each component declares its options in its own `Kconfig`, shown in menuconfig under **Component config**. Project defaults live in `sdkconfig.defaults`, and the flash layout in `partitions.csv`.
- Keep every source file under 1000 lines so a reviewer can read it whole. Split by responsibility, as `hal_nfc` is split into reader, AES, DESFire, and SDM files.

## ESP-IDF v6.1 idioms

- Check errors with `esp_check.h`: `ESP_RETURN_ON_ERROR`, `ESP_RETURN_ON_FALSE`, and `ESP_GOTO_ON_ERROR` with a `TAG`. Return an `esp_err_t` and let the caller decide.
- List only the components a file uses in `PRIV_REQUIRES`. Since v6.0 the umbrella `driver` component no longer re-exports `esp_driver_uart` and `esp_driver_gpio`, so name them.
- The NimBLE host task, the system event task, `esp_timer` callbacks, the battery monitor, and the button task must return quickly. Do not wait on a queue, take a mutex, or sleep in them.
- A callback only posts to the state task queue with `app_post_event()`, which does not wait. The state task owns the state, the LED pattern, and the WIFI_CONFIG and URL_SET replies. Slow work runs in its own task. The tag write task reports back with `APP_EVENT_URL_SET_DONE`, which it posts with `app_post_event_wait()`. The WiFi scan task sends its BLE notifications itself.
- The task watchdog is 10 s with panic on, and it watches the idle tasks on both cores. No loop or wait may keep a task off the CPU near 10 s. The state task subscribes to the watchdog and resets it in its loop.
- Wipe credentials with `explicit_bzero()` as soon as the last reader is done. WiFi credentials pass through BLE writes, the event queue, and the connect call.
- Log with the file's `TAG` at a level that fits. DEBUG strings are not compiled in at the default level.

## PN532 and tag learnings

Each of these cost debugging time. Check them before you change the NFC code.

- **PN532 pins.** In HSU mode some modules print I2C names. The pin named SDA is the PN532 UART TX, and the pin named SCL is the PN532 UART RX. The mode switches are both down on the development module, and other modules label them differently.
- **Tag flow.** A plain NDEF write to an NTAG424 DNA fails with status `6982`. The flow is detect, select the NDEF application, `AuthenticateEV2First` with key 0, DESFire `WriteData`, then `ChangeFileSettings`. Send SELECT right after detect, because a late command makes the PN532 report status `0x0B`.
- **File settings bytes.** `SDMAccessRights` goes out least significant byte first, so meta read and file read key 2 is `0xFF 0x22`. `SDMEncOffset` and `SDMEncLength` are present only when bit 4 of `SDMOptions` is set, and the firmware uses `0xC1`, so it omits them. `SDMMACInputOffset` equals `SDMMACOffset`, so the MAC covers no input. The AN12196 example payload `4000E0C1F121200000430000430000` is a cross-check for the byte layout.
- **Offsets.** `PICCDataOffset` and `SDMMACOffset` count from byte 0 of the NDEF file, which is the length byte. They include the record header and the URI prefix substitution (`https://` becomes one byte).
- **One frame.** The NDEF file must fit one `WriteData` frame, so `HAL_NFC_SDM_BASE_URL` is at most 139 characters. A `_Static_assert` enforces it.
- **PN532 state.** A failed APDU can leave the PN532 in a protocol error state where every later command fails. `nfc_reader_recover()` first drains the link, sends the wake-up bytes, and pings. When that fails it deletes the UART driver and initializes the PN532 again. The SDM flow calls it before the first detect, before each retry, and before each SELECT retry. The detect loop calls it after repeated errors.
- **First detect.** Detection can take a few seconds when the tag arrives, and a tag that is already in the field is found on the first poll. The first detect window is 15 s for this reason.
- **Counter.** The tag raises the read counter on every NDEF read, not on every tap, because a phone polls. A backend accepts a counter greater than the last one it saw.
- **Debug a `picc_data`.** Decrypt it with the key and a zero IV. The plaintext is `C7`, the 7 byte UID, and the 3 byte counter (little-endian):

  ```sh
  echo -n <picc_data> | xxd -r -p | openssl enc -aes-128-cbc -d -nopad -K 00000000000000000000000000000000 -iv 00000000000000000000000000000000 | xxd -p
  ```

- **Buzzer.** An active buzzer has its own oscillator and plays one pitch. It sounds while the pin holds its on level, so the polarity option must match the circuit. A passive buzzer needs a PWM tone. Both are Kconfig choices, see `components/hal_indicator/Kconfig`.

## Keep docs true

- Update `README.md`, `docs/hardware.md`, and `docs/hardware-test-runbook.md` when you change a Kconfig option, a pin default, the BLE format, a result code, or a log line the runbook quotes.
- New source files get the Apache-2.0 SPDX header that `check-copyright` expects.
