#!/usr/bin/env python3
"""Render preview_contact_sheet.png: every variant × day/night/storm tints
composited over a sky gradient. Approve the look here BEFORE flashing.

  --golden   save current sheet to golden/ and exit
  (default)  render was/now/diff triptych if golden exists
"""
import argparse
import os
import shutil

import numpy as np
from PIL import Image, ImageDraw
import cloudgen
import sprites as spr

# (name, sky_top, sky_bottom, tint_light, tint_shadow, tint_core)
SCENES = [
    ("day",   (86, 148, 212), (168, 204, 236), (250, 250, 252), (150, 160, 178), (110, 120, 140)),
    ("night", (10, 14, 30),   (24, 30, 52),    (74, 80, 100),   (34, 38, 54),    (18, 20, 32)),
    ("storm", (60, 66, 80),   (110, 116, 128), (170, 172, 180), (62, 64, 74),    (28, 30, 38)),
]
VIEW_H = 480
HERE = os.path.dirname(__file__)
GOLDEN_DIR = os.path.join(HERE, "golden")
GOLDEN_PATH = os.path.join(GOLDEN_DIR, "preview_contact_sheet.png")

def composite(D_masks, scene, w, h):
    """LIGHT-ONLY composite — must match the device, which renders only the
    light plane (light-only optimization in blend_layer_variant). Shadow and
    core planes exist in the .clm files but are NOT drawn on hardware, so
    previewing them here would approve a look the panel can't show."""
    _, top, bot, tl, ts, tc = (scene[0], *scene[1:])
    ys = np.linspace(0.0, 1.0, VIEW_H)[:, None, None]
    sky = (1 - ys) * np.array(top)[None, None, :] + ys * np.array(bot)[None, None, :]
    y0 = (h - VIEW_H) // 2
    out = sky.copy()
    light = D_masks[0]
    a = light[y0:y0 + VIEW_H].astype(np.float64)[:, :, None] / 255.0
    out = out * (1 - a) + np.array(tl)[None, None, :] * a
    return out.astype(np.uint8)

def render_sheet(variants=5, storm_variants=3, w=800, h=768):
    thumb_w, thumb_h = 400, 240
    rows = []
    for layer in range(3):
        for v in range(variants):
            D = cloudgen.gen_density(layer, seed=100 * layer + v, w=w, h=h)
            masks = cloudgen.decompose(D, cloudgen.PROFILES[layer])
            row = [Image.fromarray(composite(masks, s, w, h)).resize((thumb_w, thumb_h))
                   for s in SCENES]
            rows.append((f"L{layer}v{v}", row))
    for layer in (1, 2):
        for v in range(storm_variants):
            D = cloudgen.gen_density(layer, seed=900 + 100 * layer + v, w=w, h=h, storm=True)
            masks = cloudgen.decompose(D, cloudgen.PROFILES_STORM[layer])
            row = [Image.fromarray(composite(masks, s, w, h)).resize((thumb_w, thumb_h))
                   for s in SCENES]
            rows.append((f"L{layer}s{v}", row))
    sheet = Image.new("RGB", (thumb_w * len(SCENES), thumb_h * len(rows)))
    for i, (_, row) in enumerate(rows):
        for j, im in enumerate(row):
            sheet.paste(im, (j * thumb_w, i * thumb_h))
    return sheet

