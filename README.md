# Anti-Theft Bag System

An ESP32 prototype that detects a bag opening while armed, sounds a local buzzer, reads a GPS receiver, and queues Telegram alerts over Wi-Fi. This repository packages the existing Arduino firmware without changing its source or including private credentials.

## Implemented features

- 4×4 keypad input, PIN entry clearing, and overflow rejection.
- Arming only when the reed switch indicates a stable closed bag; 50 ms nonblocking switch debounce.
- Latched audible alarm: closing the bag does not silence it; a correct PIN disarms it.
- GPS parsing with TinyGPSPlus; only fixes less than five seconds old are treated as current.
- Serial status and Google Maps location links, with one deferred location report when a fresh fix arrives.
- Telegram theft alerts and a conditional fresh-location follow-up, sent by a separate FreeRTOS task with bounded retries.
- HTTPS certificate-bundle verification and clock synchronization before Telegram requests.

## Technologies and components

Arduino C++, ESP32 Arduino core, FreeRTOS, Keypad, TinyGPSPlus, ArduinoJson, WiFi, NetworkClientSecure, and HTTPClient. The existing build script targets an ESP32 DOIT DevKit V1 (`esp32:esp32:esp32doit-devkit-v1`) using ESP32 core 3.3.12.

The code expects a 4×4 keypad, reed switch with magnet, active-high buzzer, and UART GPS receiver. Exact GPS and buzzer hardware models are not identified in the firmware.

## Pin connections supported by the source

| Connection | ESP32 GPIO / setting |
| --- | --- |
| Keypad rows, top to bottom | 33, 32, 27, 26 |
| Keypad columns, left to right | 25, 23, 19, 18 |
| Reed switch input | 35; external 10 kΩ pull-up to 3.3 V, reed to GND |
| Active-high buzzer control | 14 |
| GPS transmit signal → ESP32 receive | 16; UART2, 9600 baud, 8N1 |
| ESP32 GPS transmit | Disabled |

The source specifies these signals, not a complete power or driver circuit. Confirm peripheral voltage and current requirements against the actual hardware before wiring.

## Setup

1. Install Arduino IDE and ESP32 board support. Use ESP32 core 3.3.12 to match the existing build environment.
2. Install the Keypad, TinyGPSPlus, and ArduinoJson libraries. ArduinoJson must support `JsonDocument` (version 7).
3. Open `firmware/KeypadOnlyTest/KeypadOnlyTest.ino` and select ESP32 DOIT DevKit V1 and the appropriate serial port.
4. Copy `Secrets.example.h` to `Secrets.h` in that sketch folder. Fill it with your local Wi-Fi SSID/password, Telegram bot token, and destination chat ID. `Secrets.h` is ignored by Git. Start a chat with your bot and obtain the destination ID before using notifications.
5. Compile and upload. The Telegram source expects the ESP32 core's embedded X.509 certificate bundle symbols; builds with other core configurations may require bundle configuration.
6. Open Serial Monitor at 115200 baud. Wi-Fi, synchronized time, and Telegram access are required for online delivery. Local alarm handling runs separately from the network task.

## Usage

- Keep the magnet near the reed switch, wait for a stable closed reading, and press **A** to arm.
- Move the magnet away to trigger the latched buzzer and queue an alert.
- Enter the firmware's demonstration PIN followed by **#** to disarm. Press **\*** to clear an incomplete entry.
- Watch Serial Monitor for alarm, GPS, and Telegram delivery status. GPS acquisition may require an outdoor view of the sky.

## Known limitations

- The existing firmware has a fixed demonstration PIN and prints disarming instructions to Serial. It has no PIN provisioning or attempt lockout. Replace that design before relying on it for security.
- Telegram delivery is best effort: Wi-Fi, Internet, clock synchronization, API availability, and queue/retry limits can prevent delivery. A queued primary alert is not cancelled by disarming; a follow-up checks the active alarm.
- Live coordinates appear on Serial and are transmitted to the configured Telegram chat. No historical location data, serial logs, private JSON configuration, or compiled firmware is distributed here.
- No battery management, enclosure design, cellular connectivity, remote dashboard, or certified theft prevention is implemented in the supplied files. These are possible future work, not current features.
- This packaging has not been tested on connected hardware. Previous local test records are excluded because they may contain location or other private data.
- A fresh compile with placeholder credentials was attempted using the installed ESP32 3.3.12 toolchain. It failed with `bits/c++config.h: No such file or directory`; a successful build is not claimed. Repair the local toolchain/include-path setup and compile again before hardware use.

## Repository contents

`firmware/KeypadOnlyTest/` contains the three original source files and a placeholder-only configuration example. The original working project and private configuration are preserved separately. No Git history was found in the source workspace.
