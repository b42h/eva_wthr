#!/usr/bin/env python3
"""Capture an animated GIF from the live panel over CDC.

Usage:
    cdc_animate.py <out.gif> <kind> [--frames N] [--offset H] [--delay MS]
                   [--lightning] [--port PORT]

Pins the scene with `weatherpin on` (live weather would otherwise overwrite
a debug kind mid-capture, CLAUDE.md s6), sets the requested kind, then grabs
N screenshots back to back and writes them out as a looping GIF.

Screenshots arrive as 480x800 portrait JPEGs whose content reads sideways;
they are rotated 90 deg CCW here so the GIF matches what a human sees on the
panel (the double-rotation gotcha in CLAUDE.md s6).

Requires: pyserial, Pillow.
"""
import argparse
import io
import os
import sys
import time

import serial  # pyserial
from PIL import Image

DEFAULT_PORT = "/dev/cu.usbmodem1234561"
BAUD = 115200


def read_until(ser, marker, timeout=8.0):
    deadline = time.time() + timeout
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            idx = buf.find(marker)
            if idx != -1:
                return bytes(buf), idx
        else:
            time.sleep(0.01)
    return bytes(buf), -1


def send_cmd(ser, cmd, settle=0.2):
    ser.write((cmd + "\r\n").encode())
    ser.flush()
    time.sleep(settle)


def grab_frame(ser):
    """Return one PIL Image, or None if the frame did not arrive."""
    ser.reset_input_buffer()
    send_cmd(ser, "screenshot", settle=0.05)

    buf, idx = read_until(ser, b"BEGIN_SCREENSHOT ", timeout=8.0)
    if idx < 0:
        return None
    tail = buf[idx + len(b"BEGIN_SCREENSHOT "):]
    nl = tail.find(b"\r\n")
    while nl < 0:
        more = ser.read(256)
        if not more:
            return None
        tail += more
        nl = tail.find(b"\r\n")
    try:
        size = int(tail[:nl].decode().strip())
    except ValueError:
        return None

    payload = bytearray(tail[nl + 2:])
    deadline = time.time() + 12.0
    while len(payload) < size and time.time() < deadline:
        chunk = ser.read(min(4096, size - len(payload)))
        if chunk:
            payload.extend(chunk)
        else:
            time.sleep(0.01)
    if len(payload) < size:
        return None

    try:
        img = Image.open(io.BytesIO(bytes(payload[:size])))
        img.load()
    except Exception:
        return None
    # No rotation: eva_screenshot.c encodes the 800x480 LANDSCAPE render
    # buffer (before the PPA rotate onto the portrait panel), so the JPEG
    # already reads the right way up. The -90 host rotation documented for
    # tools/eva-screenshot.py applies to the portrait-native capture path,
    # not to this one.
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("kind")
    ap.add_argument("--frames", type=int, default=24)
    ap.add_argument("--offset", default="0")
    ap.add_argument("--delay", type=int, default=120, help="GIF frame delay, ms")
    ap.add_argument("--lightning", action="store_true",
                    help="force a strike every 3rd frame (storm scenes)")
    ap.add_argument("--port", default=DEFAULT_PORT)
    ap.add_argument("--scale", type=float, default=1.0,
                    help="1.0 keeps the native 800x480 landscape frame")
    ap.add_argument("--settle", type=float, default=8.0,
                    help="seconds to let the scene settle before recording")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = BAUD
    ser.timeout = 0.2
    # Assert DTR+RTS once and leave them — avoids the bootloader RTS-edge trap.
    ser.dtr = True
    ser.rts = True
    ser.open()
    time.sleep(0.5)
    ser.reset_input_buffer()

    send_cmd(ser, "weatherpin on", settle=0.4)
    send_cmd(ser, f"clockoffset {args.offset}", settle=0.4)
    send_cmd(ser, f"weatherdebug {args.kind} 0", settle=0.3)
    read_until(ser, b"weatherdebug", timeout=4.0)
    # Weather changes EASE over ~15 s (eva_wx_transition.h) — recording right
    # away captures the previous scene mid-morph. Kill the ease for capture,
    # re-issue the kind so it applies instantly, then let clouds settle.
    send_cmd(ser, "transition 0", settle=0.3)
    send_cmd(ser, f"weatherdebug {args.kind} 0", settle=0.3)
    read_until(ser, b"weatherdebug", timeout=4.0)
    time.sleep(args.settle)

    frames = []
    for i in range(args.frames):
        if args.lightning and i % 3 == 0:
            send_cmd(ser, "lightning", settle=0.05)
        img = grab_frame(ser)
        if img is None:
            print(f"  frame {i}: MISSED", file=sys.stderr)
            continue
        if args.scale != 1.0:
            img = img.resize((int(img.width * args.scale),
                              int(img.height * args.scale)),
                             Image.LANCZOS)
        frames.append(img.convert("P", palette=Image.ADAPTIVE, colors=128))
        print(f"  frame {i + 1}/{args.frames}")

    # Restore the shipped defaults we overrode for the capture.
    send_cmd(ser, "transition 15000", settle=0.3)
    send_cmd(ser, "weatherpin off", settle=0.3)
    ser.close()

    if not frames:
        print("no frames captured", file=sys.stderr)
        sys.exit(1)

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    frames[0].save(args.out, save_all=True, append_images=frames[1:],
                   duration=args.delay, loop=0, optimize=True)
    print(f"wrote {args.out} ({len(frames)} frames, "
          f"{os.path.getsize(args.out) // 1024} KB)")


if __name__ == "__main__":
    main()
