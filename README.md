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
- `KISLAP_BURNT_LEDS` — per strand, the starting list of damaged pixels and
  which of their colors still work, e.g.
  `{ { 57, KISLAP_LED_R | KISLAP_LED_B }, { 980, KISLAP_LED_NONE } }`. Normally
  you manage this list over the REST API instead (`burntLeds`, below).

Customize these values in `src/config.h`, and keep `src/main.cpp` for your animation logic.

## REST API

The device serves a small JSON API on port 80 (`http://<device-ip>/api/...`).
All responses are `application/json`. Errors look like `{"error": "..."}`.

| Method | Path          | What it does                                              |
|--------|---------------|-----------------------------------------------------------|
| GET    | `/api/config` | Current settings                                          |
| POST   | `/api/config` | Change some settings; saves them and returns all settings |
| GET    | `/api/status` | Live telemetry                                            |
| GET    | `/api/schema` | Describes every setting, so a client can build its UI     |

### GET /api/config

Returns every runtime setting. Values from a freshly flashed device (the
compiled-in defaults):

```json
{
  "scheme": "halloween",
  "brightness": 128,
  "rippleSpeed": 2,
  "rippleWidth": 16,
  "rippleReturnRatio": 2,
  "rippleMinAmp": 4,
  "rippleMaxAmp": 10,
  "rippleMinIntervalFrames": 150,
  "rippleMaxIntervalFrames": 400,
  "rippleShade": 0.16,
  "rippleBaseLevel": 0.78,
  "maxRipples": 75,
  "stripeWidth": 20,
  "introStarEvery": 4,
  "introStarMin": 0,
  "frameDelayMs": 0,
  "halloweenSectionWidth": 40,
  "halloweenFadeWidth": 5,
  "lightningEnabled": true,
  "lightningMinIntervalMs": 3000,
  "lightningMaxIntervalMs": 7000,
  "lightningMinLength": 60,
  "lightningMaxLength": 200,
  "lightningAmbient": 48,
  "autumnLeafMinSize": 5,
  "autumnLeafMaxSize": 16,
  "autumnLeafRate": 0.8,
  "autumnFallMinFrames": 20,
  "autumnFallMaxFrames": 50,
  "autumnGustStrength": 2,
  "autumnGustMinIntervalFrames": 100,
  "autumnGustMaxIntervalFrames": 300,
  "autumnFlutter": 120,
  "autumnBackground": 60,
  "autumnGreenPercent": 8,
  "burntTolerance": 10,
  "zones": [
    { "start": 0,    "end": 639,  "length": 639, "introPixels": 110 },
    { "start": 639,  "end": 872,  "length": 233, "introPixels": 60 },
    { "start": 872,  "end": 1263, "length": 391, "introPixels": 100 },
    { "start": 1263, "end": 1521, "length": 258, "introPixels": 60 }
  ],
  "burntLeds": []
}
```

### POST /api/config

Send a JSON object with only the settings you want to change. Anything left
out stays as it is. Out-of-range values are clamped rather than rejected, and
values of the wrong type are ignored. The new settings are saved (they survive
a reboot), and the response is the full config, the same as a GET, showing
exactly what took effect after clamping.

`zones` is matched by position: element 0 is zone 0, and so on. Only the
elements and keys you send are changed. Only `length` and `introPixels` can be
set; `start` and `end` are worked out from them, and the last zone's length
always takes up whatever LEDs are left.

```sh
curl -X POST http://<device-ip>/api/config \
     -H "Content-Type: application/json" \
     -d '{"scheme": "autumn", "brightness": 300, "zones": [{"introPixels": 120}]}'
# -> full config, with "scheme": "autumn", "brightness": 255 (clamped), ...
```

`burntLeds` lists damaged LEDs and which of their colors still work. Unlike
`zones`, sending it **replaces the whole list** (send `[]` to clear it), so to
add one LED, GET the list, add to it, and POST it back. In each entry, a color
that's left out counts as dead, so `{"pixel": 980}` is a completely dead LED.
The list comes back sorted by pixel, with one entry per pixel (the last one sent
wins). It holds at most 64 LEDs (`KISLAP_MAX_BURNT_LEDS`).

A listed LED lights only when its working colors can show the color it's given,
and stays off otherwise. `burntTolerance` (0-100, default 10) is how much of a
dead color is ignored, as a percent of the color's strongest part. At 10, a red
with a trace of green (`0xC00700`) still shows on an LED whose green is dead,
but orange or white turns it off. At 0, any amount of a dead color turns it off.

```sh
curl -X POST http://<device-ip>/api/config      -H "Content-Type: application/json"      -d '{"burntLeds": [
           {"pixel": 57,  "red": true, "blue": true},
           {"pixel": 412, "green": true},
           {"pixel": 980}
         ]}'
```

The request is rejected with a 400 and nothing is changed if the body isn't a
JSON object, `scheme` isn't a known name, or `burntLeds` isn't an array of up
to 64 entries that each have a `pixel` on the strip:

```json
{ "error": "scheme must be \"patriotic\", \"halloween\" or \"autumn\"" }
```

### GET /api/status

Live telemetry. Not saved, and not settable.

```json
{ "fps": 58.7, "uptimeMs": 123456, "numLeds": 1521 }
```

- `fps`: frames rendered per second, measured over about the last second
- `uptimeMs`: milliseconds since boot
- `numLeds`: LEDs on the strip (compile-time)

### GET /api/schema

A description of every setting in `/api/config`, generated by the firmware from
the same table it uses to read, check and save those settings. A client can
build its whole settings page from this instead of keeping its own list of
fields. The response is about 9 KB. It is streamed out in small pieces, so it
needs only about 1 KB of free heap.

