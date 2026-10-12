# Hardware test runbook

This runbook checks a flashed ESP32-S3 terminal on the bench, subsystem by subsystem. It matches the firmware in this repository on ESP-IDF v6.1. The log lines below are the messages in the code. In the monitor each line also carries a level letter, a timestamp, and the log tag, for example `I (1234) app_state: State: INIT -> IDLE`. This runbook shows the tag and the message, and `<n>` or `<SSID>` marks a value that changes. A line that the code logs through a check macro (for example `ESP_RETURN_ON_ERROR`) also has `<function>(<line>): ` before the message, so search for the message text.

Test 1 comes first. Run the destructive tests (11 and 12) last.

## Prerequisites

### Hardware

| Item | Notes |
|------|-------|
| ESP32-S3-DevKitC-1 | Flashed with the firmware |
| PN532 NFC module | HSU mode, wired as in [hardware.md](hardware.md) |
| NTAG424 DNA tag | With the factory all-zero keys, so a new tag |
| A second tag that is not an NTAG424 DNA | For the unsupported tag test (for example an NTAG213) |
| WS2812 LED, buzzer, button | Wired as in [hardware.md](hardware.md) |
| Phone with a BLE client | For example nRF Connect |
| Phone with an NFC tag reader app | Shows the URL record of a tag |
| WiFi network | A known SSID and password, and the option to switch it off |
| Optional | A bench supply and a divider for the battery tests |

### Software

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
./scripts/build.sh
idf.py -p /dev/ttyUSB0 flash monitor
```

Use your own serial port. Use `idf.py erase-flash` before the first run when the board held other firmware, so no old NVS data is left.

The default log level is INFO. Lines at DEBUG level, such as the WiFi config summary in `hal_ble`, do not appear.

### Pre-flight checklist

- [ ] PN532 wired to GPIO17 and GPIO18 with 3.3 V and GND, both mode switches set to HSU
- [ ] Button on GPIO4 and GND
- [ ] LED and buzzer set up to match menuconfig (type and polarity)
- [ ] `HAL_NFC_SDM_BASE_URL` set to the URL you want on the tag
- [ ] A new NTAG424 DNA tag within reach
- [ ] Serial monitor open at 115200 baud

## Test 1: Boot

Verifies that every HAL starts and the state machine reaches IDLE.

Power-cycle the board (unplug and replug USB). Expect these lines in this order. Other lines can appear between them, such as the `hal_ble` address line, lines from the `PN532` library, and the WiFi driver.

```
main: ESP32 NTAG424 terminal
main: Firmware <version>
hal_indicator: Initialized (LED=GPIO48, Buzzer=GPIO10)
app_state: Reset reason: 1
hal_diag: RTC history initialized
hal_diag: Boot #1, reset reason: 1
hal_nfc: Initializing PN532 via UART1 (TX=17, RX=18)
hal_nfc: NFC HAL initialized successfully
hal_network: No saved WiFi credentials found
hal_network: Initialized
hal_button: Initialized on GPIO4 (active_low=1, pullup=1, debounce=50ms)
hal_ble: NimBLE host task started
hal_ble: BLE HAL initialized, device name: TERMINAL-XXXX
app_url_set: No stored device id (namespace not created yet)
app_state: Application state machine initialized
app_state: State: INIT -> IDLE
app_state: Device idle, WiFi disconnected
main: Device ready
```

The last four lines can appear in another order, because the state task runs as soon as the init-complete event is queued. The boot count in `Boot #N` rises with each boot. Reset reason 1 is a power-on reset. After a software reset the second diagnostics line reads `RTC history recovered (<n> transitions, <n> tag-write attempts)`.

| Check | Expected | Status |
|-------|----------|--------|
| No error lines during init | No `E` lines from `app_state`, `hal_nfc`, `hal_ble`, or `main` | [ ] |
| Init order | The indicator comes first, then diagnostics, NFC, network, button, and BLE | [ ] |
| LED | White pulse, 2 s cycle (idle, WiFi disconnected) | [ ] |
| No boot beep | The buzzer stays silent | [ ] |
| State | `State: INIT -> IDLE` | [ ] |

When a peripheral init step fails, `app_state` logs `<step> init failed: <error>`, where the step is `NFC`, `Network`, `Button`, `BLE`, `Device id`, or `Power`. The other steps still run. Then `main` logs `Failed to initialize application: <error>`, the state goes `INIT -> ERROR`, and the device logs `Device in error state`. The LED blinks red and the buzzer plays two long beeps. A short press returns the device to IDLE.

