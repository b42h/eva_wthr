#!/usr/bin/env python3
"""Host test: cloudgen noise + decomposition invariants + pool quality gates.
Run: python3 tools/cloudgen/test_cloudgen.py
"""
import glob
import os

import numpy as np
import cloudgen

W, H = 800, 768
VARIANT_DIFF_MIN = 0.012

def test_density_seamless_x():
    for layer in range(3):
        D = cloudgen.gen_density(layer, seed=42, w=W, h=H)
        seam = np.abs(D[:, 0] - D[:, -1])
        interior = np.abs(np.diff(D, axis=1)).mean()
        assert seam.mean() < interior * 3.0, \
            f"layer {layer}: seam step {seam.mean():.4f} vs interior {interior:.4f}"

def test_density_range_and_coverage():
    for layer in range(3):
        p = cloudgen.PROFILES[layer]
        for seed in (1, 2, 3):
            D = cloudgen.gen_density(layer, seed=seed, w=W, h=H)
            assert D.dtype == np.float64
            assert D.min() >= 0.0 and D.max() <= 1.0
            cov = cloudgen.coverage(D)
            assert p["cover_min"] <= cov <= p["cover_max"] + 0.08, \
                f"layer {layer} seed {seed}: coverage {cov:.2f} out of range"

def test_storm_coverage():
    for layer in (1, 2):
        p = cloudgen.PROFILES_STORM[layer]
        D = cloudgen.gen_density(layer, seed=77, w=W, h=H, storm=True)
        cov = cloudgen.coverage(D)
        assert p["cover_min"] <= cov <= p["cover_max"] + 0.05, \
            f"storm L{layer}: coverage {cov:.2f}"

def test_decompose_masks():
    D = cloudgen.gen_density(2, seed=7, w=W, h=H)
    light, shadow, core = cloudgen.decompose(D, cloudgen.PROFILES[2])
    for m in (light, shadow, core):
        assert m.shape == (H, W) and m.dtype == np.uint8
    total = light.astype(int) + shadow.astype(int)
    ref = (D * 255.0 + 0.5).astype(int)
    assert np.abs(total - ref).max() <= 2
    assert (core > 0).mean() < (ref > 0).mean()

def test_light_carries_visible_mass():
    """The device renders ONLY the light plane (light-only optimization in
    blend_layer_variant). If light doesn't carry the bulk of the density,
    clouds are invisible on hardware regardless of how previews look."""
    for layer, storm, profile in ((2, False, cloudgen.PROFILES[2]),
                                  (2, True, cloudgen.PROFILES_STORM[2]),
                                  (1, False, cloudgen.PROFILES[1])):
        D = cloudgen.gen_density(layer, seed=31, w=W, h=H, storm=storm)
        light, _, _ = cloudgen.decompose(D, profile)
        share = light.sum() / max((D * 255.0).sum(), 1.0)
        # 0.50 floor: storm profiles deliberately shade bellies darker
        # (shade_floor 0.52) — the hard visibility guarantee is the
        # dense-area mean below, not the global share.
        assert share >= 0.50, \
            f"layer {layer} storm={storm}: light share {share:.2f} < 0.50"
        dense = D > 0.5
        if dense.any():
            mean_dense = light[dense].mean()
            assert mean_dense >= 100.0, \
                f"layer {layer} storm={storm}: dense-area light {mean_dense:.0f} < 100"

def test_no_vertical_striping():
    """Per-column depth shading must not produce column-to-column jumps —
    they render as thin vertical 'texture cut' lines on the panel
    (hardware artifact 2026-07-03). Inside dense cloud, the horizontal
    gradient of the light plane must stay small."""
    for layer, storm, profile in ((2, True, cloudgen.PROFILES_STORM[2]),
                                  (2, False, cloudgen.PROFILES[2])):
        D = cloudgen.gen_density(layer, seed=17, w=W, h=H, storm=storm)
        light, _, _ = cloudgen.decompose(D, profile)
        dense = (light[:, :-1] > 60) & (light[:, 1:] > 60)
        if not dense.any():
            continue
        dx = np.abs(light[:, 1:].astype(int) - light[:, :-1].astype(int))[dense]
        # Stripes are thin (<1 % of pixels) — p99 misses them, use the tail.
        p999 = np.percentile(dx, 99.9)
        assert p999 <= 12, \
            f"layer {layer} storm={storm}: vertical striping p99.9 {p999:.0f} > 12"

