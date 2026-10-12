# esp32-ntag424-terminal

ESP32-S3 firmware that writes a Secure Dynamic Messaging (SDM, also called SUN) URL into an NXP NTAG424 DNA tag. A PN532 NFC module does the writing over UART. After that, a phone that taps the tag reads a URL whose `picc_data` and `cmac` values change on every read, and a backend can verify them.

BLE (NimBLE) onboarding sets the WiFi credentials and the device id from a phone. A WS2812 LED and a buzzer show the device state. A button starts onboarding (hold 3 s) and factory-resets the device (hold 10 s).

The firmware targets ESP-IDF v6.1. It is a demo. Read [Security limitations](#security-limitations) before you use it for anything real.

## Status

This release builds and passes its checks, but it has not run on a board yet. An earlier version of the tag write worked on hardware. Since then the code around it changed: how the PN532 recovers from errors, which failures are retried, and how long each attempt waits. The bytes sent to the tag are the same. BLE onboarding, WiFi, the buzzer and storage changed too. Run [docs/hardware-test-runbook.md](docs/hardware-test-runbook.md), starting with the tag write (test 4d) and the phone read (test 5), before you rely on any of it, and open an issue with the log when a step fails.

## Hardware

| Part | Notes |
|------|-------|
| [ESP32-S3-DevKitC-1](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/index.html) | 8 MB flash, which `sdkconfig.defaults` sets together with the table in `partitions.csv` |
| PN532 NFC module with an HSU mode (the PN532 high-speed serial interface, a UART), see the [PN532 user manual](https://www.nxp.com/docs/en/user-guide/141520.pdf) | Powered at 3.3 V |
| [NTAG424 DNA tag](https://www.nxp.com/products/NTAG424DNA) | Must still have the factory all-zero AES key 0 |
| WS2812 LED | The DevKitC-1 has one on board, see the [WS2812B datasheet](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf) |
| Buzzer | Active or passive, set in menuconfig |
| Push button | Between the button GPIO and GND |

Details, wiring variants, and the optional battery divider are in [docs/hardware.md](docs/hardware.md).

### Wiring

The pins are the Kconfig defaults. Change them in menuconfig if your wiring differs.

| Signal | ESP32-S3 GPIO | Connects to | Option |
|--------|---------------|-------------|--------|
| PN532 UART TX | 17 | PN532 RX | `HAL_NFC_TX_GPIO` |
| PN532 UART RX | 18 | PN532 TX | `HAL_NFC_RX_GPIO` |
| Button | 4 | Button, other leg to GND | `HAL_BUTTON_GPIO` |
| WS2812 data | 48 | On-board LED | `HAL_INDICATOR_LED_GPIO` |
| Buzzer | 10 | Buzzer signal | `HAL_INDICATOR_BUZZER_GPIO` |
| Battery sense | 1 (ADC1 channel 0) | Divider midpoint | `HAL_POWER_ADC_CHANNEL` |

The PN532 also needs 3.3 V and GND, and its mode switches set to HSU. The PN532 UART is UART1 (`HAL_NFC_UART_NUM`) at 115200 baud, so UART0 stays free for the console. The battery sense pin is used only when `HAL_POWER_BATTERY_MONITOR` is on. The on-board LED is GPIO48 on DevKitC-1 v1.0 and GPIO38 on v1.1.

## Setup

Install ESP-IDF v6.1 with EIM, Espressif's installer.

1. Download `eim-cli` for your OS from the [EIM releases](https://github.com/espressif/idf-im-ui/releases).
2. Install ESP-IDF:

   ```sh
   eim install -i v6.1
   ```

3. Activate it in every new shell:

   ```sh
   source ~/.espressif/tools/activate_idf_v6.1.sh
   ```

Clone the repository, then fetch the PN532 library into `libs/ESP32-PN532`.

```sh
git clone https://github.com/OmerFarukOruc/esp32-ntag424-terminal.git
cd esp32-ntag424-terminal
git submodule update --init
```

Do not use `--recurse-submodules`. The PN532 library has a nested submodule for its PCB case whose pinned commit no longer exists upstream, so a recursive clone stops with an error. The firmware does not need it.

## Build, flash, monitor

```sh
./scripts/build.sh                      # same as idf.py build; activates ESP-IDF if idf.py is not on PATH
idf.py -p /dev/ttyUSB0 flash monitor    # use your serial port; Ctrl+] leaves the monitor
```

`./scripts/build.sh` initializes the submodule when it is missing and looks for `~/.espressif/tools/activate_idf_v6.1.sh`. Set `IDF_ACTIVATE` to use another activation script.

## Configuration

Run `idf.py menuconfig` and open **Component config**. `sdkconfig.defaults` holds the project defaults (ESP32-S3 target, 8 MB flash, NimBLE, 10 s task watchdog).

| Menu | Option | Default |
|------|--------|---------|
| HAL NFC | `HAL_NFC_UART_NUM` | 1 |
| HAL NFC | `HAL_NFC_TX_GPIO` | 17 |
| HAL NFC | `HAL_NFC_RX_GPIO` | 18 |
| HAL NFC | `HAL_NFC_SDM_BASE_URL` | `https://example.com/tap` |
| HAL button | `HAL_BUTTON_GPIO` | 4 |
| HAL button | `HAL_BUTTON_ACTIVE_LOW` | on |
| HAL button | `HAL_BUTTON_INTERNAL_PULLUP` | on |
| HAL button | `HAL_BUTTON_DEBOUNCE_MS` | 50 |
| Indicator (hal_indicator) | `HAL_INDICATOR_LED_GPIO` | 48 |
| Indicator (hal_indicator) | `HAL_INDICATOR_BUZZER_GPIO` | 10 |
| Indicator (hal_indicator) | `HAL_INDICATOR_BUZZER_TYPE` | `HAL_INDICATOR_BUZZER_ACTIVE` |
| Indicator (hal_indicator) | `HAL_INDICATOR_BUZZER_ACTIVE_LOW` | on |
| HAL power | `HAL_POWER_BATTERY_MONITOR` | off |
| HAL power | `HAL_POWER_ADC_CHANNEL` | 0 (GPIO1) |
| HAL time | `HAL_TIME_NTP_SERVER_PRIMARY` | `pool.ntp.org` |
| HAL time | `HAL_TIME_NTP_SERVER_SECONDARY` | `time.google.com` |

Set these before you flash.

- **`HAL_NFC_SDM_BASE_URL`** is the URL prefix written to the tag. Point it at the backend that serves the tap URL. Do not end it with a slash. It can be at most 139 characters, and the build fails when it is longer.
- **Pins.** Change the NFC, button, LED, and buzzer GPIOs when your wiring differs from the table above.
- **Buzzer.** Pick `HAL_INDICATOR_BUZZER_ACTIVE` or `HAL_INDICATOR_BUZZER_PASSIVE` to match your part. `HAL_INDICATOR_BUZZER_ACTIVE_LOW` applies to active buzzers only. Turn it on when the buzzer sounds with the pin at 0 V.
- **Button.** `HAL_BUTTON_INTERNAL_PULLUP` is offered only when `HAL_BUTTON_ACTIVE_LOW` is on, because the pull-up needs a button that reads low when pressed.
- **Battery.** Turn on `HAL_POWER_BATTERY_MONITOR` only on a board with the voltage divider wired to the sense pin. With the option off, the firmware does not start the monitor. A floating sense pin on a board without the divider would otherwise put the device to sleep after boot. A chip without ADC calibration data cannot start the monitor, and the device then ends in the error state.
- **NTP servers.** The build fails when `CONFIG_LWIP_SNTP_MAX_SERVERS` is below 2. `sdkconfig.defaults` sets it to 2. Leave `HAL_TIME_NTP_SERVER_SECONDARY` empty to use one server.

## Using the terminal

1. Hold the button for 3 s. The LED pulses blue and the device advertises over BLE as `TERMINAL-XXXX`.
2. Connect from a BLE client, pair, enable notifications, and optionally scan for networks.
3. Send the device id with URL_SET and hold the tag on the PN532. The device writes the SDM URL into the tag.
4. Send the WiFi credentials with WIFI_CONFIG. The device connects and returns to idle.

While the tag write runs, the device ignores the button, refuses a second URL_SET, and answers a WIFI_CONFIG with a failure result.

WIFI_CONFIG ends the onboarding session. When it starts, the device stops advertising. The client stays connected, so it receives the result, and the device returns to idle when the connect attempt ends. Send URL_SET first. To onboard again, hold the button for 3 s. The device then drops a client that is still connected and advertises again.

A short press or a 3 s hold leaves onboarding, and the device disconnects the client. Holding the button for 10 s erases NVS and restarts the device. The device refuses the 10 s reset while a tag write runs.

When a part fails to start at boot, for example the PN532 is not wired, the device ends in an error state with a blinking red LED and two long beeps. A short press returns it to idle.

## BLE onboarding protocol

The device advertises only in onboarding mode. The name is `TERMINAL-XXXX`, where `XXXX` is the last two Bluetooth MAC bytes in uppercase hex. The scan response carries the service UUID. The device accepts one connection. Payloads are compact UTF-8 JSON objects.

Service UUID: `67cb0001-25fe-42d1-9fce-8bf97986312f`

| Characteristic | UUID | Properties |
|----------------|------|------------|
| DEVICE_INFO | `67cb0002-25fe-42d1-9fce-8bf97986312f` | read |
| WIFI_SCAN | `67cb0003-25fe-42d1-9fce-8bf97986312f` | write, notify |
| WIFI_CONFIG | `67cb0004-25fe-42d1-9fce-8bf97986312f` | write, notify |
| URL_SET | `67cb0006-25fe-42d1-9fce-8bf97986312f` | write, notify |

### Pairing and writes

The three write characteristics need an encrypted link. The device pairs with LE Secure Connections using Just Works (no passkey) and stores the bond in NVS. DEVICE_INFO can be read without pairing.

- A write on an unencrypted link fails with an ATT error (ATT is the attribute protocol that GATT writes use). The code is `0x05` when the device has no stored key for the phone and `0x0F` when it has one. Both make the phone start pairing or encrypt the link, and the client repeats the write.
- Writes use the acknowledged ATT write, so the client sees errors.
- Enable notifications (the CCCD, the descriptor that switches notifications on) on a characteristic before you write to it. Without that, the device cannot send its reply and logs `Client has not enabled notifications on this characteristic`.
- Request an ATT MTU (the largest packet the link carries) of at least 100 bytes. A notification must fit in MTU minus 3 bytes. The device cannot start the MTU exchange itself. With the default 23-byte MTU it skips every scan entry and cannot send the `done` notification either, so a client that waits for `done` waits forever.
- A WIFI_CONFIG or URL_SET write may be at most 200 bytes.
- When pairing fails, the device ends the connection. When a phone pairs again after it lost its bond, the device drops its old bond and accepts the new pairing.

ATT errors on a write:

| Code | Name | Meaning |
|------|------|---------|
| `0x05` | Insufficient Authentication | The link is not encrypted, and the device has no stored key for the phone |
| `0x0F` | Insufficient Encryption | The link is not encrypted, and the device has a stored key |
| `0x0D` | Invalid Attribute Value Length | A length is wrong, see below |
| `0x13` | Value Not Allowed | The content is wrong, see below |
| `0x0E` | Unlikely Error | The device could not copy the write |

The device returns `0x0D` in these cases.

- The write is empty or longer than 200 bytes.
- WIFI_CONFIG has an empty `ssid`, an `ssid` over 32 bytes, or a `password` over 64 bytes.
- URL_SET has an empty `deviceId` or one over 31 bytes.

The device returns `0x13` in these cases.

- The write is not valid JSON.
- WIFI_CONFIG has no `ssid`, or an `ssid` or a `password` that is not a string.
- URL_SET has no `deviceId`, or one that is not a string.

### DEVICE_INFO

Read returns:

```json
{"name":"TERMINAL-84F2","deviceId":"84F2","version":"<firmware version>"}
```

`name` is the advertised name. `deviceId` is the device id without a prefix. Until URL_SET stores an id, the device id is the last two Bluetooth MAC bytes in uppercase hex. `version` is the firmware version, which comes from `git describe`.

### WIFI_SCAN

Write any bytes to start a scan. The device sends one notification per network:

```json
{"ssid":"MyNetwork","rssi":-52,"sec":"WPA2","ch":6}
```

`sec` is one of `OPEN`, `WPA2`, or `WPA3`. A mixed WPA/WPA2 network shows `WPA2` and a mixed WPA2/WPA3 network shows `WPA3`. The scan leaves out WEP, WPA-only, and enterprise networks, because the device cannot join them. A hidden network has an empty `ssid`. The device takes the 10 strongest networks the radio finds and drops the ones it cannot join, so the list has at most 10 entries.

A final notification ends the scan:

```json
{"done":true,"count":4,"skipped":0}
```

`count` is the number of network notifications sent. `skipped` is the number of networks whose JSON did not fit in MTU minus 3 bytes. A failed or empty scan sends only the terminator with `count` 0. The client treats the scan as complete when it sees `done`.

The device runs a scan only during onboarding. Outside onboarding it answers with the empty terminator at once, and it ignores a request while a scan runs.

### WIFI_CONFIG

Write the credentials:

```json
{"ssid":"MyNetwork","password":"secret"}
```

The `ssid` is 1 to 32 bytes. The `password` is empty or 8 to 64 bytes (64 bytes means a hex key). Leave it empty or omit it for an open network. A password of 1 to 7 bytes gets the failure notification at once, and the current connection stays up. With a password, the device joins only WPA2 and WPA3 networks, not WEP or WPA-only ones. After the connect attempt, the device sends one notification:

```json
{"success":true,"ip":"","msg":"WiFi connected successfully"}
{"success":false,"ip":"","msg":"WiFi connection failed"}
```

`ip` is always an empty string. The failure notification arrives when the WiFi driver rejects the credentials at once. It also arrives after 3 failed attempts that mean wrong credentials or no matching access point (a few tens of seconds), or after 10 failed attempts for any other reason (about 3 minutes). A short press that ends the attempt early sends no result. The WiFi driver stores the credentials in NVS, so the device reconnects after a reboot.

### URL_SET

Write the device id:

```json
{"deviceId":"SHOP_01"}
```

The id is 1 to 31 characters from `A-Z a-z 0-9 _ -`. The device checks the id first, and a bad character returns code 1006 in the result notification, not an ATT error. It then waits up to 15 s for a tag on the PN532. Each of up to 4 retries waits up to 5 s, and the write gives up after 30 s in total. A wrong key 0, a tag that is not an NTAG424 DNA, and a tag without the NDEF application end the write after the first attempt. Put the tag on the antenna before or right after the write. When the write succeeds, the device stores the id in NVS, and DEVICE_INFO reports it from then on.

The result arrives as one notification:

```json
{"success":true,"code":0,"message":"sdm_configured"}
```

| Code | `message` | Cause |
|------|-----------|-------|
| 0 | `sdm_configured` | The tag is configured |
| 1001 | `tag_not_detected` | All 5 attempts ended without a tag before the time limit. Rare, because the detect windows add up to more than 30 s (see 1005). |
| 1002 | `unsupported_tag` | The tag is not ISO 14443-4 (not an NTAG424 DNA) |
| 1003 | `pn532_unresponsive` | The PN532 did not answer after recovery |
| 1004 | `tag_auth_failed` | The tag rejected key 0 |
| 1005 | `sdm_timeout` | The 30 s limit passed, or a PN532 exchange timed out on the last attempt. A tag that is missing the whole time usually ends here, because the first 15 s window and the 5 s retry windows add up to more than 30 s. |
| 1006 | `invalid_device_id` | The id has a bad length or character |
| 1007 | `unsupported_tag_capability` | The tag lacks a required capability, for example the NDEF application |
| 1099 | `sdm_config_failed` | Any other failure, such as a framing error or an unexpected status from the tag |

Code 1099 also comes with `nfc_config_busy` (a tag write is already running), `not_onboarding` (the device left onboarding while the client stayed connected) and `nfc_config_task_create_failed` (out of memory). A WIFI_CONFIG sent during a tag write or outside onboarding gets a WIFI_CONFIG failure notification. The device sends no result when the client disconnects before the write ends, or when the client has not enabled notifications on URL_SET.

## SDM URL and backend verification

After URL_SET, the tag holds one NDEF URL record (NDEF is the standard format for data on NFC tags):

```
<HAL_NFC_SDM_BASE_URL>/<device_id>?picc_data=<32 hex>&cmac=<16 hex>
```

Each time a phone reads the NDEF file, the tag replaces the two placeholders:

- `picc_data` is the tag UID (its unique id) and a read counter, encrypted with AES-128.
- `cmac` is an 8 byte CMAC (a message authentication code built on AES) over the session, keyed from the same data.

The firmware enables UID and counter mirroring and sets key 2 as both the meta read and the file read key. Key 2 and key 0 keep the factory all-zero value. For the full protocol, see NXP [AN12196](https://www.nxp.com/docs/en/application-note/AN12196.pdf) and the [NT4H2421Gx datasheet](https://www.nxp.com/docs/en/data-sheet/NT4H2421Gx.pdf).

A backend verifies a request in these steps.

1. Decrypt `picc_data` with key 2 using AES-128-CBC with a zero IV (one block). The plaintext is `0xC7`, the 7 byte UID, the 3 byte read counter (little-endian), and random padding. Reject the request when the first byte is not `0xC7`.
2. Build the session vector `3CC300010080 || UID || counter` (16 bytes, counter as read from the plaintext). The session MAC key is the AES-CMAC of that vector under key 2.
3. Compute the AES-CMAC of the empty message under the session MAC key. The firmware sets the MAC input offset equal to the MAC offset, so nothing is MACed.
4. Keep bytes 1, 3, 5 up to 15 of the 16 byte result. This 8 byte value must equal `cmac`.
5. Accept the request only when the counter is greater than the last counter seen for that UID. The counter rises with every NDEF read, not with every tap. A phone that polls the tag reads many times, so do not require `last + 1`. The counter has 24 bits.

This Python function implements the steps. It needs the `cryptography` package, and the all-zero key stands for the factory key:

```python
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.primitives.cmac import CMAC

KEY = bytes(16)


def verify(picc_hex, cmac_hex, last_counter):
    dec = Cipher(algorithms.AES(KEY), modes.CBC(bytes(16))).decryptor()
    plain = dec.update(bytes.fromhex(picc_hex)) + dec.finalize()
    if plain[0] != 0xC7:
        return None
    uid, counter = plain[1:8], int.from_bytes(plain[8:11], "little")
    mac_key = CMAC(algorithms.AES(KEY))
    mac_key.update(bytes.fromhex("3CC300010080") + uid + plain[8:11])
    mac = CMAC(algorithms.AES(mac_key.finalize()))
    mac.update(b"")
    if mac.finalize()[1::2] != bytes.fromhex(cmac_hex) or counter <= last_counter:
        return None
    return uid.hex().upper(), counter
```

Check it against this test vector, which `openssl` reproduces too. With the all-zero key, `picc_data` `EF963FF7828658A599F3041510671E88` and `cmac` `94EED9EE65337086` give UID `04DE5F1EACC040` and counter 61:

```sh
KEY=00000000000000000000000000000000
echo -n EF963FF7828658A599F3041510671E88 | xxd -r -p \
  | openssl enc -aes-128-cbc -d -nopad -K $KEY -iv $KEY | xxd -p
# c704de5f1eacc0403d0000da5cf60941
SK=$(echo -n 3CC30001008004de5f1eacc0403d0000 | xxd -r -p | openssl mac -cipher AES-128-CBC -macopt hexkey:$KEY CMAC)
printf '' | openssl mac -cipher AES-128-CBC -macopt hexkey:$SK CMAC
# E194C7EE12D9F7EE8A65C8331B704386, and its bytes 1, 3, 5 ... 15 are 94EED9EE65337086
```

## Security limitations

This firmware is a demo. Treat it that way until per-tag keys exist.

- **Factory keys on the tag.** The tags keep NXP's factory all-zero AES-128 keys. The firmware authenticates with key 0 and never calls ChangeKey, because an interrupted key change can lock a tag for good. Anyone who knows this firmware can decrypt `picc_data` and forge a valid `cmac`. SDM then shows only that a tap happened, not that a genuine tag produced it. A tag whose key 0 was changed cannot be configured by this firmware.
- **Plain NVS.** NVS is not encrypted and flash encryption is off. The WiFi credentials, the BLE bond keys, and the device id sit in plain flash and can be read from the chip.
- **Just Works pairing.** BLE pairing has no passkey and no man-in-the-middle protection. A device in range that connects during onboarding can pair, then read DEVICE_INFO, scan, write WiFi credentials, and write the device id. Run onboarding only in a place you trust, and only for as long as it takes.
- **Open onboarding window.** The device advertises until a short press, a 3 s hold, or a WIFI_CONFIG ends it. Nothing times it out.

## Development

Install the hooks once:

```sh
pre-commit install
```

This installs the pre-commit, commit-msg, and pre-push hooks. The command below runs the pre-commit stage hooks by hand. The message lint runs on each commit, and the build runs on each push. Run the build by hand with `./scripts/build.sh`.

```sh
pre-commit run --all-files
```

| Hook | What it checks |
|------|----------------|
| `trailing-whitespace`, `end-of-file-fixer`, `mixed-line-ending`, `check-yaml`, `check-merge-conflict`, `check-executables-have-shebangs`, `check-added-large-files` | Basic file hygiene |
| `astyle_py` | C style with the same astyle options as ESP-IDF v6.1 |
| `check-copyright` | An Apache-2.0 SPDX header in each C source and header file |
| `codespell` | Spelling |
| `gitleaks` | Secrets in staged changes |
| `shellcheck` | Shell scripts |
| `check-kconfig-files`, `check-deprecated-kconfig-options` | Kconfig files |
| `cmake-lint` | CMake files |
| `conventional-precommit-linter` | The commit message (commit-msg stage) |
| `idf-build` | `scripts/build.sh` runs before every push (pre-push stage) |

Commit messages follow Conventional Commits, for example `fix(hal_ble): reject an empty ssid`. The subject needs at least 10 characters.

CI (`.github/workflows/ci.yml`) runs on pushes to `main`, on pull requests, and on tags that start with `v`. It has four jobs.

- `pre-commit` runs the pre-commit stage hooks except `gitleaks`.
- `gitleaks` scans the full git history.
- `build` runs `idf.py build` in the `espressif/idf:v6.1` container, fails when the build changes `dependencies.lock`, and uploads the firmware files.
- `publish` runs only for a `v*` tag, and only after the other three jobs pass.

For a `v*` tag, `build` also makes a merged binary named `esp32-ntag424-terminal-<tag>.bin` and packs the complete source including the submodule. `publish` adds `SHA256SUMS` and creates the GitHub release with generated notes. A tag with a hyphen, such as `v1.0.0-rc1`, becomes a pre-release.

Agent and contributor rules are in [AGENTS.md](AGENTS.md). The checklist for testing on real hardware is [docs/hardware-test-runbook.md](docs/hardware-test-runbook.md).

## License

The source in this repository is licensed under [Apache-2.0](LICENSE).

The firmware links the PN532 library in `libs/ESP32-PN532` ([RevK/ESP32-PN532](https://codeberg.org/RevK/ESP32-PN532)), which is licensed under GPL-3.0. A firmware binary built from this repository is therefore a combined work, and you distribute it under GPL-3.0. Anyone who receives the binary must also receive its complete source, including the library. Each release includes a source archive with the submodule for this reason.
