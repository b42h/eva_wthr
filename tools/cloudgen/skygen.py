"""Offline sky keyframe column generator — matches eva_sky_palette + fill_sky_rows."""
import json
import math
import os
import subprocess

import numpy as np

RENDER_H = 480

KIND_NAMES = {
    1: "clear-day",
    2: "clear-night",
    3: "partly-cloudy-day",
    4: "partly-cloudy-night",
    5: "cloudy",
    6: "fog",
    7: "rain",
    8: "heavy-rain",
    9: "snow",
    10: "thunderstorm",
    11: "sleet",
    12: "hail",
}

DAYPART_NAMES = ["day", "sunset", "night", "sunrise"]

RENDER_H = 480

BAYER8 = (
     0, 48, 12, 60,  3, 51, 15, 63,
    32, 16, 44, 28, 35, 19, 47, 31,
     8, 56,  4, 52, 11, 59,  7, 55,
    40, 24, 36, 20, 43, 27, 39, 23,
     2, 50, 14, 62,  1, 49, 13, 61,
    34, 18, 46, 30, 33, 17, 45, 29,
    10, 58,  6, 54,  9, 57,  5, 53,
    42, 26, 38, 22, 41, 25, 37, 21,
)

def _dither_u8(c, x, y, step):
    t = BAYER8[(y & 7) * 8 + (x & 7)]
    v = int(c) + ((t - 32) * step + 32) // 64
    return max(0, min(255, v))

def _dither565(r8, g8, b8, x, y):
    r = _dither_u8(r8, x, y, 8)
    g = _dither_u8(g8, x, y, 4)
    b = _dither_u8(b8, x, y, 8)
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)

def close(a, b, tol=8):
    """Compare RGB565 to 8-bit RGB tuple within tolerance."""
    ar = (int(a) >> 11) & 0x1F; ag = (int(a) >> 5) & 0x3F; ab = int(a) & 0x1F
    ar = ar * 255 // 31; ag = ag * 255 // 63; ab = ab * 255 // 31
    br, bg, bb = b
    return abs(ar - br) <= tol and abs(ag - bg) <= tol and abs(ab - bb) <= tol

def gen_column_from_palette(top_rgb, bot_rgb):
    tr, tg, tb = top_rgb
    br, bg, bb = bot_rgb
    col = np.zeros(RENDER_H, np.uint16)
    for y in range(RENDER_H):
        yn = y / (RENDER_H - 1)
        t = yn ** 2.2
        ti = int(t * 255 + 0.5)
        r = tr + ((br - tr) * ti) // 255
        g = tg + ((bg - tg) * ti) // 255
        b = tb + ((bb - tb) * ti) // 255
        col[y] = _dither565(r, g, b, 0, y)
    return col

def palette_dump():
    root = os.path.join(os.path.dirname(__file__), "..", "..")
    skypreview = os.path.join(root, "skypreview")
    if not os.path.isfile(skypreview):
        subprocess.check_call(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-lm", "-I", "main",
             "-o", skypreview, "tools/skypreview.c"], cwd=root)
    raw = subprocess.check_output([skypreview, "--dump-json"], cwd=root, text=True)
    arr = json.loads(raw)
    out = {}
    for e in arr:
        kname = KIND_NAMES.get(e["kind"], str(e["kind"]))
        dp = e["daypart"]
        out.setdefault(kname, {})[dp] = {"top": e["top"], "bottom": e["bottom"]}
    return out

def gen_column(kind, daypart):
    pal = palette_dump()[kind][daypart]
    return gen_column_from_palette(tuple(pal["top"]), tuple(pal["bottom"]))
