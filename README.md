# Eva Weather Firmware

Weather-display firmware for the Guition **JC4880P443C_I_W** board
(ESP32-P4, ILI9881C 480×800 MIPI-DSI panel). It boots straight into a
native weather canvas: a procedural sky with pre-baked, animated cloud
layers, sun/moon, precipitation, and lightning — driven by live weather
over hosted Wi-Fi.

## Current look

| Moving clouds | Rain + lightning |
|---|---|
| ![Clouds moving](docs/images/05-clouds-moving.gif) | ![Thunderstorm rain and lightning](docs/images/06-thunderstorm-rain.gif) |

| Moon at night | Sunrise |
|---|---|
| ![Moon at night](docs/images/07-moon-at-night.gif) | ![Sunrise](docs/images/08-sunrise.gif) |

## Features

- Native 800×480 landscape render, PPA-rotated to the portrait panel.
- **Pre-baked cloud masks** generated offline (`tools/cloudgen/`) and
  shipped as an LZ4 pack (`assets/clouds.bin`) mmap'd from a raw flash
  partition — no on-device generation, no frame stalls. Storm scenes use
  a separate cloud pool.
- Three parallax cloud layers with slow morph, wind drift, and depth
  "breathing"; seamless horizontal tiling.
- Time-of-day sky palette (day → sunset → night → sunrise) with Bayer
  dithering, independent sun and moon, god rays, night backlight dim.
- Rain / heavy-rain / snow / sleet / hail particles; lightning composited
  over the cloud deck in thunderstorms.
- Hosted Wi-Fi + HTTP time sync, NVS timezone, TinyUSB CDC debug console,
  screenshot capture.

Wi-Fi credentials are provided via Kconfig
(`CONFIG_EVA_WIFI_SSID` / `CONFIG_EVA_WIFI_PASSWORD`) — set them with
`idf.py menuconfig`; they are not committed.

For the history of what has been built and what is still planned, see
[`docs/DONE.md`](docs/DONE.md) and the pending specs/plans under
`docs/superpowers/`.

## Build

Use ESP-IDF **v5.5.4** for this project.

```sh
source "$HOME/.espressif/v5.5.4/esp-idf/export.sh"
idf.py build            # app → build/eva_weather.bin, pack → assets/clouds.bin
```

To change the cloud look, edit `PROFILES` in `tools/cloudgen/cloudgen.py`,
regenerate, review the preview, then rebuild:

```sh
python tools/cloudgen/genpool.py     # regenerates assets/clouds.bin
python tools/cloudgen/preview.py     # contact sheet for visual approval
```

## Flash (automatic)

From `phase7_eva_weather`:

```sh
./flash.sh                       # or ./flash-weather.sh from 42_EVE_FW
./flash.sh /dev/cu.usbmodem1234561   # pass a known runtime CDC port
```

`flash.sh` opens the runtime CDC port, toggles DTR/RTS to enter ROM
download mode, waits for the ROM USB port, then runs
`esptool ... write_flash @flash_args`.

## Flash recovery (manual BOOT+RST)

Use this if the runtime CDC port is unstable or missing.

1. Hold `BOOT`, tap `RST`, release `BOOT`.
2. Flash from `build/`:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodemXXXX -b 460800 \
  --before no_reset --after hard_reset write_flash @flash_args
```

Find the port with `ls /dev/cu.usbmodem*`.

## Smoke test after flash

Open a monitor on the runtime CDC port (`idf.py -p /dev/cu.usbmodemXXXX
monitor`) and run:

```text
whoami        # → weather
status        # Wi-Fi / time / weather state
perf          # render + cloud timings
cloudinfo     # active cloud pool per layer
weather refresh
```

## CDC commands

```text
whoami                 time            tz [<posix-tz>]
clockoffset <hours>    status          perf
weather                weather <kind> <temp_c> "Description"
weather refresh        weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>
weatherdebug <kind> <frame>            cloudinfo
lightning              screenshot      log [<level>]
```

`<kind>`: `clear-day clear-night partly-cloudy-day partly-cloudy-night
cloudy fog rain heavy-rain snow thunderstorm sleet hail`

- `clockoffset` temporarily shifts the visual scene + clock; it does not
  rewrite the NVS timezone.
- `weatherdebug` uses `frame` as a repeatable seed for cloud variation.
- `weatherraw`/`weatherdebug` scenes are overwritten by the next live
  weather fetch — re-issue them for long demo sessions.
- `lightning` forces a strike (thunderstorm/hail only).

### Serial port note

This firmware uses CDC line-state transitions to auto-enter the
bootloader. Some serial tools toggle RTS/DTR on connect/disconnect, which
can reboot the board; reconnect and continue, or use manual BOOT+RST
recovery flash.
