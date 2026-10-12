# Hardware

The parts, the wiring, and the module settings the firmware expects. Every pin below is a Kconfig default. Change it in `idf.py menuconfig` under **Component config** when your wiring differs. The names of the options are in the [README](../README.md#configuration).

## Parts

| Part | Role | Reference |
|------|------|-----------|
| ESP32-S3-DevKitC-1 | Microcontroller board | [User guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/index.html), [ESP32-S3 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-s3_datasheet_en.pdf) |
| PN532 NFC module | Writes the SDM URL into the tag over UART (HSU mode) | [PN532 user manual](https://www.nxp.com/docs/en/user-guide/141520.pdf) |
| NTAG424 DNA tag | Holds the URL and produces `picc_data` and `cmac` | [Product page](https://www.nxp.com/products/NTAG424DNA), [NT4H2421Gx datasheet](https://www.nxp.com/docs/en/data-sheet/NT4H2421Gx.pdf), [AN12196](https://www.nxp.com/docs/en/application-note/AN12196.pdf) |
| WS2812 LED | One status pixel | [WS2812B datasheet](https://cdn-shop.adafruit.com/datasheets/WS2812B.pdf) |
| Buzzer | Audio feedback, active or passive | Chosen in menuconfig |
| Push button | Onboarding and factory reset | Any momentary switch |
| Voltage divider (optional) | Battery sense | Two equal resistors |

The PN532 only writes the tag during onboarding. When a customer or a test phone taps the tag, the phone reads the tag directly, and the PN532 takes no part.

## Pin map

| Function | Default GPIO | Option |
|----------|--------------|--------|
| PN532 UART TX (ESP32 to PN532 RX) | 17 | `HAL_NFC_TX_GPIO` |
| PN532 UART RX (PN532 TX to ESP32) | 18 | `HAL_NFC_RX_GPIO` |
| Button | 4 | `HAL_BUTTON_GPIO` |
| WS2812 data | 48 | `HAL_INDICATOR_LED_GPIO` |
| Buzzer | 10 | `HAL_INDICATOR_BUZZER_GPIO` |
| Battery sense | 1 | `HAL_POWER_ADC_CHANNEL` (ADC1 channel 0) |

When you move a signal to another pin, avoid these on the DevKitC-1.

- GPIO0, GPIO3, GPIO45, and GPIO46 are strapping pins that the chip reads at reset.
- GPIO19 and GPIO20 are the native USB pins.
- GPIO26 to GPIO32 connect to the SPI flash. GPIO33 to GPIO37 are also taken on modules with octal flash or PSRAM.
- GPIO43 and GPIO44 are UART0, the console.

Check your board revision in the [DevKitC-1 user guide](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/index.html) before you rely on this list.

## PN532 in HSU mode

The firmware talks to the PN532 over UART1 (`HAL_NFC_UART_NUM`) at 115200 baud, which is the PN532 default for HSU.

```
ESP32-S3 DevKitC-1        PN532 module
3V3             ------    VCC
GND             ------    GND
GPIO17 (TX)     ------    RXD
GPIO18 (RX)     ------    TXD
```

- **Mode switches.** The module has two DIP switches that select the interface. On the module used during development, both switches down select HSU. Check the labels on your module, because some number the positions 0 and 1.
- **Pin labels.** Some modules print the I2C names on the header. In HSU mode the pin named SDA is the PN532 UART TX, and the pin named SCL is the PN532 UART RX. Connect GPIO18 (ESP32 RX) to SDA, and GPIO17 (ESP32 TX) to SCL.
- **Power.** Supply 3.3 V. The firmware drives the UART at 3.3 V logic.
- **Loose wires.** When the PN532 does not answer at boot, the log shows `Failed to initialize PN532` or a `PN532` line such as `SAMConfiguration fail`, and the device ends in the error state (red blinking LED and two long beeps). Check the four wires and the switches, then power-cycle the board.

Put the tag flat on the antenna of the module. The first detection can take a few seconds.

## Status LED

The LED is one WS2812 pixel driven through the RMT peripheral. The DevKitC-1 has one on board, so the default needs no extra part. The board revision decides the pin.

- DevKitC-1 v1.0 uses GPIO48.
- DevKitC-1 v1.1 uses GPIO38. Set `HAL_INDICATOR_LED_GPIO` to 38.
- On modules whose flash and PSRAM run at 1.8 V (for example the ESP32-S3R8V), GPIO47 and GPIO48 run at 1.8 V too. Check that your LED accepts a data high at that level.

For an external WS2812, connect its data input to the LED GPIO and share GND with the board. A WS2812 powered at 5 V may need a level shifter on the data line, because it expects a data high of about 0.7 times its supply voltage.

The firmware limits the LED to 50% brightness.

## Buzzer

Set the type in menuconfig (`HAL_INDICATOR_BUZZER_TYPE`) to match the part. The firmware drives the buzzer pin with LEDC PWM.

| Variant | Wiring | Settings |
|---------|--------|----------|
| Active module with a low-level trigger (3 pins: VCC, GND, I/O) | VCC to 3V3, GND to GND, I/O to the buzzer GPIO | `HAL_INDICATOR_BUZZER_ACTIVE`, `HAL_INDICATOR_BUZZER_ACTIVE_LOW` on (default) |
| Active buzzer that the GPIO powers directly | Buzzer + to the buzzer GPIO, buzzer - to GND | `HAL_INDICATOR_BUZZER_ACTIVE`, `HAL_INDICATOR_BUZZER_ACTIVE_LOW` off |
| Passive piezo (2 pins) | Piezo + to the buzzer GPIO, piezo - to GND | `HAL_INDICATOR_BUZZER_PASSIVE` |

An active buzzer has its own oscillator. It sounds while the pin is at its on level, and every sound has the same pitch, so the sounds differ only in length and count. A passive buzzer needs a tone. The firmware sends a 50% duty PWM at 2700 Hz for a beep and at 2000 Hz for the low battery warning.

A buzzer that sounds all the time after boot has the wrong polarity setting. Flip `HAL_INDICATOR_BUZZER_ACTIVE_LOW`. Only connect a buzzer to a GPIO directly when it draws a few milliamps. A larger buzzer needs a transistor.

The firmware plays these sounds.

| Sound | Pattern |
|-------|---------|
| Button hold accepted, or onboarding left | 1 very short beep (50 ms) |
| Onboarding started | 1 short beep (100 ms) |
| Success | 2 short beeps |
| Error state, tag write failure, factory reset | 2 long beeps (400 ms each) |
| Low battery | 3 warning beeps (200 ms each) |

## Button

The button connects the button GPIO to GND. The firmware enables the internal pull-up, so no external resistor is needed. Debounce is 50 ms. If the board has an external pull resistor, turn off `HAL_BUTTON_INTERNAL_PULLUP`. If the button reads high when pressed, turn off `HAL_BUTTON_ACTIVE_LOW` and add an external pull-down. menuconfig then hides `HAL_BUTTON_INTERNAL_PULLUP`, because the pull-up only works with a button that reads low when pressed.

## Battery divider (optional)

The battery monitor is off by default. Turn on `HAL_POWER_BATTERY_MONITOR` only when the divider is wired. Without a divider the sense pin floats, and a reading between the no-battery and the critical level would put the device into deep sleep after boot.

The firmware expects a 1:2 divider, so use two equal resistors:

```
Battery +  ---[ R ]---+---[ R ]---  GND
                      |
                      +--- GPIO1 (ADC1 channel 0)
```

- A full cell at 4.2 V gives 2.1 V at the pin.
- Pick a resistor value that keeps the divider current small and the ADC input stable. The firmware averages 16 samples to smooth the noise of a high-impedance divider.
- ADC1 channel N is GPIO N+1 on the ESP32-S3. Change `HAL_POWER_ADC_CHANNEL` to use another pin.
- Share GND between the battery, the divider, and the board.

The firmware reads the voltage only. Powering the board from a battery needs a charger and a regulator that this repository does not specify. A chip without ADC calibration data cannot start the monitor, and the device then ends in the error state.

The monitor samples every 30 s, and the first sample comes 30 s after boot. It classifies the battery voltage.

| Level | Voltage | Effect |
|-------|---------|--------|
| Critical | below 3100 mV | Needs two readings in a row, 30 s apart. Then stops BLE, WiFi, and SNTP and enters deep sleep. Only a reset or a power cycle wakes the device. A tag write that is running finishes first. |
| Low | 3100 to 3299 mV | Shows the low battery pattern (red, slow blink) and plays the warning beeps. During onboarding, a WiFi connect, or a tag write, it shows after the device returns to idle. |
| Medium | 3300 to 3599 mV | No effect |
| Normal | 3600 mV and above | No effect |

A reading below 1000 mV counts as no battery and reports Normal. The percentage is a linear estimate between 3000 mV and 4200 mV.
