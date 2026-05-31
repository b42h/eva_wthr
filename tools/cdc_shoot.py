#!/usr/bin/env python3
"""Drive the Eva CDC console: set clock offset + weather kind, grab screenshots.

Usage:
    cdc_shoot.py <outdir> <clockoffset_hours> <kind1> [kind2 ...]

For each kind it sends:
    clockoffset <hours>
    weatherdebug <kind> 0
    screenshot
and saves <outdir>/<kind>_off<hours>.jpg from the BEGIN_SCREENSHOT frame.

Protocol (CLAUDE.md s4):
    > screenshot\r\n
    < BEGIN_SCREENSHOT <size>\r\n
    < <size> raw JPEG bytes
    < \r\nEND_SCREENSHOT\r\n
"""
import os
import sys
import time
import serial  # pyserial

PORT = "/dev/cu.usbmodem1234561"
BAUD = 115200


def read_until(ser, marker, timeout=6.0):
    """Read bytes until `marker` (bytes) is seen; return everything read."""
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


def send_cmd(ser, line, settle=0.0):
    ser.write((line + "\r\n").encode())
    ser.flush()
    if settle:
        time.sleep(settle)


def grab_screenshot(ser, path):
    # Flush any pending input first.
    ser.reset_input_buffer()
    send_cmd(ser, "screenshot")
    # Wait for the BEGIN_SCREENSHOT <size>\r\n header.
    data, idx = read_until(ser, b"BEGIN_SCREENSHOT ", timeout=8.0)
    if idx == -1:
        print(f"  !! no BEGIN_SCREENSHOT header; got {len(data)} bytes: {data[:120]!r}")
        return False
    # Parse the size: from after the marker up to the next \r\n.
    after = data[idx + len(b"BEGIN_SCREENSHOT "):]
    # We may not yet have the full "<size>\r\n"; read more until \r\n present.
    while b"\r\n" not in after:
        more = ser.read(64)
        if not more:
            break
        after += more
    nl = after.find(b"\r\n")
    if nl == -1:
        print("  !! malformed size line")
        return False
    try:
        size = int(after[:nl].strip())
    except ValueError:
        print(f"  !! bad size field: {after[:nl]!r}")
        return False
    # Bytes already buffered after the size header are the start of the JPEG.
    jpeg = bytearray(after[nl + 2:])
    deadline = time.time() + 12.0
    # We need `size` JPEG bytes, then \r\nEND_SCREENSHOT\r\n.
    while len(jpeg) < size and time.time() < deadline:
        chunk = ser.read(size - len(jpeg))
        if chunk:
            jpeg.extend(chunk)
        else:
            time.sleep(0.01)
    jpeg = bytes(jpeg[:size])
    if len(jpeg) < size:
        print(f"  !! short JPEG: {len(jpeg)}/{size}")
        return False
    # Drain the trailing END marker (don't fail if it's slow).
    read_until(ser, b"END_SCREENSHOT", timeout=2.0)
    with open(path, "wb") as f:
        f.write(jpeg)
    print(f"  saved {path} ({size} bytes)")
    return True


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(2)
    outdir = sys.argv[1]
    offset = sys.argv[2]
    kinds = sys.argv[3:]
    os.makedirs(outdir, exist_ok=True)

    ser = serial.Serial()
    ser.port = PORT
    ser.baudrate = BAUD
    ser.timeout = 0.2
    # Assert DTR+RTS once and leave them — avoids the bootloader RTS-edge trap.
    ser.dtr = True
    ser.rts = True
    ser.open()
    time.sleep(0.4)            # let the line settle past the 250ms boot window
    ser.reset_input_buffer()

    send_cmd(ser, f"clockoffset {offset}", settle=0.4)
    print(read_until(ser, b"clockoffset", timeout=2.0)[0].decode(errors="replace").strip())

    for kind in kinds:
        print(f"[{kind}] offset={offset}h")
        send_cmd(ser, f"weatherdebug {kind} 0", settle=0.2)
        resp, _ = read_until(ser, b"weatherdebug", timeout=3.0)
        # Give the canvas time to rebake the background + clouds for the new kind.
        time.sleep(1.6)
        path = os.path.join(outdir, f"{kind}_off{offset}.jpg")
        grab_screenshot(ser, path)

    ser.close()


if __name__ == "__main__":
    main()
