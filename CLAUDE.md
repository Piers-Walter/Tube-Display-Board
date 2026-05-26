# ESP32-4848S040 TubeStatus

London Underground status board running on the ESP32-4848S040 480×480 touch display using LVGL 9.2.

## Project structure

- `src/main.cpp` — complete application (all UI + data)
- `include/lv_conf.h` — LVGL config; Montserrat fonts 10–22 + 48 enabled
- `include/touch.h` — GT911 touch driver
- `include/ESP32_4848S040.h` — display init sequences
- `lib/Touch_GT911/` — GT911 Arduino library
- `platformio.ini` — ESP32-S3 build config (16MB QIO flash, PSRAM)

## Application overview

Three screens navigate via touch:

| Screen | Description |
|--------|-------------|
| **Home** | 4×3 grid of 12 tiles per page, sorted severe → minor → good. Swipe left/right to page through all 20 lines. Page dots at bottom when multi-page. Header shows clock + last-updated. |
| **Detail** | Large roundel badge, status pill, full disruption text for a single line. |
| **Settings** | Toggle which lines appear on the home grid. All/None shortcuts. |

## Key design decisions

- **Badge style**: Ring (hollow circle + horizontal name band) matching TfL roundel proportions
- **Status colours**: Dark grey = good, amber = minor delays, red = severe delays
- **Navigation**: `navigate_to()` builds fresh screens; old screen deleted asynchronously via `lv_async_call`
- **Clock**: Runs from millis() starting at 12:00 (no RTC/NTP in current stub build)
- **Line data**: All 12 lines hard-coded with TfL official hex colours

## TfL API (stubbed)

`fetch_tfl_status()` in main.cpp is a no-op placeholder. To wire up real data:

1. Add `WiFi.h`, `HTTPClient.h`, `ArduinoJson.h` to `platformio.ini` lib_deps
2. Connect WiFi in `setup()`
3. In `fetch_tfl_status()`, GET:
   ```
   https://api.tfl.gov.uk/Line/Mode/tube,dlr,elizabeth-line,tram,overground/Status
   ```
4. Parse `lineStatuses[0].statusSeverity`: 10 → GOOD, 7/9 → MINOR, ≤6 → SEVERE
5. Populate `line_statuses[]` and call `navigate_to(SCREEN_HOME)` to refresh

## Build

```bash
pio run --target upload
pio device monitor
```