When a core step fails, init stops. The log names the step (`Indicator init failed`, `NVS init failed`, `Diagnostics init failed`, `Event queue create failed`, or `State task create failed`) and `main` logs `Failed to initialize application: <error>`. The state stays INIT, and the LED blinks red when the indicator started.

| Failure line | Likely cause |
|--------------|--------------|
| `hal_nfc: Failed to initialize PN532` | PN532 wiring, mode switches, or power. Boot once with the PN532 unplugged to check the error state. |
| `hal_power: ADC calibration failed` | `HAL_POWER_BATTERY_MONITOR` is on and the chip has no ADC calibration data |
| `app_state: Abnormal reset detected (reason=N)` | The previous run crashed, see test 12 |
| `app_state: NVS needs an erase (...), stored settings are lost` | NVS held data from another version, the device erased it |

## Test 2: WiFi connection

Verifies that the device connects to a saved network, reconnects, and starts time sync. Run test 4 first to save credentials, then come back.

Reboot the device after onboarding saved the credentials. Expect:

```
hal_network: Saved WiFi '<SSID>' found, auto-connecting on boot
hal_network: Initialized
app_state: Device idle, WiFi connecting
hal_network: Got IP: <address>
hal_time: SNTP sync started
app_state: Device idle, WiFi connected
hal_time: Time synchronized: <date> <time> UTC
```

The `Got IP` line, the `SNTP` line, and the idle lines can appear in another order. When the IP address arrives before the state machine reaches IDLE, the first idle line already says `WiFi connected`.

| Check | Expected | Status |
|-------|----------|--------|
| WiFi connects | `hal_network: Got IP: ...` | [ ] |
| LED | Solid green | [ ] |
| SNTP starts and syncs | `SNTP sync started` then `Time synchronized: ... UTC` | [ ] |

### Reconnection

1. Switch the access point off.
2. Watch the log and the LED.
3. Switch it on again.

Expect `hal_network: WiFi disconnected, reason: <n>`, then `Reconnecting in 1000 ms (attempt 1)`, `Attempting WiFi reconnect...`, and the delay doubling up to 30 s. The device reports a drop as connecting first, and as disconnected only after 10 failed attempts, which takes about 3 minutes.

| Check | Expected | Status |
|-------|----------|--------|
| First drop | `app_state: Device idle, WiFi connecting`, LED blinks brown at 1 Hz | [ ] |
| After 10 failures | `hal_network: Many failures (10), reporting disconnected`, `Device idle, WiFi disconnected`, LED white pulse | [ ] |
| Retries continue | `Reconnecting in 30000 ms` keeps appearing | [ ] |
| Link returns | `Got IP`, `Device idle, WiFi connected`, LED green | [ ] |
| SNTP | Nothing stops SNTP on a disconnect. `SNTP sync started` appears again after each `Got IP`, with one `Time synchronized` and no `SNTP init failed` line | [ ] |
| Disassociation reason 8 | When the access point drops the device with reason 8 (for example from its admin page), the log shows `WiFi disconnected, reason: 8` and then `Reconnecting in <n> ms (attempt <n>)`, and the device reconnects | [ ] |
| No LED errors | No `LED update failed` line appears during the reconnects | [ ] |

## Test 3: Button

Verifies the debounce and the hold events on the button GPIO.

| Action | Expected log | Status |
|--------|--------------|--------|
| Press and release quickly | `app_state: Button pressed`, then `Button released (held <n> ms)` | [ ] |
| Hold 2 s, release | `Button hold: 1 s`, `Button hold: 2 s`, `Button released (held ~2000 ms)`, no state change | [ ] |
| Hold 3 s | `Button hold: 1 s`, `Button hold: 2 s`, then `Button hold: 3 s, BLE onboarding` (warning level). See test 4. | [ ] |

A press shorter than 1000 ms counts as a short press. A hold fires one event per second, from 1 s to 10 s. Contact bounce within 50 ms must not produce extra press lines.

The 10 s hold is in test 11.

## Test 4: BLE onboarding

