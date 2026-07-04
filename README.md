# Eva Weather Firmware

`phase7_eva_weather` is the active weather-only firmware for the Guition
JC4880P443C_I_W board.

It boots directly into the procedural weather canvas and keeps:
- hosted Wi-Fi + HTTP time sync
- NVS timezone storage
- TinyUSB CDC debug console
- weather fetch + debug commands
- screenshot capture

It does not include the eyes scene or emotion controls.

## Correct Build/Flash Workflow

Use ESP-IDF `v5.5.4` for this project.

```sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
source "$HOME/.espressif/v5.5.4/esp-idf/export.sh"
idf.py build
```

Output app binary:

```text
build/eva_weather.bin
```

## Flash (Automatic)

From `phase7_eva_weather`:

```sh
./flash.sh
```

Or from `42_EVE_FW`:

```sh
./flash-weather.sh
```

You can also pass a known runtime CDC port:

```sh
./flash.sh /dev/cu.usbmodem1234561
```

`flash.sh` flow:
1. opens runtime CDC port
2. toggles DTR/RTS to enter ROM download mode
3. waits for ROM USB modem port
4. runs `esptool ... write_flash @flash_args`

## Flash Recovery (Manual BOOT+RST)

Use this path if the runtime CDC port is unstable or missing.

1. Hold `BOOT`
2. Tap `RST`
3. Release `BOOT`
4. Flash from `build/`:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem21101 -b 460800 \
  --before no_reset --after hard_reset write_flash @flash_args
```

If `/dev/cu.usbmodem21101` is different on your host, check:

```sh
ls /dev/cu.usbmodem*
```

## Smoke Test After Flash

1. Open monitor on the runtime CDC port:

```sh
idf.py -p /dev/cu.usbmodem1234561 monitor
```

2. Confirm boot banner appears:

```text
Eva WEATHER v1 built <date> <time>
```

3. Run these commands in CDC console:

```text
whoami
status
perf
weather refresh
weather rain 12 "Test rain"
```

Expected:
- `whoami` returns `weather`
- `status` prints Wi-Fi/time/weather state
- `perf` prints render and cloud-strip timings
- weather update commands return `OK ...`

## Serial Port Note (Important)

This firmware uses CDC line-state transitions for auto-entering bootloader.
Some serial tools toggle RTS/DTR on connect/disconnect, which can reboot the
board unexpectedly. If that happens, reconnect and continue, or use manual
`BOOT+RST` recovery flash.

## CDC Commands

```text
whoami
time
tz
tz <posix-tz-string>
clockoffset <hours>
weather
weather <kind> <temp_c> "Description"
weather refresh
weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>
weatherdebug <kind> <frame>
status
perf
screenshot
log
log <none|error|warn|info|debug|verbose>
```

`<kind>` accepts:

```text
clear-day clear-night partly-cloudy-day partly-cloudy-night cloudy fog rain heavy-rain snow thunderstorm sleet hail
```

`clockoffset` is temporary and only shifts the visual weather scene + clock;
it does not rewrite NVS timezone settings.
`weatherdebug` uses `frame` as a seed for repeatable cloud variation, so
scripts can capture distinct looks for the same weather kind.
