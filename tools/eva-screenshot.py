#!/usr/bin/env python3
"""
Capture a screenshot of the Eva weather canvas over USB-CDC.

The board returns a hardware-encoded JPEG framed as:
    BEGIN_SCREENSHOT <size>\r\n
    <size bytes of raw JPEG>
    \r\nEND_SCREENSHOT\r\n

Usage:
    python3 tools/eva-screenshot.py                 # save to eva_YYYYMMDD_HHMMSS.jpg
    python3 tools/eva-screenshot.py out.jpg         # save to out.jpg
    python3 tools/eva-screenshot.py --port /dev/cu.usbmodem1234561 out.jpg

Exits non-zero on any error.
"""
from __future__ import annotations

import argparse
import datetime
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
READ_TIMEOUT = 8.0   # seconds


def find_default_port() -> str:
    """Return the Eva CDC port if it's present, otherwise DEFAULT_PORT.
    Eva sets a fixed USB iSerialNumber so the port name is stable."""
    candidates = [
        DEFAULT_PORT,
        "/dev/cu.usbmodem1234562",   # rare fallback if iSerial collided
    ]
    for c in candidates:
        if Path(c).exists():
            return c
    # Generic glob fallback
    for p in sorted(Path("/dev").glob("cu.usbmodem*")):
        return str(p)
    return DEFAULT_PORT


def read_until(ser: serial.Serial, sentinel: bytes, max_bytes: int, deadline: float) -> bytes:
    """Read from ser until `sentinel` appears in the stream or deadline passes.
    Returns everything up to and including the sentinel. Raises on timeout."""
    buf = bytearray()
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf.extend(chunk)
            if sentinel in buf:
                idx = buf.index(sentinel) + len(sentinel)
                return bytes(buf[:idx])
            if len(buf) > max_bytes:
                raise RuntimeError(f"sentinel {sentinel!r} not found after {len(buf)} bytes")
        else:
            time.sleep(0.01)
    raise TimeoutError(f"timed out waiting for {sentinel!r}")


def read_exact(ser: serial.Serial, n: int, deadline: float) -> bytes:
    buf = bytearray()
    while len(buf) < n:
        if time.time() >= deadline:
            raise TimeoutError(f"timed out reading {n} bytes (got {len(buf)})")
        chunk = ser.read(n - len(buf))
        if chunk:
            buf.extend(chunk)
        else:
            time.sleep(0.01)
    return bytes(buf)


def capture(port: str, outfile: Path) -> int:
    ser = serial.Serial(port, 115200, timeout=0.5)
    try:
        # Drain any pending tail from a previous command (board-side CDC TX
        # ring may still be flushing an earlier screenshot blob). Read until
        # the stream goes quiet for ~300 ms.
        ser.reset_input_buffer()
        idle_start = None
        deadline_drain = time.time() + 3.0
        while time.time() < deadline_drain:
            d = ser.read(2048)
            if d:
                idle_start = None
            else:
                if idle_start is None:
                    idle_start = time.time()
                elif time.time() - idle_start > 0.3:
                    break
        ser.write(b"screenshot\r\n")
        ser.flush()
        deadline = time.time() + READ_TIMEOUT

        # Look for the BEGIN_SCREENSHOT header followed by CRLF. ESP_LOG lines
        # use LF only (not CRLF), so scanning for the BEGIN prefix + CRLF
        # cleanly skips over any logs that print between the command echo and
        # the actual header. We also bail early if we see "ERR screenshot".
        buf = bytearray()
        size = None
        size_end = -1
        while time.time() < deadline:
            chunk = ser.read(4096)
            if chunk:
                buf.extend(chunk)
                # Early error detection.
                err_idx = buf.find(b"ERR screenshot")
                if err_idx >= 0:
                    eol = buf.find(b"\n", err_idx)
                    line = buf[err_idx:eol if eol > 0 else len(buf)]
                    sys.stderr.write(line.decode("utf-8", "replace") + "\n")
                    return 1
                # Find begin header + its terminating CRLF.
                begin_idx = buf.find(BEGIN_PREFIX)
                if begin_idx >= 0:
                    crlf = buf.find(b"\r\n", begin_idx)
                    if crlf > begin_idx:
                        line = buf[begin_idx:crlf].decode("ascii", "replace")
                        try:
                            size = int(line.split()[-1])
                            size_end = crlf + 2  # skip past CRLF
                            break
                        except ValueError:
                            sys.stderr.write(f"could not parse size from: {line!r}\n")
                            return 1
                if len(buf) > 16384:
                    sys.stderr.write(f"no BEGIN found after {len(buf)} bytes\n")
                    return 1
            else:
                time.sleep(0.01)
        if size is None:
            sys.stderr.write("timeout waiting for BEGIN_SCREENSHOT\n")
            return 1
        if size <= 0 or size > 1_000_000:
            sys.stderr.write(f"unreasonable size: {size}\n")
            return 1

        # Payload may already be partially in `buf` after the header.
        payload = bytearray(buf[size_end:size_end + size])
        need = size - len(payload)
        deadline = time.time() + READ_TIMEOUT
        if need > 0:
            payload.extend(read_exact(ser, need, deadline))
        # Read tail (might also be partially buffered already; the simpler
        # path is to read it fresh because we copied the payload slice and
        # know exact remaining offset in buf).
        already = buf[size_end + size:size_end + size + len(END_MARKER)]
        tail = bytearray(already)
        need = len(END_MARKER) - len(tail)
        if need > 0:
            tail.extend(read_exact(ser, need, deadline))
        if bytes(tail) != END_MARKER:
            sys.stderr.write(f"missing END marker, got: {bytes(tail)!r}\n")
            return 1

        outfile.write_bytes(bytes(payload))
        print(f"saved {size} bytes to {outfile}")
        return 0
    finally:
        ser.close()


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("outfile", nargs="?", default=None,
                   help="output JPEG path (default: eva_YYYYMMDD_HHMMSS.jpg)")
    p.add_argument("--port", default=None,
                   help=f"CDC port (default: {DEFAULT_PORT} if present, else first cu.usbmodem*)")
    args = p.parse_args()

    if args.outfile is None:
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        outfile = Path(f"eva_{ts}.jpg")
    else:
        outfile = Path(args.outfile)

    port = args.port or find_default_port()
    print(f"port: {port}")
    try:
        return capture(port, outfile)
    except (serial.SerialException, FileNotFoundError) as e:
        sys.stderr.write(f"serial error: {e}\n")
        return 1
    except TimeoutError as e:
        sys.stderr.write(f"timeout: {e}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