Top level:

- `schemaVersion`: version of this document's *shape*. It goes up only for
  breaking changes to the schema format. Adding settings, groups or schemes does
  not change it.
- `schemes`: selectable schemes, `{ name, label }`. `name` is the value used for
  `scheme` in `/api/config`.
- `groups`: how to group settings on screen, `{ id, label, scheme? }`. A group
  with a `scheme` only matters while that scheme is selected. A group without
  one always applies.
- `fields`: one entry per `/api/config` key, in `/api/config` order.

Each field has:

| Key       | Meaning |
|-----------|---------|
| `key`     | The exact JSON name used in `/api/config` |
| `label`   | Short display name |
| `group`   | A `groups[].id` |
| `type`    | `int`, `float`, `bool`, `enum`, `string` or `array` |
| `min`/`max` | Limits the device enforces (values outside are clamped). Left out when there is no limit |
| `step`    | Suggested UI step (`int`/`float` only) |
| `default` | Compiled-in default |
| `unit`    | `px`, `ms`, `frames`, `px/frame`, `leaves/frame`, or `""` |
| `help`    | One-sentence description, including any rule that involves another setting (e.g. "raised to Min gap if set lower") |
| `options` | `enum` only: the allowed values |
| `count`, `fixedCount`, `items` | `array` only: number of elements, whether that number can change, and the keys of each element (each with its own `type`). Item flags: `readOnly` (never settable), `readOnlyOnLast` (not settable on the last element) |
| `maxCount` | `array` with `fixedCount: false` only (e.g. `burntLeds`): most elements allowed. Such an array is replaced as a whole when sent |

Example (shortened; the real response lists every setting):

```json
{
  "schemaVersion": 1,
  "schemes": [
    { "name": "patriotic", "label": "Patriotic" },
    { "name": "halloween", "label": "Halloween" },
    { "name": "autumn",    "label": "Autumn" }
  ],
  "groups": [
    { "id": "general",   "label": "General" },
    { "id": "patriotic", "label": "Patriotic",    "scheme": "patriotic" },
    { "id": "ripples",   "label": "Wind ripples", "scheme": "patriotic" },
    { "id": "halloween", "label": "Halloween",    "scheme": "halloween" },
    { "id": "lightning", "label": "Lightning",    "scheme": "halloween" },
    { "id": "autumn",    "label": "Autumn",       "scheme": "autumn" },
    { "id": "burnt",     "label": "Burnt LEDs" }
  ],
  "fields": [
    { "key": "scheme", "label": "Scheme", "group": "general", "type": "enum",
      "options": ["patriotic", "halloween", "autumn"], "default": "halloween",
      "unit": "", "help": "Which look the strip shows; each scheme keeps its own settings while another one is showing." },
    { "key": "brightness", "label": "Brightness", "group": "general", "type": "int",
      "min": 0, "max": 255, "step": 1, "default": 128, "unit": "",
      "help": "Global LED brightness." },
    { "key": "rippleWidth", "label": "Ripple width", "group": "ripples", "type": "float",
      "min": 0.1, "step": 0.5, "default": 16, "unit": "px",
      "help": "Spread of a typical ripple; stronger ripples are drawn proportionally wider." },
    { "key": "lightningMaxLength", "label": "Max bolt length", "group": "lightning", "type": "int",
      "min": 2, "max": 1520, "step": 1, "default": 200, "unit": "px",
      "help": "Longest lightning bolt; raised to Min bolt length if set lower." },
    { "key": "zones", "label": "Zones", "group": "patriotic", "type": "array",
      "count": 4, "fixedCount": true,
      "items": [
        { "key": "start", "type": "int", "readOnly": true, "unit": "px", "help": "First LED of the zone (worked out from the zone lengths)." },
        { "key": "end", "type": "int", "readOnly": true, "unit": "px", "help": "One past the last LED of the zone (worked out from the zone lengths)." },
        { "key": "length", "type": "int", "readOnlyOnLast": true, "min": 1, "max": 1518, "step": 1, "unit": "px", "help": "LEDs in the zone; ..." },
        { "key": "introPixels", "type": "int", "min": 0, "max": 1518, "step": 1, "unit": "px", "help": "LEDs at the start of the zone that form the blue/star intro; capped at the zone's length." }
      ],
      "default": [
        { "length": 639, "introPixels": 110 }, { "length": 233, "introPixels": 60 },
        { "length": 391, "introPixels": 100 }, { "length": 258, "introPixels": 60 }
      ],
      "unit": "", "help": "Each zone covers one side of the house: ..." },
    { "key": "burntLeds", "label": "Burnt LEDs", "group": "burnt", "type": "array",
      "maxCount": 64, "fixedCount": false,
      "items": [
        { "key": "pixel", "type": "int", "min": 0, "max": 1520, "step": 1, "unit": "", "help": "Position of the LED on the strip, counting from 0." },
        { "key": "red", "type": "bool", "unit": "", "help": "Whether the LED's red still works (left out = dead)." },
        { "key": "green", "type": "bool", "unit": "", "help": "Whether the LED's green still works (left out = dead)." },
        { "key": "blue", "type": "bool", "unit": "", "help": "Whether the LED's blue still works (left out = dead)." }
      ],
      "default": [],
      "unit": "", "help": "Damaged LEDs and which of their colors still work; ..." }
  ]
}
```

### Adding a setting

Every setting is described once, in the `FIELDS` table in
`src/runtime_config.cpp`. That table drives `/api/config`, NVS load/save, the
range clamps and `/api/schema`. To add one, follow the checklist above
`struct Config` in `src/runtime_config.h`. If the table and `Config` get out of
step, the build fails.