Verifies advertising, pairing, WiFi scan, WiFi setup, and the tag write. Use the formats in the [README](../README.md#ble-onboarding-protocol).

### 4a: Enter onboarding

Hold the button for 3 s from idle. Expect:

```
app_state: Button hold: 1 s
app_state: Button hold: 2 s
app_state: Button hold: 3 s, BLE onboarding
app_state: State: IDLE -> ONBOARDING_BLE
hal_ble: BLE advertising started
app_state: BLE onboarding mode active
```

The last two lines can swap, because advertising starts on the BLE host task.

| Check | Expected | Status |
|-------|----------|--------|
| LED | Blue pulse, 1 s cycle | [ ] |
| Buzzer | One very short beep, then one short beep | [ ] |
| BLE client | `TERMINAL-XXXX` is visible, with service UUID `67cb0001-25fe-42d1-9fce-8bf97986312f` | [ ] |
| Leave onboarding | A short press logs `State: ONBOARDING_BLE -> IDLE` and `BLE advertising stopped`, and the name disappears. A 3 s hold does the same. | [ ] |
| Client dropped | With a client connected, leaving onboarding also logs `BLE disconnected, reason=<n>`, and the phone link ends | [ ] |
| Onboard again | A second 3 s hold advertises again, and the phone connects again | [ ] |

The device does not leave onboarding on its own. Enter it again before you continue.

### 4b: Connect, pair, and read DEVICE_INFO

1. Connect from the BLE client and request an MTU of at least 100.
2. Read DEVICE_INFO (`67cb0002-...`) before you pair.
3. Enable notifications on WIFI_SCAN, WIFI_CONFIG, and URL_SET.
4. Write any byte to WIFI_SCAN.

Expect `hal_ble: BLE connected, conn_handle=<n>`, `MTU update: conn_handle=<n>, mtu=<n>`, and one `Subscribe: conn_handle=<n> attr_handle=<n> notify:0->1` line for each characteristic you enabled. The write in step 4 fails with ATT error `0x05` (Insufficient Authentication) on a phone the device has no key for, or `0x0F` (Insufficient Encryption) on a phone it has a key for. The phone offers to pair, and after pairing the log shows `Encryption change: conn_handle=<n>, status=0`.

| Check | Expected | Status |
|-------|----------|--------|
| DEVICE_INFO without pairing | JSON with `name` (`TERMINAL-XXXX`), `deviceId`, `version` | [ ] |
| Id before URL_SET | `deviceId` is the last two Bluetooth MAC bytes in hex | [ ] |
| Write before pairing | ATT error `0x05` for a phone with no bond, `0x0F` for a phone with a bond the device kept | [ ] |
| Pairing | Just Works, no passkey prompt | [ ] |
| Write after pairing | Accepted | [ ] |
| Invalid JSON | A write of `abc` to WIFI_CONFIG or URL_SET fails with ATT error `0x13`, and the log shows `Write is not valid JSON (<n> bytes)` | [ ] |
| Missing field | `{"x":1}` to URL_SET fails with ATT error `0x13` and `URL_SET needs a string deviceId` | [ ] |
| No notifications | With notifications off on a characteristic, the write is accepted, no reply arrives, and the log shows `Client has not enabled notifications on this characteristic` | [ ] |
| Pairing failure | `Encryption failed, terminating connection` and a disconnect | [ ] |
| Repeat pairing | Delete the bond on the phone only, reconnect, pair again. The device accepts it. | [ ] |

### 4c: WiFi scan

Write any byte to WIFI_SCAN with notifications on. Expect:

```
hal_ble: WiFi scan requested via BLE
app_wifi_scan:   [1] <SSID> (ch:<n>, rssi:<n>, <security>)
app_wifi_scan: Scan complete, found <N> networks
hal_ble: Sent WiFi scan results: <N> networks, 0 skipped
```

| Check | Expected | Status |
|-------|----------|--------|
| Notifications | One JSON object per network, `{"ssid":"..","rssi":-52,"sec":"WPA2","ch":6}` | [ ] |
| Terminator | `{"done":true,"count":N,"skipped":0}` as the last notification | [ ] |
| Network limit | At most 10 networks, even in a crowded area | [ ] |
| Security text | Only `OPEN`, `WPA2`, and `WPA3` appear. A mixed WPA/WPA2 network shows `WPA2`, and a mixed WPA2/WPA3 network shows `WPA3`. | [ ] |
| Unjoinable networks | A WEP or WPA-only access point does not appear in the scan | [ ] |
| Repeated request | A second write while a scan runs logs `Scan request ignored: a scan is already running` | [ ] |
| Small MTU | Without an MTU request (23 bytes, so 20 per notification) every entry is skipped, and the terminator does not fit either. The log shows `Notification exceeds ATT MTU - 3 (20 bytes)`. With an MTU of 100 or more, every entry arrives. | [ ] |
| Connected | A scan while the device is connected keeps the link up, and no `Attempting WiFi reconnect...` line follows the scan | [ ] |
| Saved network out of range | A scan with a saved network that is out of range can log `Scan blocked by a connect attempt, retry <n> of 7`. The list still arrives. After the scan the log shows `Attempting WiFi reconnect...`, and the device connects without a reboot when the access point returns. | [ ] |
| No saved network | A scan with no saved credentials is not followed by a reconnect line | [ ] |
| Client problems | A client that never enabled notifications on WIFI_SCAN, or that disconnects during the scan, only causes log lines. A later scan works. | [ ] |

A scan that fails still ends with `{"done":true,"count":0,"skipped":0}` so the client stops waiting.

### 4d: URL_SET (tag write)

This is the main test. Put a new NTAG424 DNA tag on the PN532 antenna, then write `{"deviceId":"TEST123"}` to URL_SET with notifications on. Expect:

```
hal_ble: URL_SET: deviceId=TEST123
app_url_set: URL_SET attempt #1 start: device_id=TEST123
app_state: State: ONBOARDING_BLE -> WRITING_TAG
hal_nfc: SDM NDEF offsets: picc_data=41, mac_input=79, mac=79
hal_nfc: PN532 stage=soft_resync_begin last_err=<name>(<n>)
hal_nfc: PN532 stage=soft_resync_ok last_err=<name>(<n>)
hal_nfc: detect attempt 1: cards=<n>, pn532_err=<name>(<n>)
hal_nfc: Tag detected: ATQA=0x0344 SAK=0x20 type=<type> (<n> ms)
hal_nfc: AuthenticateEV2First successful with Key 0
hal_nfc: SDM config: NDEF URL written via DESFire WriteData (95 bytes)
hal_nfc: SDM enabled on File 02
hal_nfc: SDM timing: attempt=1 detect=<n>ms select=<n>ms write=<n>ms settings=<n>ms total=<n>ms
hal_nfc: NTAG424 DNA SDM configured successfully for device: TEST123
app_url_set: URL_SET attempt #1 done: sdm_configured (code 0, hal_nfc error 0), <n> ms
app_state: State: WRITING_TAG -> ONBOARDING_BLE
```

The tag write runs in its own task, so the `app_url_set` start line and the first `hal_nfc` lines can swap.

The offsets and the byte count are for the default base URL `https://example.com/tap` and the id `TEST123`. Another base URL or id gives other numbers. The `picc_data` offset is the position just after `picc_data=` and the `mac` offset the position just after `cmac=`, counted from the first byte of the NDEF file. `mac_input` equals `mac`, so the MAC covers no input bytes.

| Check | Expected | Status |
|-------|----------|--------|
| Id check | The id is validated before the tag tap, and a bad id returns at once | [ ] |
| State | `ONBOARDING_BLE -> WRITING_TAG` at the start and `WRITING_TAG -> ONBOARDING_BLE` at the end. The LED keeps pulsing blue during the write. | [ ] |
| Tag found | `Tag detected` within 15 s. The first detect can take a few seconds. | [ ] |
| Auth | `AuthenticateEV2First successful with Key 0` | [ ] |
| SDM enabled | `SDM enabled on File 02` | [ ] |
| LED and buzzer | Green solid and two short beeps | [ ] |
| Notification | `{"success":true,"code":0,"message":"sdm_configured"}` | [ ] |
| Stored id | The id is saved after the write succeeds. DEVICE_INFO now shows `TEST123` as `deviceId`. | [ ] |
| Repeat on the same tag | The device releases the tag after a successful write. Write a second id right away and expect success again with a clean detect. This is not confirmed on hardware. | [ ] |
| No notifications | With notifications off on URL_SET, the tag is still written, the LED and buzzer show success, and the log shows `Client has not enabled notifications on this characteristic` | [ ] |

#### Failures

| Trigger | Expected result | Status |
|---------|-----------------|--------|
| Id `bad id!` | Notification code 1006 `invalid_device_id` at once, log `URL_SET rejected: invalid device id (<n> bytes)`, no tag needed | [ ] |
| Empty id, or an id over 31 bytes | ATT error `0x0D` on the write, log `URL_SET deviceId has <n> bytes` | [ ] |
| No tag on the antenna | After about 30 s, code 1005 `sdm_timeout`, with `Tag detection timed out after <n> attempts` and `SDM config: Overall timeout reached before attempt <n>`. The 15 s first window and 5 s retry windows add up to more than 30 s, so the time limit ends the run before 5 attempts finish and code 1001 is unlikely. | [ ] |
| A tag that is not an NTAG424 DNA | Code 1002 `unsupported_tag` after one attempt (for example an NTAG215) | [ ] |
| A DESFire tag without the NDEF application | Code 1007 `unsupported_tag_capability` after one attempt | [ ] |
| PN532 TX wire unplugged after boot | Code 1003 `pn532_unresponsive` after several seconds, with `SDM config: PN532 unavailable after recovery` | [ ] |
| Wire plugged in again, same request | Success without a reboot, because the firmware re-initializes the PN532. Not confirmed on hardware. | [ ] |
| Second URL_SET while one runs | At once, code 1099 `nfc_config_busy`, log `URL_SET rejected: nfc_config_busy` | [ ] |
| WIFI_CONFIG during the write | Notification `{"success":false,"ip":"","msg":"WiFi connection failed"}`, log `WiFi config rejected: a tag write is running`, and the write goes on | [ ] |
| Disconnect the BLE client during the write | The write still ends, log `URL_SET attempt #<n>: BLE disconnected before the result, not sent` | [ ] |
| Short press or 3 s hold during the write | Nothing changes. The state stays WRITING_TAG until the write ends. | [ ] |
| Hold the button 10 s during the write | `Factory reset refused: a tag write is running` | [ ] |

A tag whose key 0 is not all zeros fails with code 1004 `tag_auth_failed` after one attempt. The log shows one `SDM config: authentication failed (<n>)` line and then `SDM config: Failed, last_err=<n>`.

### 4e: WIFI_CONFIG

Send `{"ssid":"<SSID>","password":"<password>"}` to WIFI_CONFIG. Expect:

```
hal_network: Connecting to SSID: <SSID>
app_state: State: ONBOARDING_BLE -> CONNECTING_WIFI
hal_ble: BLE advertising stopped
hal_network: Got IP: <address>
hal_time: SNTP sync started
app_state: State: CONNECTING_WIFI -> IDLE
app_state: Device idle, WiFi connected
```

The `BLE advertising stopped` line comes from the BLE host task, so it can appear before the state line.

| Check | Expected | Status |
|-------|----------|--------|
| Notification | `{"success":true,"ip":"","msg":"WiFi connected successfully"}` | [ ] |
| Onboarding ends | Advertising stops when the state becomes CONNECTING_WIFI. The phone stays connected until it disconnects, and the device returns to idle (LED green). | [ ] |
| Onboard again | A new 3 s hold drops the old phone link (`BLE disconnected, reason=<n>`) and advertises again | [ ] |
| Credentials twice | Onboard again while connected and send credentials a second time. The log shows no `Reconnecting` line from the old connection, and one clean connect follows. | [ ] |
| WPA3 | The device joins a WPA3 access point that allows only the H2E form of the SAE handshake, and a WPA2 access point | [ ] |
| Credentials persist | After a reboot the device logs `Saved WiFi '<SSID>' found, auto-connecting on boot` | [ ] |
| Open network | An empty or missing `password` joins an open network | [ ] |
| Empty `ssid` | ATT error `0x0D`, log `Invalid WiFi config: ssid 0 bytes, password <n> bytes` | [ ] |
| `ssid` over 32 bytes or `password` over 64 bytes | ATT error `0x0D` | [ ] |
| Missing `ssid`, or a `password` that is not a string | ATT error `0x13`, log `WiFi config needs a string ssid and, when present, a string password` | [ ] |
| Short press while connecting | Returns to IDLE and the connection continues. The LED pulses blue until the press, then blinks brown until the link is up. | [ ] |

#### Wrong password

Send credentials with a wrong password. Expect these lines, with the reason number depending on the access point:

```
hal_network: WiFi disconnected, reason: <n>
hal_network: Reconnecting in <n> ms (attempt <n>)
hal_network: New credentials failed 3 times (reason <n>), giving up
```

| Check | Expected | Status |
|-------|----------|--------|
| Notification | `{"success":false,"ip":"","msg":"WiFi connection failed"}` after 3 attempts, which takes a few tens of seconds | [ ] |
| After the failure | The device returns to idle and does not retry until a reboot or new credentials | [ ] |
| Reboot | The driver stored the failed credentials, so the device tries them again at boot | [ ] |
| Right password after the failure | Connects | [ ] |
| Access point lost after a working connection | The device keeps retrying with backoff and does not give up early (see test 2) | [ ] |

A password of 1 to 7 bytes gets the failure notification at once, the log shows `Password must be empty or 8 to 64 bytes`, and the current connection stays up. When the WiFi driver rejects the credentials at once, the failure notification also arrives at once and the log shows `WiFi connect failed to start: <error>`. Any failure that does not point at the credentials follows the 10 attempt rule and takes about 3 minutes.

## Test 5: Tag read by a phone

Verifies that the tag holds a working SDM URL. Use the tag from test 4d. The phone reads the tag directly. The PN532 is not involved.

1. Tap the phone to the tag and read the URL in the tag reader app.
2. Tap again and compare.

| Check | Expected | Status |
|-------|----------|--------|
| URL shape | `<HAL_NFC_SDM_BASE_URL>/TEST123?picc_data=<32 hex>&cmac=<16 hex>` | [ ] |
| No placeholders | `picc_data` and `cmac` are not all zeros. All zeros mean the offsets are wrong. | [ ] |
| Dynamic values | `picc_data` and `cmac` differ on every read | [ ] |
| Decrypt | `picc_data` decrypts with the all-zero key to `C7`, the tag UID, and a counter | [ ] |
| Counter | The counter rises on every read, and several reads per tap are normal | [ ] |
| MAC | The `cmac` matches the backend check in the README | [ ] |

Decrypt `picc_data` for a quick check:

```sh
echo -n <picc_data> | xxd -r -p \
  | openssl enc -aes-128-cbc -d -nopad -K 00000000000000000000000000000000 -iv 00000000000000000000000000000000 | xxd -p
```

The output has `c7` first, then 7 UID bytes, then 3 counter bytes (little-endian), then random bytes. The full verification steps and a test vector are in the [README](../README.md#sdm-url-and-backend-verification).

## Test 6: LED patterns

Verifies the patterns on the status LED. The LED runs at 50% brightness.

| State | LED | How to trigger | Status |
|-------|-----|----------------|--------|
| Idle, WiFi disconnected | White pulse, 2 s cycle | Boot without saved WiFi | [ ] |
| Idle, WiFi connected | Green solid | Connect to WiFi | [ ] |
| Idle, WiFi connecting | Brown blink, 1 Hz | Switch the access point off | [ ] |
| Onboarding | Blue pulse, 1 s cycle | Hold the button for 3 s | [ ] |
| Onboarding, tag written or WiFi connected | Green solid | Test 4d or a saved network | [ ] |
| Tag write failed | Red blink, 1 Hz | A failing URL_SET from test 4d | [ ] |
| Error | Red blink, 1 Hz | Boot with the PN532 unplugged, or see test 12 | [ ] |
| Low battery | Red blink, 0.25 Hz | Battery option on, see test 9 | [ ] |
| Factory reset | Red blink, 1 Hz for 1.5 s | Test 11 | [ ] |

The idle and onboarding patterns move. They must not freeze after one step.

## Test 7: Buzzer

Verifies the buzzer with your variant. An active buzzer plays one pitch, and the sounds differ in length and count. A passive buzzer plays 2700 Hz beeps and 2000 Hz for the low battery warning.

| Event | Sound | How to trigger | Status |
|-------|-------|----------------|--------|
| Hold accepted, or onboarding left | 1 very short beep | Hold 3 s, or press shortly in onboarding | [ ] |
| Onboarding started | 1 short beep | Hold 3 s | [ ] |
| Success | 2 short beeps | Tag write in onboarding (a WiFi connect has no beep) | [ ] |
| Error, tag write failure, factory reset | 2 long beeps | Test 4d failure, test 11 | [ ] |
| Low battery | 3 warning beeps | Test 9 | [ ] |

| Check | Expected | Status |
|-------|----------|--------|
| Silent at rest | No sound after boot, with the right polarity setting | [ ] |
| No driver errors | After a button press and a tag write, the log has no `Buzzer update failed` and no `Buzzer silence failed` line | [ ] |
| Your variant | Test each buzzer variant you have. The passive and the active-high variants were only compile-checked. | [ ] |

## Test 8: Event queue and tasks

Verifies that repeated work causes no crash. The stack sizes of the scan task (5120 bytes), the state task (8192), the tag write task (8192), and the monitor tasks are not measured.

Run these in one session without a reboot.

1. Run a WiFi scan 10 times.
2. Run URL_SET 10 times, mixing successes and failures.
3. Connect and disconnect the access point 5 times.

| Check | Expected | Status |
|-------|----------|--------|
| No panic | No `stack overflow` message, no `Guru Meditation Error`, no reboot | [ ] |
| No watchdog | No `Task watchdog got triggered` message | [ ] |
| Queue | No `Event queue full, dropping` line | [ ] |
| No error state | No `Event queue overflowed 3 times in a row, entering ERROR state` line, and no `Device in error state` line | [ ] |
| Soak | After 5 minutes idle and 5 minutes in onboarding the state is unchanged and the log shows no error | [ ] |

## Test 9: Battery monitor

The monitor is off by default. Skip this test unless you wired the divider.

First check the default build.

| Check | Expected | Status |
|-------|----------|--------|
| Option off | No `hal_power` lines at boot, and the board never enters deep sleep | [ ] |

Then turn on `HAL_POWER_BATTERY_MONITOR`, rebuild, flash, and wire the divider (see [hardware.md](hardware.md)).

```
hal_power: Initialized, battery: <n>% (<n>mV)
```

The monitor samples every 30 s. Raise or lower the supply to test the levels.

| Check | Expected | Status |
|-------|----------|--------|
| Voltage | The log and a multimeter agree within the ADC tolerance | [ ] |
| No battery | With the sense pin tied to GND (under 1000 mV) the level is Normal and nothing happens | [ ] |
| Low (3100 to 3299 mV) | `hal_power: Battery level changed: <old> -> 1`, `app_state: State: IDLE -> LOW_BATTERY`, `Low battery warning`, red slow blink and 3 beeps | [ ] |
| Low during onboarding | The warning waits. It shows as `State: ONBOARDING_BLE -> IDLE` followed by `State: IDLE -> LOW_BATTERY` after you leave onboarding. A low level during a tag write waits the same way. | [ ] |
| Leave low battery | A short press returns to IDLE | [ ] |
| One low sample | A single reading below 3100 mV followed by a normal one does not put the device to sleep | [ ] |
| Critical (below 3100 mV) | Two readings in a row, 30 s apart, give `hal_power: Battery level changed: <old> -> 0`, `Critical battery, entering deep sleep`, and `hal_power: Entering deep sleep`. BLE, WiFi, and SNTP stop first. Only a reset or a power cycle wakes the device. | [ ] |
| Critical during a tag write | `Deep sleep postponed: a tag write is running`. The device sleeps after the write result. | [ ] |

The level numbers in the log are 0 critical, 1 low, 2 medium, and 3 normal.

## Test 10: Time sync

Verifies that SNTP starts after WiFi connects.

| Check | Expected | Status |
|-------|----------|--------|
| Start | `hal_time: SNTP sync started` after `Got IP` | [ ] |
| Sync | `hal_time: Time synchronized: <date> <time> UTC` | [ ] |
| Reconnect | After a drop and a reconnect, `SNTP sync started` appears once more and the SNTP init and start steps log no error | [ ] |
| One server | With an empty `HAL_TIME_NTP_SERVER_SECONDARY` the sync still completes. The log does not show the server count. | [ ] |

## Test 11: Factory reset

Verifies that the 10 s hold erases NVS and restarts the device. The erase removes the WiFi credentials, the BLE bonds, the stored device id, and the boot count.

Hold the button for 10 s. The device enters onboarding at 3 s, because the 3 s event fires first. Expect:

```
app_state: Button hold: 3 s, BLE onboarding
app_state: Button hold: 9 s
app_state: Button hold: 10 s, factory reset
app_state: Factory reset: erasing NVS and restarting
```

The 10 s line is logged at error level, and the reset line at warning level.

| Check | Expected | Status |
|-------|----------|--------|
| Feedback | Red blink and two long beeps for 1.5 s | [ ] |
| Restart | A fresh boot, with `Reset reason: 3` (software reset) | [ ] |
| History | `hal_diag: RTC history recovered (<n> transitions, <n> tag-write attempts)`, because a software reset keeps the RTC memory | [ ] |
| Counter | `Boot #1` | [ ] |
| WiFi gone | `No saved WiFi credentials found` | [ ] |
| Id gone | `No stored device id (namespace not created yet)`, and DEVICE_INFO shows the MAC-based id again | [ ] |
| Bond gone | The phone must pair again | [ ] |
| In error and low battery states | The 10 s hold also resets, and a short press returns to IDLE | [ ] |

If the erase fails, the log shows `NVS erase failed: <error>` and the device goes to the error state.

## Test 12: Error recovery

Verifies how the device behaves after an abnormal reset.

The device counts a task watchdog, interrupt watchdog, other watchdog, or panic reset as abnormal. A brownout and a power-on do not count. The firmware has no trigger for such a reset, so this test needs a temporary code change.

1. In `button_callback` in `main/core/app_state.c`, add `abort();` as the first line of the `hold_time_ms == 3000` branch. `abort()` resets the device at once (reason 4, panic). For a task watchdog reset (reason 6) use `while (1) {}` there instead; that reset comes about 10 s after the hold.
2. Build, flash, and hold the button for 3 s.
3. Undo the change and flash again when the test is done.

After such a reset, expect:

```
app_state: Abnormal reset detected (reason=<n>)
app_state: State: INIT -> ERROR
app_state: Device in error state
```

The log also shows `hal_diag: Previous run: state <n> -> <n> (event <n>) at <n> ms` lines with the last transitions, and `Previous run: tag write #<n> ...` lines with the last tag writes. A brownout reset shows them too. The watchdog panics after 10 s, so a hung task resets the chip.

| Reset reason | Meaning | Abnormal |
|--------------|---------|----------|
| 1 | Power-on | No |
| 3 | Software reset | No |
| 4 | Panic | Yes |
| 5 | Interrupt watchdog | Yes |
| 6 | Task watchdog | Yes |
| 7 | Other watchdog | Yes |
| 8 | Deep sleep wake | No |
| 9 | Brownout | No |

| Check | Expected | Status |
|-------|----------|--------|
| Error state | Red blink and two long beeps | [ ] |
| Leave the state | A short press returns to IDLE | [ ] |

## Summary

### Critical path

| Test | Verifies | Status |
|------|----------|--------|
| 1 | Boot reaches IDLE | [ ] |
| 4d | The tag write: select, authentication, WriteData, ChangeFileSettings | [ ] |
| 5 | A phone reads dynamic `picc_data` and `cmac` from the tag | [ ] |

### Functional

| Test | Verifies | Status |
|------|----------|--------|
| 2 | WiFi connect and reconnect, SNTP | [ ] |
| 3 | Button press and hold events | [ ] |
| 4a to 4c, 4e | BLE advertising, pairing, scan, WiFi setup | [ ] |
| 6, 7 | LED patterns and buzzer | [ ] |
| 8 | Repeated work without a crash | [ ] |
| 10 | Time sync | [ ] |

### Last

| Test | Verifies | Status |
|------|----------|--------|
| 9 | Battery monitor (only with the divider) | [ ] |
| 11 | Factory reset | [ ] |
| 12 | Abnormal reset handling | [ ] |

## Log tags

| Tag | Component | Main lines |
|-----|-----------|------------|
| `main` | Entry point | Firmware version, `Device ready` |
| `app_state` | State machine | State changes, button lines, factory reset |
| `app_url_set` | Tag write job | Attempt start, result, stored id |
| `app_wifi_scan` | WiFi scan job | Networks found |
| `hal_nfc` | PN532 and NTAG424 | Detect, authentication, SDM steps, recovery |
| `PN532` | PN532 library | Low-level errors |
| `hal_network` | WiFi | Connect, disconnect, backoff, IP |
| `hal_ble` | BLE service | Advertising, connection, pairing, writes |
| `hal_indicator` | LED and buzzer | Init line, LED and buzzer errors |
| `hal_button` | Button | Init line |
| `hal_power` | Battery | Level changes, deep sleep |
| `hal_time` | SNTP | Start and sync |
| `hal_diag` | Post-mortem history | Boot count, previous run |
