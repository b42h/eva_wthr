#!/usr/bin/env python3
"""
Capture a batch of Eva weather screenshots across dayparts.

The script drives the board over CDC, shifts the temporary visual clock with
`clockoffset`, seeds `weatherdebug` so cloud layouts vary, and saves one JPEG
per weather family/daypart pair.

Default matrix:
  - clear
  - partly-cloudy
  - cloudy
  - fog
  - rain
  - heavy-rain
  - snow
  - thunderstorm
  - sleet
  - hail

Each family is captured for:
  - morning
  - day
  - evening
  - night

Clear and partly-cloudy map to their day/night variants automatically.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import random
import re
import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.stderr.write("Install pyserial first: pip3 install --break-system-packages pyserial\n")
    sys.exit(2)

DEFAULT_PORT = "/dev/cu.usbmodem1234561"
BEGIN_PREFIX = b"BEGIN_SCREENSHOT "
END_MARKER = b"\r\nEND_SCREENSHOT\r\n"
READ_TIMEOUT = 8.0
SETTLE_SECONDS = 1.0

DAYPARTS = [
    ("morning", 8),
    ("day", 13),
    ("evening", 18),
    ("night", 23),
]

FAMILIES = [
    "clear",
    "partly-cloudy",
    "cloudy",
    "fog",
    "rain",
    "heavy-rain",
    "snow",
    "thunderstorm",
    "sleet",
    "hail",
]


def find_default_port() -> str:
    candidates = [
        DEFAULT_PORT,
        "/dev/cu.usbmodem1234562",
    ]
    for c in candidates:
        if Path(c).exists():
            return c
    for p in sorted(Path("/dev").glob("cu.usbmodem*")):
        return str(p)
    return DEFAULT_PORT


def read_text_block(ser: serial.Serial, deadline: float = 2.0, quiet: float = 0.25) -> bytes:
    buf = bytearray()
    last_data = time.time()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            last_data = time.time()
        else:
            if time.time() - last_data >= quiet:
                break
            time.sleep(0.01)
    return bytes(buf)


def send_cmd(ser: serial.Serial, cmd: str, deadline: float = 2.0) -> str:
    ser.reset_input_buffer()
    ser.write(cmd.encode("ascii") + b"\r\n")
    ser.flush()
    return read_text_block(ser, deadline=deadline).decode("utf-8", "replace")


def parse_time_hour(text: str) -> int | None:
    for line in text.splitlines():
        m = re.match(r"^\d{4}-\d{2}-\d{2}\s+(\d{2}):(\d{2}):(\d{2})\b", line)
        if m:
            return int(m.group(1))
    return None


def wait_for_synced_hour(ser: serial.Serial, max_wait_s: float = 180.0) -> int:
    deadline = time.time() + max_wait_s
    while time.time() < deadline:
        text = send_cmd(ser, "time", deadline=2.0)
        hour = parse_time_hour(text)
        if hour is not None and "not synced yet" not in text:
            return hour
        time.sleep(1.0)
    raise TimeoutError("device time never synced; can't choose dayparts reliably")


def resolve_clock_hour(ser: serial.Serial) -> int:
    try:
        return wait_for_synced_hour(ser, max_wait_s=20.0)
    except TimeoutError:
        host_hour = dt.datetime.now().hour
        print(f"warning: device time not synced; falling back to host hour {host_hour:02d}", flush=True)
        return host_hour


def signed_hour_delta(current_hour: int, target_hour: int) -> int:
    delta = (target_hour - current_hour) % 24
    if delta > 12:
        delta -= 24
    return delta


def actual_kind(family: str, daypart: str) -> str:
    if family == "clear":
        return "clear-night" if daypart == "night" else "clear-day"
    if family == "partly-cloudy":
        return "partly-cloudy-night" if daypart == "night" else "partly-cloudy-day"
    return family


def capture_jpeg(ser: serial.Serial, outfile: Path) -> int:
    ser.reset_input_buffer()
    deadline = time.time() + READ_TIMEOUT
    ser.write(b"screenshot\r\n")
    ser.flush()

    buf = bytearray()
    size = None
    size_end = -1
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            err_idx = buf.find(b"ERR screenshot")
            if err_idx >= 0:
                eol = buf.find(b"\n", err_idx)
                line = buf[err_idx:eol if eol > 0 else len(buf)]
                raise RuntimeError(line.decode("utf-8", "replace"))

            begin_idx = buf.find(BEGIN_PREFIX)
            if begin_idx >= 0:
                crlf = buf.find(b"\r\n", begin_idx)
                if crlf > begin_idx:
                    line = buf[begin_idx:crlf].decode("ascii", "replace")
                    try:
                        size = int(line.split()[-1])
                        size_end = crlf + 2
                        break
                    except ValueError as e:
                        raise RuntimeError(f"could not parse screenshot size from {line!r}") from e
            if len(buf) > 16384:
                raise RuntimeError("screenshot header did not appear")
        else:
            time.sleep(0.01)

    if size is None:
        raise TimeoutError("timed out waiting for BEGIN_SCREENSHOT")
    if size <= 0 or size > 1_000_000:
        raise RuntimeError(f"unreasonable screenshot size: {size}")

    payload = bytearray(buf[size_end:size_end + size])
    need = size - len(payload)
    while need > 0:
        if time.time() >= deadline:
            raise TimeoutError(f"timed out reading JPEG payload ({len(payload)}/{size} bytes)")
        chunk = ser.read(need)
        if chunk:
            payload.extend(chunk)
            need = size - len(payload)
        else:
            time.sleep(0.01)

    tail = bytearray(buf[size_end + size:size_end + size + len(END_MARKER)])
    while len(tail) < len(END_MARKER):
        if time.time() >= deadline:
            raise TimeoutError("timed out waiting for END_SCREENSHOT")
        chunk = ser.read(len(END_MARKER) - len(tail))
        if chunk:
            tail.extend(chunk)
        else:
            time.sleep(0.01)
    if bytes(tail) != END_MARKER:
        raise RuntimeError(f"missing END marker, got {bytes(tail)!r}")

    outfile.write_bytes(bytes(payload))
    return size


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("out_dir", nargs="?", default=None,
                   help="output directory (default: screenshots/eva_YYYYMMDD_HHMMSS)")
    p.add_argument("--port", default=None,
                   help=f"CDC port (default: {DEFAULT_PORT} if present, else first cu.usbmodem*)")
    p.add_argument("--keep-live", action="store_true",
                   help="do not queue a live weather refresh at the end")
    p.add_argument("--settle-seconds", type=float, default=SETTLE_SECONDS,
                   help="wait after each scene change before capturing")
    args = p.parse_args()

    ts = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out_dir = Path(args.out_dir) if args.out_dir else Path("screenshots") / f"eva_{ts}"
    out_dir.mkdir(parents=True, exist_ok=True)

    port = args.port or find_default_port()
    print(f"port: {port}", flush=True)
    print(f"out:  {out_dir}", flush=True)

    manifest = {
        "generated_at": dt.datetime.now().isoformat(timespec="seconds"),
        "port": port,
        "out_dir": str(out_dir),
        "shots": [],
    }

    rng = random.SystemRandom()

    ser = serial.Serial(port, 115200, timeout=0.5)
    try:
        # Clear any tail from earlier activity and leave the board in a clean
        # normal state before we start the batch.
        ser.reset_input_buffer()
        send_cmd(ser, "test off")

        current_hour = resolve_clock_hour(ser)
        print(f"base hour: {current_hour:02d}", flush=True)

        for family in FAMILIES:
            family_dir = out_dir / family
            family_dir.mkdir(parents=True, exist_ok=True)
            for daypart, target_hour in DAYPARTS:
                offset = signed_hour_delta(current_hour, target_hour)
                kind = actual_kind(family, daypart)
                frame = rng.randrange(0, 256)
                outfile = family_dir / f"{daypart}.jpg"

                print(f"{family:>15} / {daypart:<8} -> {kind} frame={frame:03d} offset={offset:+d}", flush=True)
                send_cmd(ser, f"clockoffset {offset}")
                send_cmd(ser, f"weatherdebug {kind} {frame}")
                time.sleep(args.settle_seconds)
                size = capture_jpeg(ser, outfile)
                print(f"  saved {size} bytes -> {outfile}", flush=True)

                manifest["shots"].append({
                    "family": family,
                    "daypart": daypart,
                    "weather_kind": kind,
                    "frame": frame,
                    "clock_offset_hours": offset,
                    "file": str(outfile),
                    "bytes": size,
                })

        send_cmd(ser, "clockoffset 0")
        if not args.keep_live:
            send_cmd(ser, "weather refresh")
        manifest["completed_at"] = dt.datetime.now().isoformat(timespec="seconds")
        (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        print(f"manifest: {out_dir / 'manifest.json'}", flush=True)
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