def test_variants_differ():
    for layer in range(3):
        a = cloudgen.gen_density(layer, seed=10, w=W, h=H)
        b = cloudgen.gen_density(layer, seed=11, w=W, h=H)
        assert np.abs(a - b).mean() > VARIANT_DIFF_MIN
    for layer in (1, 2):
        a = cloudgen.gen_density(layer, seed=20, w=W, h=H, storm=True)
        b = cloudgen.gen_density(layer, seed=21, w=W, h=H, storm=True)
        assert np.abs(a - b).mean() > VARIANT_DIFF_MIN

def test_viewport_margin():
    for layer in range(3):
        for seed in (0, 3, 7):
            D = cloudgen.gen_density(layer, seed, w=W, h=H)
            assert not cloudgen.viewport_margin_violation(D, layer, w=W, h=H)

def test_mid_unchanged_formula():
    """MID profile constants unchanged — regeneration should stay in family."""
    p = cloudgen.PROFILES[1]
    assert p["base_nx"] == 10 and p["stretch_x"] == 1.4 and p["cover"] == 0.52

def test_clm_roundtrip(tmpdir="/tmp"):
    import lz4.block, clm
    D = cloudgen.gen_density(0, seed=3, w=W, h=H)
    masks = cloudgen.decompose(D, cloudgen.PROFILES[0])
    path = os.path.join(tmpdir, "test.clm")
    clm.write_clm(path, W, H, *masks)
    with open(path, "rb") as f:
        blob = f.read()
    assert blob[:4] == b"CLM1"
    w = int.from_bytes(blob[4:6], "little")
    h = int.from_bytes(blob[6:8], "little")
    assert (w, h) == (W, H)
    sizes = [int.from_bytes(blob[8 + i*4:12 + i*4], "little") for i in range(3)]
    off = 20
    for size, ref in zip(sizes, masks):
        raw = lz4.block.decompress(blob[off:off + size], uncompressed_size=W * H)
        assert np.frombuffer(raw, np.uint8).reshape(H, W).tobytes() == ref.tobytes()
        off += size
    assert off == len(blob)

def test_spiffs_pool_if_present():
    root = os.path.join(os.path.dirname(__file__), "..", "..", "spiffs_image")
    paths = sorted(glob.glob(os.path.join(root, "cloud_L*.clm")))
    if not paths:
        return
    import lz4.block
    for path in paths:
        base = os.path.basename(path)
        storm = "s_v" in base
        layer = int(base.split("_")[1][1])
        with open(path, "rb") as f:
            blob = f.read()
        w = int.from_bytes(blob[4:6], "little")
        h = int.from_bytes(blob[6:8], "little")
        sizes = [int.from_bytes(blob[8 + i*4:12 + i*4], "little") for i in range(3)]
        off = 20
        blocks = []
        for size in sizes:
            raw = lz4.block.decompress(blob[off:off + size], uncompressed_size=w * h)
            blocks.append(np.frombuffer(raw, np.uint8).reshape(h, w))
            off += size
        light, shadow, core = blocks
        total = light.astype(int) + shadow.astype(int)
        dens = total.astype(np.float64) / 255.0
        cov = cloudgen.coverage(dens)
        p = cloudgen._profile_for(layer, storm)
        assert p["cover_min"] <= cov <= p["cover_max"] + 0.10, \
            f"{base}: coverage {cov:.2f} out of range"

def test_bolt_sprite():
    import sprites
    for v in range(3):
        core, glow = sprites.gen_bolt(seed=40 + v)
        assert core.shape == (560, 360) and core.dtype == np.uint8
        assert glow.shape == core.shape
        # Main channel connectivity: every row between the first and last
        # populated row has at least one core pixel (unbroken top->bottom).
        rows = np.where(core.max(axis=1) > 40)[0]
        assert rows.size > 400, "channel too short"
        full = np.arange(rows.min(), rows.max() + 1)
        populated = set(rows.tolist())
        gaps = [r for r in full if r not in populated]
        assert not gaps, f"channel broken at rows {gaps[:5]}"
        # Branching exists but doesn't flood the sprite.
        cov = (core > 20).mean()
        assert 0.005 < cov < 0.10, f"core coverage {cov:.3f}"
        assert (glow > 8).mean() > cov, "glow must be wider than core"

def test_ray_sprite_phases():
    import sprites
    prev = None
    for ph in range(4):
        a = sprites.gen_rays(phase=ph)
        assert a.shape == (480, 480) and a.dtype == np.uint8
        assert 0.05 < (a > 10).mean() < 0.60
        if prev is not None:
            assert np.abs(a.astype(int) - prev.astype(int)).mean() > 0.5, \
                "phases must differ (animation)"
        prev = a

