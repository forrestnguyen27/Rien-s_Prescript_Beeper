# Prescript Beeper

ESP-IDF firmware for an ESP32, 128x64 SSD1306 OLED, TTP223 touch sensors, and passive buzzer. The firmware uses FreeRTOS tasks and queues for touch input, UI/timing, and prompt requests.

## Behavior

- On boot, a centered monochrome logo appears.
- First touch reveals a prompt character by character. Each character cycles through two random glyphs before settling on the correct character, with a quiet high-pitched tick on each change. The screen animation is designed to complete in under one second. If Wi-Fi and a DeepSeek API key are configured, a loading indicator appears while the prompt is fetched; without a connection or if the request fails, the built-in prompt is revealed instead.
- The next touch on the prompt sensor displays `._CLEAR` in a large centered font and plays three beeps.
- Every 10 minutes, the logo appears and the buzzer plays four beeps.
- Touch the pause sensor to show a large centered `paused` message and pause the 10-minute reminder. Touch it again to resume; the previous screen is restored and a fresh 10-minute interval starts.

The display driver includes both uppercase and lowercase glyphs. The boot and reminder logo uses the 64x64 source image at `docs/index-logo.png`, converted to a monochrome bitmap for the OLED.

## Wiring

Default pins are configured near the top of `main/PrescriptBeeper.c` for a classic ESP32 DevKit:

| ESP32 | Component |
| --- | --- |
| GPIO 21 | OLED SDA |
| GPIO 22 | OLED SCL |
| GPIO 18 | Prompt TTP223 OUT |
| GPIO 19 | Pause/resume TTP223 OUT |
| GPIO 23 | Passive buzzer positive, through a transistor driver if needed |
| 3V3 | OLED VCC and both touch sensor VCC pins |
| GND | OLED GND, both touch sensor GND pins, buzzer negative |

The OLED driver defaults to I2C address `0x3C`; change `OLED_I2C_ADDRESS` for a `0x3D` module. Use 3.3 V compatible modules and connect all grounds. TTP223 OUT should idle low and go high on touch. Use a transistor or MOSFET to drive a buzzer that needs more current than an ESP32 GPIO can supply.

## Configure Wi-Fi and DeepSeek

Run `idf.py menuconfig` and open **Prescript Beeper**. Set the Wi-Fi SSID and password and a DeepSeek API key. The default model is `deepseek-flash`; change it in menuconfig if needed. The API key and Wi-Fi password are stored in local `sdkconfig`, so keep that file private and out of source control.

DeepSeek requests use the HTTPS Chat Completions endpoint and Bearer API-key authentication. See the [DeepSeek Chat Completions API documentation](https://api-docs.deepseek.com/api/create-chat-completion/).

## Build and flash

From an ESP-IDF terminal, set your target and build:

```sh
idf.py set-target esp32
idf.py menuconfig
idf.py build
idf.py -p PORT flash monitor
```

Replace `PORT` with the board's serial port. For a different ESP32 variant, select its target and verify the GPIO assignments.

## Driver modules

- `driver/ssd1306.c` implements the 128x64 OLED and text rendering. `driver/index_logo_64.h` contains the monochrome bitmap generated from `docs/index-logo.png`.
- `driver/touch_buttons.c` polls and debounces both TTP223 inputs.
- `driver/buzzer.c` controls the passive buzzer with LEDC tones.
