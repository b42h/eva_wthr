#!/usr/bin/env python3
"""Push a build to the Eva panel over Wi-Fi. No cable.

    tools/ota.py                 # build, then send whatever changed
    tools/ota.py --no-build      # send the current build/ as-is
    tools/ota.py --dry-run       # show what would be sent, send nothing
    tools/ota.py --host 192.168.1.47

Only what changed is transferred: the device records the SHA-256 the host sent
for each artifact, so an unchanged 7.6 MB clouds pack costs one status request
instead of a minute of Wi-Fi.

Stdlib only — no pip install on a fresh Mac.

Exit codes:
    0  updated, or already up to date
    1  transfer or verification failed
    2  device unreachable
    3  build failed
    4  the new image was rolled back by the device
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.dirname(HERE)
APP_BIN = os.path.join(PROJECT, "build", "eva_weather.bin")
PACK_BIN = os.path.join(PROJECT, "assets", "clouds.bin")

DEFAULT_HOST = "eva-weather.local"
DEFAULT_PORT = 8080
IDF_PATH = os.path.expanduser("~/.espressif/v5.5.4/esp-idf")

# Exit codes
EX_OK, EX_FAIL, EX_UNREACHABLE, EX_BUILD, EX_ROLLBACK = 0, 1, 2, 3, 4


# ----------------------------------------------------------------- utilities

def human(n):
    return f"{n / (1024 * 1024):.2f} MB" if n >= 1024 * 1024 else f"{n / 1024:.0f} KB"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def log(msg):
    print(msg, flush=True)


def die(code, msg):
    print(f"error: {msg}", file=sys.stderr, flush=True)
    sys.exit(code)


# ------------------------------------------------------------------ discovery

def resolve_host(explicit, port):
    """Return a reachable host string, or None."""
    candidates = [explicit] if explicit else [DEFAULT_HOST]

    for cand in candidates:
        try:
            socket.getaddrinfo(cand, port, proto=socket.IPPROTO_TCP)
            return cand
        except socket.gaierror:
            pass

    if explicit:
        return None

    # mDNS name did not resolve — browse for the service instead. dns-sd is
    # part of macOS; the two-step browse/resolve is why this is not just
    # getaddrinfo.
    log("eva-weather.local did not resolve; browsing for _eva-ota._tcp ...")
    try:
        proc = subprocess.Popen(
            ["dns-sd", "-B", "_eva-ota._tcp"],
            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
        deadline = time.time() + 5
        name = None
        while time.time() < deadline:
            line = proc.stdout.readline()
            if not line:
                break
            parts = line.split()
            if len(parts) >= 7 and "_eva-ota._tcp" in line and "Add" in parts:
                name = parts[-1]
                break
        proc.terminate()
        if name:
            host = f"{name}.local"
            try:
                socket.getaddrinfo(host, port, proto=socket.IPPROTO_TCP)
                return host
            except socket.gaierror:
                pass
    except FileNotFoundError:
        pass

    return None


# --------------------------------------------------------------- HTTP client

def get_json(base, path, timeout=10):
    with urllib.request.urlopen(f"{base}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode())


class ProgressReader:
    """Wraps a file object so urllib streams it while we draw a progress bar."""

    def __init__(self, path, size, label):
        self.f = open(path, "rb")
        self.size = size
        self.label = label
        self.sent = 0
        self.start = time.time()
        self.last_draw = 0.0

    def read(self, n=-1):
        chunk = self.f.read(n)
        if chunk:
            self.sent += len(chunk)
            now = time.time()
            if now - self.last_draw > 0.2 or self.sent >= self.size:
                self.last_draw = now
                self._draw()
        return chunk

    def _draw(self):
        pct = self.sent * 100 // self.size if self.size else 100
        elapsed = max(time.time() - self.start, 0.001)
        rate = self.sent / elapsed / 1024
        bar_w = 28
        filled = pct * bar_w // 100
        bar = "#" * filled + "-" * (bar_w - filled)
        eta = (self.size - self.sent) / (self.sent / elapsed) if self.sent else 0
        sys.stdout.write(
            f"\r  {self.label} [{bar}] {pct:3d}%  {rate:6.0f} KB/s  ETA {eta:4.0f}s")
        sys.stdout.flush()

    def close(self):
        self.f.close()


def post_file(base, path, filepath, sha, label, timeout=600):
    size = os.path.getsize(filepath)
    reader = ProgressReader(filepath, size, label)
    req = urllib.request.Request(f"{base}{path}", data=reader, method="POST")
    req.add_header("Content-Type", "application/octet-stream")
    req.add_header("Content-Length", str(size))
    req.add_header("X-Eva-Sha256", sha)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            body = r.read().decode()
        sys.stdout.write("\n")
        return json.loads(body)
    except urllib.error.HTTPError as e:
        sys.stdout.write("\n")
        detail = e.read().decode(errors="replace")[:300]
        raise RuntimeError(f"HTTP {e.code}: {detail}") from None
    finally:
        reader.close()


def wait_for_reboot(base, want_version, timeout=120):
    """Poll /ota/ping until the device answers. Returns its reported version."""
    log(f"  waiting for reboot (up to {timeout}s) ...")
    deadline = time.time() + timeout
    time.sleep(3)
    while time.time() < deadline:
        try:
            info = get_json(base, "/ota/ping", timeout=4)
            return info.get("fw_version", "?")
        except Exception:
            time.sleep(2)
    return None


# ----------------------------------------------------------------------- main

def build(project):
    log("building ...")
    env = dict(os.environ)
    env["IDF_PATH"] = IDF_PATH
    cmd = f'. "{IDF_PATH}/export.sh" >/dev/null 2>&1 && idf.py build'
    proc = subprocess.run(["bash", "-c", cmd], cwd=project, env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                          text=True)
    if proc.returncode != 0:
        sys.stdout.write(proc.stdout[-4000:])
        die(EX_BUILD, "idf.py build failed")
    for line in proc.stdout.splitlines():
        if "binary size" in line or "Project build complete" in line:
            log(f"  {line.strip()}")


def main():
    ap = argparse.ArgumentParser(description="Push a build to the Eva panel over Wi-Fi.")
    ap.add_argument("--host", help="device host or IP (default: eva-weather.local, then mDNS browse)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--app-only", action="store_true", help="never send the clouds pack")
    ap.add_argument("--pack-only", action="store_true", help="never send the app")
    ap.add_argument("--force", action="store_true", help="send even if the hash matches")
    ap.add_argument("--no-build", action="store_true", help="use build/ as-is")
    ap.add_argument("--dry-run", action="store_true", help="report the diff, send nothing")
    ap.add_argument("--timeout", type=int, default=600, help="per-upload timeout (s)")
    args = ap.parse_args()

    if not args.no_build and not args.pack_only:
        build(PROJECT)

    if not os.path.exists(APP_BIN):
        die(EX_BUILD, f"missing {APP_BIN} — build first")
    if not os.path.exists(PACK_BIN):
        die(EX_BUILD, f"missing {PACK_BIN}")

    host = resolve_host(args.host, args.port)
    if not host:
        die(EX_UNREACHABLE,
            "could not find the device.\n"
            "  tried: --host, eva-weather.local, dns-sd browse for _eva-ota._tcp\n"
            "  is it on Wi-Fi? run `otainfo` over CDC to see its IP.")
    base = f"http://{host}:{args.port}"

    try:
        status = get_json(base, "/ota/status")
    except Exception as e:
        die(EX_UNREACHABLE,
            f"no answer from {base}/ota/status ({e}).\n"
            "  is the OTA firmware flashed and the device on Wi-Fi?")

    app_sha = sha256_file(APP_BIN)
    pack_sha = sha256_file(PACK_BIN)
    app_size = os.path.getsize(APP_BIN)
    pack_size = os.path.getsize(PACK_BIN)

    dev_app = status.get("app_sha256", "")
    dev_pack = status.get("pack_sha256", "")

    log("")
    log(f"device  {host}  fw {status.get('fw_version', '?')}  "
        f"slot {status.get('app_running', '?')} ({status.get('app_state', '?')})")

    def line(name, local, remote, size):
        if args.force:
            return True, f"{name:5s} {local[:8]}…  FORCED     {human(size)}"
        if local == remote:
            return False, f"{name:5s} {local[:8]}… == {remote[:8]}…  unchanged  (skip {human(size)})"
        shown = remote[:8] + "…" if remote else "(none)"
        return True, f"{name:5s} {shown} -> {local[:8]}…  CHANGED    {human(size)}"

    send_app, app_line = line("app", app_sha, dev_app, app_size)
    send_pack, pack_line = line("pack", pack_sha, dev_pack, pack_size)
    if args.pack_only:
        send_app = False
    if args.app_only:
        send_pack = False
    log(app_line)
    log(pack_line)
    log("")

    if status.get("ota_busy"):
        die(EX_FAIL, "device reports another update in progress")

    if not send_app and not send_pack:
        log("up to date — nothing to send.")
        return EX_OK

    if args.dry_run:
        log("dry run — nothing sent.")
        return EX_OK

    # Pack first: both artifacts need a reboot to take effect, and doing the
    # pack before the app lets a single reboot commit both.
    rebooted = False
    if send_pack:
        log("sending clouds pack ...")
        try:
            post_file(base, "/ota/pack", PACK_BIN, pack_sha, "pack", args.timeout)
        except Exception as e:
            die(EX_FAIL, f"pack upload failed: {e}")
        rebooted = True

    if send_app:
        if rebooted:
            # The pack handler reboots; wait for the device to come back before
            # pushing the app.
            if not wait_for_reboot(base, None, timeout=120):
                die(EX_FAIL, "device did not come back after the pack update")
        log("sending app ...")
        try:
            post_file(base, "/ota/app", APP_BIN, app_sha, "app ", args.timeout)
        except Exception as e:
            die(EX_FAIL, f"app upload failed: {e}")
        rebooted = True

    if rebooted:
        version = wait_for_reboot(base, None, timeout=120)
        if not version:
            die(EX_FAIL, "device did not come back after the update")
        log(f"  device is back, fw {version}")

        # A rollback shows up as the device running its previous image, which
        # also clears the recorded app hash.
        try:
            final = get_json(base, "/ota/status")
        except Exception:
            final = {}
        if send_app and final.get("app_sha256", "") != app_sha:
            log("")
            log("WARNING: the device is NOT running the image just sent —")
            log("  its self-check failed and the bootloader rolled back.")
            log(f"  running slot: {final.get('app_running', '?')}")
            log("  the device is fine and still reachable; fix the build and re-run.")
            return EX_ROLLBACK

    log("")
    log("UPDATED")
    return EX_OK


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(EX_FAIL)
