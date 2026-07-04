# Kislap

ESP32 custom firmware project scaffolded with PlatformIO.

## Getting started

1. Install PlatformIO in VS Code.
2. Open this folder in VS Code.
3. Build with `PlatformIO: Build` or run `pio run` from the terminal.
4. Upload with `PlatformIO: Upload` or run `pio run -t upload`.
5. Open the serial monitor with `PlatformIO: Monitor` or run `pio device monitor`.

## Notes

- Target board: `esp32dev`
- Framework: `arduino`
- Serial baud: `115200`

## Configuration

LED settings are centralized in `src/config.h`:

- `KISLAP_DATA_PIN` — data pin for your LED strip
- `KISLAP_NUM_LEDS` — count of LEDs
- `KISLAP_LED_TYPE` / `KISLAP_COLOR_ORDER` — LED chipset configuration
- `KISLAP_BRIGHTNESS` — default brightness

Customize these values in `src/config.h`, and keep `src/main.cpp` for your animation logic.
