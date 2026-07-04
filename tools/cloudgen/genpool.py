#!/usr/bin/env python3
"""Generate the full .clm pool into out/ and pack assets/clouds.bin."""
import os
import struct
import sys

import numpy as np
import cloudgen, clm
import skygen
import sprites as spr
import test_cloudgen

NORMAL_VARIANTS = 4
STORM_VARIANTS = 3
W, H = 800, 768
OUT = os.path.join(os.path.dirname(__file__), "out")
PACK = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "clouds.bin")

CLP_TYPE_CLOUD = 0
CLP_TYPE_BOLT  = 1
CLP_TYPE_RAY   = 2
CLP_TYPE_MOON  = 3
CLP_TYPE_DROP  = 4
CLP_TYPE_TRAIL = 5
CLP_TYPE_RAIN  = 6
CLP_TYPE_FOG   = 7
CLP_TYPE_SKY   = 8

CLOUD_POOL_NORMAL       = 0
CLOUD_POOL_STORM        = 1
CLOUD_POOL_STORM_MERGED = 2
CLOUD_POOL_STORM_LIT    = 3

# Soft budget raised for repartitioned 11M storage partition.
SOFT_BUDGET = int(10.5 * 1024 * 1024)
PARTITION_BUDGET = 11 * 1024 * 1024 - 65536

def write_pack(entries, pack_path):
    os.makedirs(os.path.dirname(pack_path), exist_ok=True)
    blobs = [open(path, "rb").read() for (_, _, _, _, path) in entries]
    toc_end = 6 + len(entries) * 12
    off = toc_end
    with open(pack_path, "wb") as f:
        f.write(b"CLP2")
        f.write(struct.pack("<H", len(entries)))
        for (etype, layer, pool, variant, _path), blob in zip(entries, blobs):
            f.write(struct.pack("<BBBBII", layer, pool, variant, etype, off, len(blob)))
            off += len(blob)
        for blob in blobs:
            f.write(blob)
    return off

def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    entries = []
    for layer in range(3):
        for v in range(NORMAL_VARIANTS):
            seed = 100 * layer + v
            cloudgen.validate_pool_file(layer, seed, storm=False, w=W, h=H)
            D = cloudgen.gen_density(layer, seed=seed, w=W, h=H)
            masks = cloudgen.decompose(D, cloudgen.PROFILES[layer])
            path = os.path.join(OUT, f"cloud_L{layer}_v{v}.clm")
            size = clm.write_clm(path, W, H, *masks)
            total += size
            entries.append((CLP_TYPE_CLOUD, layer, 0, v, path))
            print(f"{os.path.basename(path)}: {size/1024:.0f} KB")
    for layer in (1, 2):
        for v in range(STORM_VARIANTS):
            seed = 900 + 100 * layer + v
            cloudgen.validate_pool_file(layer, seed, storm=True, w=W, h=H)
            D = cloudgen.gen_density(layer, seed=seed, w=W, h=H, storm=True)
            masks = cloudgen.decompose(D, cloudgen.PROFILES_STORM[layer])
            path = os.path.join(OUT, f"cloud_L{layer}s_v{v}.clm")
            size = clm.write_clm(path, W, H, *masks)
            total += size
            entries.append((CLP_TYPE_CLOUD, layer, 1, v, path))
            print(f"{os.path.basename(path)}: {size/1024:.0f} KB")
    # merged (STORM_MERGED) + lit (STORM_LIT) storm pools NOT packed
    # (2026-07-04): the on-device merged path never engaged and storm
    # rendered as thin washed-out clouds; reverted to the plain per-layer
    # STORM pool above. cloudgen.merge_strips/decompose_lit remain for
    # possible future use but are unused here.

    print(f"CLOUDS: {total/1024/1024:.2f} MB")
    if total > SOFT_BUDGET:
        print("OVER SOFT BUDGET — fewer variants or half-res")
        sys.exit(1)

    def add_sprite(etype, subtype, variant, planes, name):
        path = os.path.join(OUT, name)
        h, w = planes[0].shape
        a, b, c3 = (list(planes) + [None, None])[:3]
        size = clm.write_clm(path, w, h, a, b, c3)
        entries.append((etype, subtype, 0, variant, path))
        print(f"{name}: {size/1024:.0f} KB")
        return size

    total_sprites = 0
    for v in range(8):
        core, glow = spr.gen_bolt(seed=40 + v)
        total_sprites += add_sprite(CLP_TYPE_BOLT, 0, v, (core, glow), f"bolt_v{v}.clm")
    for ph in range(4):
        total_sprites += add_sprite(CLP_TYPE_RAY, ph, 0, (spr.gen_rays(ph),), f"ray_p{ph}.clm")
    for ph in range(8):
        a, lum = spr.gen_moon(ph)
        total_sprites += add_sprite(CLP_TYPE_MOON, ph, 0, (a, lum), f"moon_p{ph}.clm")
    # Glass rain DROP + TRAIL sprites NOT packed (2026-07-04): the on-device
    # per-drop sprite blit + full-screen wet-glass composite cost ~40 ms/frame
    # and were removed from composite_glass_overlay(). The sprite lookup now
    # simply misses; nothing draws them.

    # Rain is NOT pre-baked as sprites (2026-07-04): 32 full-screen 800×480
    # frames cost 11.7 MB resident PSRAM, which starved the cloud strip
    # allocations (LOW-layer alloc failed → reboot loop). The device's
    # primitive per-particle rain draw is cheap (~1-2 ms) and looks fine, so
    # the RAIN sprite path is intentionally left unpopulated; the canvas
    # falls through to the particle loop when the sprite lookup misses.
    # (fog band likewise dropped — the CPU fog blobs stay.)

    pal = skygen.palette_dump()
    sky_entries = (
        ("clear-day", 1),
        ("partly-cloudy-day", 3),
        ("cloudy", 5),
        ("fog", 6),
        ("rain", 7),
        ("heavy-rain", 8),
        ("thunderstorm", 10),
        ("snow", 9),
    )
    for kind_name, ki in sky_entries:
        for di, dp in enumerate(skygen.DAYPART_NAMES):
            col = skygen.gen_column_from_palette(
                tuple(pal[kind_name][dp]["top"]), tuple(pal[kind_name][dp]["bottom"]))
            plane = col.view(np.uint8).reshape(len(col), 2)
            path = os.path.join(OUT, f"sky_{kind_name}_{dp}.clm")
            size = clm.write_clm(path, 2, len(col), plane, None, None)
            entries.append((CLP_TYPE_SKY, ki, 0, di, path))
            total_sprites += size
            print(f"{os.path.basename(path)}: {size/1024:.0f} KB")

    print(f"SPRITES+FX: {total_sprites/1024:.0f} KB")
    grand_total = total + total_sprites
    print(f"GRAND TOTAL: {grand_total/1024/1024:.2f} MB")
    if grand_total > PARTITION_BUDGET:
        print("PACK OVER PARTITION"); sys.exit(1)

    pack_size = write_pack(entries, PACK)
    print(f"PACK: {PACK} = {pack_size/1024/1024:.2f} MB")
    if pack_size > PARTITION_BUDGET:
        print("PACK OVER PARTITION"); sys.exit(1)
    test_cloudgen.run_all()

if __name__ == "__main__":
    main()