def render_sprites_sheet():
    """Bolt gallery, moon 8-phase strip, drop sheet, ray phases — each tile
    labeled, laid out left-to-right wrapping at 1200 px, matching the plain
    grid-paste convention of render_sheet()."""
    tiles = []
    storm = np.full((560, 360, 3), (70, 74, 86), np.uint8)
    for v in range(8):
        core, glow = spr.gen_bolt(seed=40 + v)
        img = storm.astype(np.float64)
        for plane, tint in ((glow, (188, 210, 255)), (core, (248, 252, 255))):
            a = plane.astype(np.float64)[:, :, None] / 255.0
            img = img * (1 - a) + np.array(tint) * a
        tiles.append(("bolt %d" % v, img.astype(np.uint8)))
    for ph in range(8):
        a, lum = spr.gen_moon(ph)
        night = np.full((120, 120, 3), (12, 16, 30), np.uint8).astype(np.float64)
        al = a.astype(np.float64)[:, :, None] / 255.0
        col = (lum.astype(np.float64)[:, :, None] / 255.0) * np.array((228, 228, 218))
        tiles.append(("moon %d" % ph, (night * (1 - al) + col * al).astype(np.uint8)))
    for ph in range(4):
        a = spr.gen_rays(ph).astype(np.float64)[:, :, None] / 255.0
        day = np.full((480, 480, 3), (120, 178, 224), np.uint8).astype(np.float64)
        tiles.append(("rays %d" % ph,
                      (day * (1 - a) + np.array((255, 246, 214)) * a).astype(np.uint8)))
    for size_c in range(3):
        for shape in range(3):
            al, sp = spr.gen_drop(size_c, shape)
            bg = np.full(al.shape + (3,), (96, 108, 122), np.uint8).astype(np.float64)
            aa = al.astype(np.float64)[:, :, None] / 255.0
            img = bg * (1 - aa * 0.5) + np.array((210, 220, 232)) * aa * 0.5
            ss = sp.astype(np.float64)[:, :, None] / 255.0
            img = img * (1 - ss) + 255.0 * ss
            tiles.append(("drop %d/%d" % (size_c, shape), img.astype(np.uint8)))

    LABEL_H = 20
    MAX_W = 1200
    pil_tiles = []
    for label, arr in tiles:
        im = Image.fromarray(arr)
        cell = Image.new("RGB", (im.width, im.height + LABEL_H), (20, 20, 20))
        cell.paste(im, (0, 0))
        d = ImageDraw.Draw(cell)
        d.text((4, im.height + 3), label, fill=(255, 255, 255))
        pil_tiles.append(cell)

    # Pack left-to-right, wrapping at MAX_W, row height = tallest tile in row.
    rows = []
    row, row_w, row_h = [], 0, 0
    for cell in pil_tiles:
        if row and row_w + cell.width > MAX_W:
            rows.append((row, row_w, row_h))
            row, row_w, row_h = [], 0, 0
        row.append(cell)
        row_w += cell.width
        row_h = max(row_h, cell.height)
    if row:
        rows.append((row, row_w, row_h))

    sheet_w = max(rw for _, rw, _ in rows)
    sheet_h = sum(rh for _, _, rh in rows)
    sheet = Image.new("RGB", (sheet_w, sheet_h), (20, 20, 20))
    y = 0
    for row, _, row_h in rows:
        x = 0
        for cell in row:
            sheet.paste(cell, (x, y))
            x += cell.width
        y += row_h
    return sheet


def diff_triptych(was_path, now_im):
    was = Image.open(was_path).convert("RGB")
    now = now_im.convert("RGB")
    if was.size != now.size:
        now = now.resize(was.size)
    was_a = np.array(was, dtype=np.int16)
    now_a = np.array(now, dtype=np.int16)
    diff = np.clip(np.abs(now_a - was_a) * 4, 0, 255).astype(np.uint8)
    trip = Image.new("RGB", (was.width * 3, was.height))
    trip.paste(was, (0, 0))
    trip.paste(now, (was.width, 0))
    trip.paste(Image.fromarray(diff), (was.width * 2, 0))
    return trip

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden", action="store_true", help="snapshot current sheet into golden/")
    ap.add_argument("--variants", type=int, default=5)
    ap.add_argument("--out", default=None,
                     help="output path (default: preview_contact_sheet.png, "
                          "or sprites_sheet.png with --sprites)")
    ap.add_argument("--sprites", action="store_true",
                     help="render sprites_sheet.png (bolt/moon/rays/drop gallery) and exit")
    args = ap.parse_args()

    if args.sprites:
        sheet = render_sprites_sheet()
        out = args.out or "sprites_sheet.png"
        sheet.save(out)
        print(f"wrote {out}")
        return

    args.out = args.out or "preview_contact_sheet.png"
    sheet = render_sheet(variants=args.variants)
    if args.golden:
        os.makedirs(GOLDEN_DIR, exist_ok=True)
        sheet.save(GOLDEN_PATH)
        shutil.copy(GOLDEN_PATH, args.out)
        print(f"golden saved: {GOLDEN_PATH}")
        return

    sheet.save(args.out)
    print(f"wrote {args.out}")
    if os.path.isfile(GOLDEN_PATH):
        trip = diff_triptych(GOLDEN_PATH, sheet)
        trip_path = os.path.splitext(args.out)[0] + "_golden_diff.png"
        trip.save(trip_path)
        print(f"wrote {trip_path} (was | now | amplified diff)")

if __name__ == "__main__":
    main()
