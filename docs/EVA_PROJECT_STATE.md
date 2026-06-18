# EVA Project State

Last updated: 2026-05-22
Current checkpoint: weather scene implemented on `phase6_eva_weather` (renamed
from `phase5_wifi_v55` once the scope grew well beyond "Wi-Fi works")

This repository is the clean-room Eva rewrite on top of the vendor `lvgl_demo_v9`
base. The working rule is still the same: one feature at a time, then build,
flash, verify on the board, commit, and move on.

## What this firmware can do now

| Phase | Status | Notes |
| --- | --- | --- |
| 0 | done | New `9-Eva_Firmware-v2/` project, local BSP components, git init, pinned camera sensor component |
| 1 | done | White screen with a single blue oval rendered from a minimal `main.c` |
| 2 | done | Two idle eyes positioned on screen |
| 2.1 | done | TinyUSB CDC debug console, live `ESP_LOG*` output over USB, `status` and `log` commands |
| 2.2 | done | Display rotated clockwise for the current physical mount |
| 3 | done | Blink, saccades, `look_at`, emotion morph worker, baked eye sprites in PSRAM |
| 4 | done | Clock overlay, NVS timezone, gesture emotion switching |
| 5 | done | Hosted Wi-Fi + Wi-Fi status label; real time synced from HTTP `Date:` headers because SNTP resets this hosted-Wi-Fi stack |
| 6 | done | Full-screen dynamic weather canvas: sky gradients, clearoutside cloud layers, particles, lightning, CDC control, NVS persistence |
| 6.1 | done | Weather/cloud polish: cloud strips now morph and crossfade, low clouds have heavier bellies, and sun/moon visibility is less aggressively damped by thin high clouds |
| 6.2 | done | Weather clock polish: centered weather clock uses a real 144px LVGL font, deep stacked shadow, HH:MM in weather, HH:MM:SS in eyes, and IP is hidden only in weather |
| 7+ | next | Camera, motion, face detection, forecast scene, weather-reactive emotions |

The current on-board state should be:
- black background
- two blue animated eyes
- clock overlay at the top; weather mode centers the clock and hides the IP line
- weather mode uses a large real font and layered depth shadow instead of a fake outline
- weather scene toggled by long-press or vertical swipe
- USB CDC console active
- USB log output visible from `ESP_LOG*`

## Important invariants

- Keep `components/esp_cam_sensor/` at v1.2.1.
- Do not let `managed_components/` silently replace that with an older camera sensor.
- Do not call `bsp_extra_codec_init()`.
- Keep `CONFIG_LV_USE_PERF_MONITOR=n`.
- Keep `CONFIG_LV_USE_SYSMON=n`.
- Keep the `flash.sh` DTR/RTS auto-download path intact.
- Treat `common_components/` as vendored local BSP source.

## Important files

- `main/main.c`: app startup, USB console, clock, eyes, display rotation, commands
- `main/eva_scene.c`: eyes/weather scene state machine and transition worker
- `main/eva_weather.c`: weather state, forward-compatible NVS blob restore, update callback
- `main/eva_weather_canvas.c`: horizontal weather canvas; renders 400x240 RGB565 in PSRAM, caches Fibonacci cloud stamps/background layers, upscales to 800x480, and logs tick/work timing
- `main/eva_font_uk_22.c`: compact Cyrillic LVGL font for Ukrainian weather captions
- `main/eva_font_clock_144.c`: digits + colon LVGL font for the oversized weather clock
- `main/weather_fetch.c`: clearoutside.com HTTP scrape for clouds, fog, visibility, precip, wind, sun, and moon data
- `main/eva_wifi.c`: hosted Wi-Fi, HTTP-Date time sync, exported connected wait helpers
- `flash.sh`: runtime CDC to ROM download handoff and flashing
- `components/esp_cam_sensor/`: pinned camera sensor component
- `common_components/espressif__esp32_p4_function_ev_board/`: local board BSP
- `common_components/espressif__esp_lcd_st7701/`: local LCD driver copy
- `README.md`: short pointer to this handoff file

## How to build and flash

```sh
idf.py build
./flash.sh
```

The script usually moves between:
- runtime CDC: `/dev/cu.usbmodem1234561`
- ROM download port after reset: `/dev/cu.usbmodem21101`

## USB console commands

- `help`
- `time`
- `tz`
- `tz <posix-tz-string>`
- `emotion`
- `emotion <idle|happy|curious|angry|sad|love|sleep>`
- `scene`
- `scene <eyes|weather>`
- `weather`
- `weather <clear-day|clear-night|partly-cloudy-day|partly-cloudy-night|cloudy|fog|rain|snow|thunderstorm|sleet|hail> <temp_c> "Description"`
- `weather refresh`
- `weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>`
- `weatherdebug <clear-day|clear-night|partly-cloudy-day|partly-cloudy-night|cloudy|fog|rain|snow|thunderstorm|sleet|hail> <frame>`
- `clockoffset <hours>`
- `status`
- `log`
- `log <none|error|warn|info|debug|verbose>`

`status` reports USB console state, clock sync state, scene, emotion, weather,
wind, cloud layers, fog, precipitation, sun/moon fields, timezone, log level,
and uptime.
`log <level>` changes the global log level for `ESP_LOG*`.

## Recent UI notes

- Weather descriptions now classify `Мінлива хмарність` more conservatively so nearly clear skies can stay `Ясно` when they only have thin or high cloud cover.
- Weather sun/moon rendering treats high clouds more lightly, so the luminary is still visible in partially clear skies.
- The weather clock shadow is built from stacked same-position layers with increasing scale and falling opacity, so it reads as depth rather than a cast shadow.
- `weatherdebug` uses its `frame` argument as a repeatable cloud-variation seed, and `clockoffset` is a temporary visual offset for scene/clock screenshots.
- Wi-Fi IP is shown in eyes mode and hidden in weather mode only.

## Backup snapshot

This checkpoint has a build snapshot saved outside the repo at:

`/Users/b42h/Desktop/JC4880P443C_I_W/9-Eva_Firmware-v2-snapshots/phase2-3389a5c-2026-05-14/`

## Changelog

| Commit | What changed |
| --- | --- |
| `915ba9c` | Vendor `lvgl_demo_v9` boots cleanly |
| `4a171d3` | USB auto-flash restored, boot clock added |
| `73a916f` | Two-eye idle pose added |
| `07a6076` | USB debug console, live logs, and CDC commands added |
| `3389a5c` | Display rotated clockwise for the current mounting direction |
| working tree | Full-screen weather canvas added: dynamic sky, particles, scene swap, clearoutside fetch, NVS, CDC commands |

## Resume point

The current firmware should be built, flashed, and smoke-tested after each
weather-canvas change. Next useful work after this checkpoint: visual QA polish,
repeated scene-toggle stress testing, and long offline/reconnect validation for
the weather fetch task.
