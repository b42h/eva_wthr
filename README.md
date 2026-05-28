# eva_wthr

Weather-only firmware for the Guition `JC4880P443C_I_W` / `ESP32-P4 + ESP32-C6` board.

This is the final working `phase8_finished` build, separated into its own repository folder for people who are specifically looking for firmware for this P4+C6 display board.

## Naming Note

This repository folder is named `eva_wthr`, but the original project name was `Eva`. That is why the firmware, binary, source files, logs, and commands still use names like `eva_weather`, `eva_*`, and `Eva WEATHER`. They are left as-is because this is the tested working naming from the final firmware.

## What This Firmware Does

- Boots directly into an animated 800x480 procedural weather scene.
- Uses the ESP32-C6 as hosted Wi-Fi through `esp_hosted`.
- Fetches weather from Open-Meteo and Clear Outside.
- Syncs time over HTTP Date headers instead of SNTP, because SNTP was unstable on this hosted Wi-Fi stack.
- Stores timezone and last weather state in NVS.
- Exposes a TinyUSB CDC console for status, testing, screenshots, and manual weather overrides.
- Includes the board support components and tuned `sdkconfig.defaults` needed for this hardware.

It does not include the older "eyes" scene. This repository is the weather firmware only.

## Hardware

Tested on:

- Board: Guition `JC4880P443C_I_W`
- Main MCU: `ESP32-P4`
- Wi-Fi coprocessor: `ESP32-C6`
- Display: 800x480 RGB/MIPI panel through the vendor BSP
- Flash config used here: 16 MB, QIO
- USB: TinyUSB CDC on the P4

Important P4+C6 hosted Wi-Fi settings are already in `sdkconfig.defaults`:

- `CONFIG_ESP_HOSTED_ENABLED=y`
- `CONFIG_ESP_HOSTED_IDF_SLAVE_TARGET="esp32c6"`
- SDIO pins: CMD `19`, CLK `18`, D0 `14`, D1 `15`
- C6 reset GPIO: `54`
- SDIO clock: `40000` kHz

## Required Toolchain

Use ESP-IDF `v5.5.4`.

```sh
cd /path/to/eva_wthr
source "$HOME/.espressif/v5.5.4/esp-idf/export.sh"
idf.py set-target esp32p4
idf.py build
```

The app binary is generated as:

```text
build/eva_weather.bin
```

## Configure Your Device

Before flashing, set your Wi-Fi and location.

Fast path:

```sh
idf.py menuconfig
```

Then open:

```text
Eva Weather
```

Set:

| Option | Meaning |
| --- | --- |
| `CONFIG_EVA_WIFI_SSID` | Your Wi-Fi network name. |
| `CONFIG_EVA_WIFI_PASSWORD` | Your Wi-Fi password. Do not commit your real value. |
| `CONFIG_EVA_WEATHER_LATITUDE` | Latitude for Open-Meteo and Clear Outside. |
| `CONFIG_EVA_WEATHER_LONGITUDE` | Longitude for Open-Meteo and Clear Outside. |

You can also edit these defaults in `sdkconfig.defaults` before the first build:

```text
CONFIG_EVA_WIFI_SSID="YOUR_WIFI_SSID"
CONFIG_EVA_WIFI_PASSWORD="YOUR_WIFI_PASSWORD"
CONFIG_EVA_WEATHER_LATITUDE="50.447914"
CONFIG_EVA_WEATHER_LONGITUDE="30.522192"
```

Those defaults point to Khreshchatyk Street in central Kyiv.

The default timezone is stored in `main/eva_settings.c`:

```c
EET-2EEST,M3.5.0/3,M10.5.0/4
```

That is Ukraine summer/winter time. You can change it at runtime from the CDC console:

```text
tz UTC0
tz EET-2EEST,M3.5.0/3,M10.5.0/4
```

The value is saved to NVS.

## Flash

Automatic flash from the project root:

```sh
./flash.sh
```

Or pass the current runtime CDC port:

```sh
./flash.sh /dev/cu.usbmodem1234561
```

The script toggles DTR/RTS on the running CDC port, waits for the ROM USB port, then flashes with:

```sh
python -m esptool --chip esp32p4 -p <ROM_PORT> -b 460800 \
  --before no_reset --after hard_reset write_flash @flash_args
```

Manual recovery if auto-flash does not catch the port:

1. Hold `BOOT`.
2. Tap `RST`.
3. Release `BOOT`.
4. Find the ROM port:

```sh
ls /dev/cu.usbmodem*
```

5. Flash from the build directory:

```sh
cd build
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem21101 -b 460800 \
  --before no_reset --after hard_reset write_flash @flash_args
```

## First Boot Check

Open the monitor:

```sh
idf.py -p /dev/cu.usbmodem1234561 monitor
```

Expected boot banner:

```text
Eva WEATHER v1 built <date> <time>
```

Useful console commands:

```text
whoami
status
perf
time
tz
weather
weather refresh
screenshot
log info
```

`whoami` should return:

```text
weather
```

`status` shows Wi-Fi, time, and weather fetch state. `perf` prints render timing. `weather refresh` forces both weather providers to fetch immediately.

## Manual Weather/Test Commands

Set a simple weather state:

```text
weather rain 12 "Test rain"
weather cloudy 8 "Cloudy"
weather thunderstorm 18 "Storm"
```

Supported weather kinds:

```text
clear-day
clear-night
partly-cloudy-day
partly-cloudy-night
cloudy
fog
rain
heavy-rain
snow
thunderstorm
sleet
hail
```

Set raw cloud/precipitation fields:

```text
weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>
```

Example:

```text
weatherraw 20 60 90 85 0 3 12
```

Debug deterministic render variants:

```text
weatherdebug rain 42
```

`frame` works as a repeatable seed for cloud variation.

## Important Files

| Path | Purpose |
| --- | --- |
| `main/main.c` | App entry, CDC command handling, test panel. |
| `main/eva_weather_canvas.c` | Procedural weather renderer. |
| `main/eva_wifi.c` | Hosted Wi-Fi setup and HTTP time sync. |
| `main/weather_fetch.c` | Dual-source weather coordinator. |
| `main/weather_fetch_openmeteo.c` | Open-Meteo API provider. |
| `main/weather_fetch_clearoutside.c` | Clear Outside HTML provider. |
| `main/eva_settings.c` | NVS timezone default and persistence. |
| `main/Kconfig.projbuild` | User-editable Wi-Fi/location project config. |
| `sdkconfig.defaults` | Known-good P4+C6/LVGL/TinyUSB/hosted-Wi-Fi defaults. |
| `partitions.csv` | Partition table. |
| `flash.sh` | Auto-flash helper for the P4 USB CDC/ROM port flow. |
| `tools/eva-screenshot.py` | Screenshot helper through CDC. |

## Notes For GitHub Users

- Do not commit your real Wi-Fi credentials in `sdkconfig`, `sdkconfig.defaults`, or source code.
- `sdkconfig.defaults` is the useful baseline for this exact board.
- If you regenerate `sdkconfig`, keep the hosted Wi-Fi, PSRAM, TinyUSB, and P4 revision settings unless you know your board differs.
- If the board reboots when a serial tool connects, it is usually DTR/RTS line-state behavior. Reconnect or use manual `BOOT` + `RST` flashing.
- The weather APIs used here do not require an API key.
