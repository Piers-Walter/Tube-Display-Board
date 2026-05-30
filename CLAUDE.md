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

Screens navigate via touch:

| Screen | Description |
|--------|-------------|
| **Home** | Dynamic grid sorted severe → minor → good. Layout chosen by enabled-line count: 1→1×1, 2→1×2, 3–4→2×2, 5–6→3×2, 7–9→3×3, 10+→4×3 (with paging). Badge size scales with layout (70–160 px). Page dots at bottom when multi-page. Header shows clock + last-updated. |
| **Detail** | Large roundel badge, status pill, full disruption text for a single line. |
| **Settings** | Menu with three items: Lines, Wi-Fi, About. |
| **Settings → Lines** | Toggle which lines appear on the home grid. All/None shortcuts. |
| **Settings → About** | App name, "Powered by TfL Open Data" attribution, TfL Open Data URL. |

## Key design decisions

- **Badge style**: Ring (hollow circle + horizontal name band) matching TfL roundel proportions
- **Status colours**: Dark grey = good, amber = minor delays, red = severe delays
- **Navigation**: `navigate_to()` builds fresh screens; old screen deleted asynchronously via `lv_async_call`; `g_current_screen` tracks active screen so API refresh only rebuilds home
- **Clock**: NTP-synced via `configTzTime()`, Europe/London timezone (GMT/BST auto-switch). Falls back to millis-based clock until NTP syncs.
- **PSRAM**: Draw buffer and `line_statuses[]` array allocated from PSRAM via `ps_malloc`/`ps_calloc` in `setup()`
- **Line data**: All 20 lines (tube + DLR + overground + tram) with TfL official hex colours

## Line visibility persistence

Which lines are shown on the home grid is stored in NVS via the `Preferences` library (namespace `"lines"`, key `"mask"`).

- `save_line_prefs()` — packs `line_enabled[]` into a 32-bit bitmask and writes it. Called immediately on any toggle change (individual row, ALL, NONE).
- `load_line_prefs()` — reads the bitmask back and restores `line_enabled[]`. Called in `setup()` after the default all-enabled initialisation. If no key exists yet (first boot) the defaults are kept.

## WiFi credential persistence

Credentials are stored in NVS via the ESP32 `Preferences` library (namespace `"wifi"`, keys `"ssid"` / `"password"`).

- `load_wifi_credentials()` — called in `setup()`, populates `g_wifi_ssid` / `g_wifi_password` from NVS.
- `wifi_connect_and_save()` — called by the "Connect & Save" button. Attempts `WiFi.begin()` with up to 15 s timeout, writes NVS on success, then calls `configure_ntp()`.
- On boot, if saved credentials exist, `WiFi.begin()` is called in the background; no blocking wait.
- `WiFi.h` and `Preferences.h` are part of the ESP32 Arduino core.

## TfL API

`fetch_tfl_status()` calls the live TfL Unified API:

```
GET https://api.tfl.gov.uk/Line/Mode/tube,dlr,elizabeth-line,tram,overground/Status
```

- Uses `WiFiClientSecure` with `setInsecure()` (no cert pinning needed for a local display device)
- Uses `http.useHTTP10(true)` so ArduinoJson can stream the response without chunked-encoding framing
- Severity mapping: `>= 10` → GOOD, `7–9` → MINOR, `<= 6` → SEVERE
- `reason` field used as detail text if non-empty, otherwise `statusSeverityDescription`
- Triggered by an LVGL timer flag checked in `loop()` — first fire 8 s after boot, then every 60 s
- Reconnects WiFi automatically before fetching if the connection dropped

## NTP / Clock

- `configure_ntp()` calls `configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org", "time.google.com")`
- Called after successful WiFi connect (both at boot via auto-connect and via the WiFi config screen)
- `clock_str()` uses `getLocalTime()` with zero timeout; falls back to millis clock if not yet synced

## PSRAM usage

- `draw_buf` — LVGL render buffer (~46 KB), allocated with `ps_malloc()`
- `line_statuses[]` — 20 × ~400 bytes of status strings, allocated with `ps_calloc()`
- Both fall back to DRAM if PSRAM allocation fails

## Screenshot server (dev only)

Disabled by default. To enable, uncomment `-D SCREENSHOT_SERVER` in `platformio.ini` `build_flags`.

When enabled, a WebServer starts on port 80 once WiFi connects. Hit `http://<device-IP>/screenshot` in a browser to download the current screen as a 24-bit BMP.

- Uses `lv_snapshot_take()` (LVGL's snapshot API, also only compiled in when the flag is set)
- Converts RGB565 → BGR888 row-by-row; streams directly to the TCP client without a second PSRAM buffer
- Zero overhead in production: the entire feature is inside `#ifdef SCREENSHOT_SERVER` guards in both `main.cpp` and `lv_conf.h`

## Build

```bash
pio run --target upload
pio device monitor
```
