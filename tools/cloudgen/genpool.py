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
MERGED_VARIANTS = 3
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

def portrait_pack(masks):
    """Rotate light/shadow/core for portrait-native device draw."""
    out = []
    for m in masks:
        out.append(cloudgen.to_portrait(m) if m is not None else None)
    return tuple(out)

def portrait_wh(masks):
    """Return (w,h) header dims after to_portrait."""
    m = next(m for m in masks if m is not None)
    return int(m.shape[1]), int(m.shape[0])

def write_pack(entries, pack_path):
    os.makedirs(os.path.dirname(pack_path), exist_ok=True)
    blobs = [open(path, "rb").read() for (_, _, _, _, path) in entries]
    toc_end = 6 + len(entries) * 12
    off = toc_end
    with open(pack_path, "wb") as f:
        f.write(b"CLP3")
        f.write(struct.pack("<H", len(entries)))
        for (etype, layer, pool, variant, _path), blob in zip(entries, blobs):
            f.write(struct.pack("<BBBBII", layer, pool, variant, etype, off, len(blob)))
            off += len(blob)
        for blob in blobs:
            f.write(blob)
    return off

import os as _os
PORTRAIT = _os.environ.get("CLOUDGEN_PORTRAIT", "1") != "0"

def pack_clm(path, masks, portrait=PORTRAIT):
    if portrait:
        masks = portrait_pack(masks)
        pw, ph = portrait_wh(masks)
    else:
        pw, ph = W, H
    return clm.write_clm(path, pw, ph, *masks)

def validate_merged_storm_light(merged, light):
    share = light.sum() / max((merged * 255.0).sum(), 1.0)
    assert share >= 0.50, f"merged light share {share:.2f} < 0.50"
    dense = merged > 0.5
    if dense.any():
        mean_dense = float(light[dense].mean())
        assert mean_dense >= 100.0, \
            f"merged dense-area light {mean_dense:.0f} < 100"

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
            size = pack_clm(path, masks)
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
            size = pack_clm(path, masks)
            total += size
            entries.append((CLP_TYPE_CLOUD, layer, 1, v, path))
            print(f"{os.path.basename(path)}: {size/1024:.0f} KB")
    for v in range(MERGED_VARIANTS):
        seed = 700 + v
        D_mid = cloudgen.gen_density(1, seed=seed, w=W, h=H, storm=True)
        D_low = cloudgen.gen_density(2, seed=seed, w=W, h=H, storm=True)
        merged = cloudgen.merge_strips(D_mid, D_low)
        light, _, _ = cloudgen.decompose(merged, cloudgen.PROFILES_STORM[2])
        validate_merged_storm_light(merged, light)
        path = os.path.join(OUT, f"cloud_merged_v{v}.clm")
        size = pack_clm(path, (light, None, None))
        total += size
        entries.append((CLP_TYPE_CLOUD, 0, CLOUD_POOL_STORM_MERGED, v, path))
        print(f"{os.path.basename(path)}: {size/1024:.0f} KB")

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
    for d in range(spr.BOLT_DIRECTIONS):
        for v in range(spr.BOLT_VARIANTS_PER_DIR):
            core, glow = spr.gen_bolt(seed=v, direction=d)
            # subtype = direction, variant = v  (device keys the sprite this way)
            total_sprites += add_sprite(CLP_TYPE_BOLT, d, v,
                                        (core, glow),
                                        f"bolt_d{d}_v{v}.clm")
    for ph in range(4):
        total_sprites += add_sprite(CLP_TYPE_RAY, ph, 0, (spr.gen_rays(ph),), f"ray_p{ph}.clm")
    for ph in range(spr.MOON_PHASE_COUNT):
        a, lum = spr.gen_moon(ph)
        total_sprites += add_sprite(CLP_TYPE_MOON, ph, 0, (a, lum), f"moon_p{ph}.clm")

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
