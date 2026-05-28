#!/bin/bash
# Auto-flash without manual BOOT+RST.
# Triggers firmware reboot-to-bootloader via DTR/RTS on the running CDC port,
# waits for ROM USB download port to appear, then flashes.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD="$SCRIPT_DIR/build"
PORT="${1:-}"

if [ -z "${IDF_PATH:-}" ]; then
  export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf"
fi

if ! command -v esptool.py >/dev/null 2>&1 && [ -f "$IDF_PATH/export.sh" ]; then
  # Load ESP-IDF tools when the caller did not already source export.sh.
  # Keep output quiet so the flash log stays readable.
  # shellcheck disable=SC1091
  . "$IDF_PATH/export.sh" >/dev/null 2>&1 || true
fi

if [ -z "$PORT" ]; then
  for _ in $(seq 1 20); do
    PORT="$(find /dev -maxdepth 1 -name 'cu.usbmodem*' | sort | head -n 1 || true)"
    [ -n "$PORT" ] && break
    sleep 0.2
  done
fi

if [ -z "$PORT" ]; then
  echo "no /dev/cu.usbmodem* port found"
  exit 1
fi

echo ">> firmware port: $PORT"

if [[ "$PORT" == *"21101" ]] || [[ "$PORT" == *"21201" ]]; then
  ROM_PORT="$PORT"
else
  echo ">> triggering reboot-to-bootloader via DTR/RTS"
  python - <<PY
import serial
import time
import sys

try:
    s = serial.Serial("$PORT", 115200, timeout=0.2)
    s.dtr = False
    s.rts = False
    time.sleep(0.05)
    s.dtr = True
    s.rts = True
    time.sleep(0.05)
    try:
      s.rts = False
    except Exception:
      pass
    time.sleep(0.05)
    try:
        s.close()
    except Exception:
        pass
except Exception as e:
    print(f"trigger note: {e}", file=sys.stderr)
PY

  echo ">> waiting for ROM port..."
  for _ in $(seq 1 30); do
    sleep 0.3
    ROM_PORT="$(find /dev -maxdepth 1 -name 'cu.usbmodem*' | grep -E 'usbmodem[0-9]+1$' | sort | head -n 1 || true)"
    if [ -n "$ROM_PORT" ] && [ "$ROM_PORT" != "$PORT" ]; then
      break
    fi
  done

  if [ -z "$ROM_PORT" ] || [ "$ROM_PORT" == "$PORT" ]; then
    echo "ROM port did not appear within 9s"
    exit 2
  fi
fi

echo ">> ROM port: $ROM_PORT -> flashing"
cd "$BUILD"
python -m esptool --chip esp32p4 -p "$ROM_PORT" -b 460800 \
  --before no_reset --after hard_reset write_flash @flash_args
echo ">> done"