def test_moon_phases_monotonic():
    import sprites
    n = sprites.MOON_PHASE_COUNT
    # The terminator now gates the ALPHA plane (shadow side transparent, not
    # black), so the VISIBLE (alpha) area is what grows with phase.
    vis_prev = -1
    for ph in range(n):
        alpha, lum = sprites.gen_moon(phase=ph)
        assert alpha.shape == (120, 120) and lum.shape == (120, 120)
        vis = int((alpha > 60).sum())
        assert vis > vis_prev, f"phase {ph}: visible area must grow"
        vis_prev = vis
    # Shadow side must be transparent: an early crescent covers well under
    # the full disc's alpha area.
    a1, _ = sprites.gen_moon(phase=1)
    a_full, _ = sprites.gen_moon(phase=n - 1)
    assert (a1 > 60).sum() < 0.6 * (a_full > 60).sum(), "shadow side not transparent"
    # Full moon shows crater texture: luminance variance inside the lit disc.
    _, lum_full = a_full, sprites.gen_moon(phase=n - 1)[1]
    inside = a_full > 128
    assert lum_full[inside].std() > 8, "craters missing"

def test_drop_sprites():
    import sprites
    for size in range(3):
        for shape in range(3):
            alpha, spec = sprites.gen_drop(size_class=size, shape=shape)
            assert alpha.shape[0] >= 24 and alpha.dtype == np.uint8
            # Specular highlight strictly inside the drop footprint.
            assert (spec[alpha < 30] == 0).all(), "specular outside footprint"
            assert spec.max() > 150, "specular too weak"
    for v in range(2):
        trail = sprites.gen_trail(variant=v)
        assert trail.shape == (160, 16)

def test_merged_storm_strip_light_carries_mass():
    D_mid = cloudgen.gen_density(1, seed=77, w=W, h=H, storm=True)
    D_low = cloudgen.gen_density(2, seed=77, w=W, h=H, storm=True)
    merged = cloudgen.merge_strips(D_mid, D_low)
    light, _, _ = cloudgen.decompose(merged, cloudgen.PROFILES_STORM[2])
    assert light.mean() > 12, "merged storm light plane too sparse to see on panel"

def test_rain_loop_frames_nonempty_and_tileable():
    import rain
    frames = rain.gen_rain_loop(intensity="storm", wind_tilt=0.2, n=8)
    assert len(frames) == 8
    for f in frames:
        assert f.dtype == np.uint8 and f.mean() > 3, "rain frame too sparse"
    top = frames[0][:8, :].mean(); bot = frames[0][-8:, :].mean()
    assert abs(top - bot) < 6, "rain frame not vertically tileable (visible seam)"

def test_fog_band_nonempty():
    import fog
    assert fog.gen_fog_band().mean() > 8

def test_lit_storm_variant_brighter():
    D = cloudgen.gen_density(2, seed=77, w=W, h=H, storm=True)
    base_l, _, _ = cloudgen.decompose(D, cloudgen.PROFILES_STORM[2])
    lit_l, _, _ = cloudgen.decompose_lit(D, cloudgen.PROFILES_STORM[2])
    assert lit_l.mean() > base_l.mean() + 15, "lit variant not visibly brighter"

def test_skygen_matches_palette_dump():
    import skygen
    col = skygen.gen_column("thunderstorm", "night")
    pal = skygen.palette_dump()
    ref = pal["thunderstorm"]["night"]
    assert skygen.close(col[0], ref["top"], tol=8)
    assert skygen.close(col[-1], ref["bottom"], tol=8)

def run_all():
    test_density_seamless_x()
    test_density_range_and_coverage()
    test_storm_coverage()
    test_decompose_masks()
    test_light_carries_visible_mass()
    test_no_vertical_striping()
    test_variants_differ()
    test_viewport_margin()
    test_mid_unchanged_formula()
    test_clm_roundtrip()
    test_spiffs_pool_if_present()
    test_bolt_sprite()
    test_ray_sprite_phases()
    test_moon_phases_monotonic()
    test_drop_sprites()
    test_merged_storm_strip_light_carries_mass()
    test_rain_loop_frames_nonempty_and_tileable()
    test_fog_band_nonempty()
    test_lit_storm_variant_brighter()
    test_skygen_matches_palette_dump()

if __name__ == "__main__":
    run_all()
    print("OK")
