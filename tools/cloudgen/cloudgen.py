#!/usr/bin/env python3
"""Offline cloud-mask generator for eva_weather.

Generates 800x768 A8 light/shadow/core strips per layer (HIGH cirrus,
MID altocumulus, LOW cumulus) using X-periodic value-noise FBM with
domain warping and vertical-transmittance lighting.

Deps: numpy, Pillow, lz4  (pip install numpy pillow lz4)
"""
import numpy as np

# Layer ids match eva_weather_canvas.c: 0=HIGH, 1=MID, 2=LOW.
PROFILES = {
    0: dict(  # cirrus: thin X-stretched streaks, translucent
        base_nx=6, base_ny=32, octaves=5, gain=0.55,
        stretch_x=8.0, warp=22.0, warp_y_scale=0.0,
        cover=0.54, soft=0.14, dens_gain=0.68,
        cover_min=0.08, cover_max=0.45,
        atten=2.2, core_lo=0.55, core_hi=0.95, core_gain=0.25,
        ridge=0.0, ridge_upper=0.0, ridge_lower=0.0,
        shade_floor=0.72, shade_rows=22.0),
    1: dict(  # altocumulus: broken clumps (formulas unchanged)
        base_nx=10, base_ny=8, octaves=6, gain=0.50,
        stretch_x=1.4, warp=34.0, warp_y_scale=0.5,
        cover=0.52, soft=0.20, dens_gain=0.88,
        cover_min=0.15, cover_max=0.72,
        atten=3.0, core_lo=0.45, core_hi=0.85, core_gain=0.65,
        ridge=0.0, ridge_upper=0.0, ridge_lower=0.0,
        shade_floor=0.64, shade_rows=44.0),
    2: dict(  # cumulus: volumetric masses with cauliflower tops
        base_nx=5, base_ny=4, octaves=6, gain=0.52,
        stretch_x=1.1, warp=48.0, warp_y_scale=0.5,
        cover=0.46, soft=0.24, dens_gain=1.00,
        cover_min=0.32, cover_max=0.85,
        atten=3.8, core_lo=0.40, core_hi=0.80, core_gain=0.82,
        ridge=0.28, ridge_upper=1.0, ridge_lower=0.0,
        shade_floor=0.60, shade_rows=60.0),
}

PROFILES_STORM = {
    1: dict(  # storm altocumulus wall
        base_nx=8, base_ny=6, octaves=5, gain=0.48,
        stretch_x=1.2, warp=28.0, warp_y_scale=0.35,
        cover=0.38, soft=0.22, dens_gain=1.15,
        cover_min=0.70, cover_max=0.98,
        atten=3.4, core_lo=0.35, core_hi=0.90, core_gain=1.15,
        ridge=0.35, ridge_upper=0.0, ridge_lower=1.0,
        shade_floor=0.55, shade_rows=42.0),
    2: dict(  # storm cumulonimbus base
        base_nx=4, base_ny=3, octaves=6, gain=0.50,
        stretch_x=1.0, warp=40.0, warp_y_scale=0.4,
        cover=0.34, soft=0.20, dens_gain=1.20,
        cover_min=0.72, cover_max=0.98,
        atten=4.2, core_lo=0.30, core_hi=0.85, core_gain=1.25,
        ridge=0.42, ridge_upper=0.15, ridge_lower=1.0,
        shade_floor=0.52, shade_rows=40.0),
}

LAYER_MAX_SCALE = {0: 1.05, 1: 1.15, 2: 1.25}
VIEW_H = 480
STRIP_H = 768
FEATHER = 21

def _smooth(t):
    return t * t * (3.0 - 2.0 * t)

def value_noise(rng, w, h, nx, ny):
    """One octave of value noise, periodic in X (lattice wraps mod nx)."""
    lat = rng.random((ny + 1, nx))
    xs = np.linspace(0.0, nx, w, endpoint=False)
    ys = np.linspace(0.0, ny, h, endpoint=False)
    xi = np.floor(xs).astype(int)
    yi = np.floor(ys).astype(int)
    fx = _smooth(xs - xi)[None, :]
    fy = _smooth(ys - yi)[:, None]
    x0, x1 = xi % nx, (xi + 1) % nx
    y0, y1 = yi, np.minimum(yi + 1, ny)
    a = lat[np.ix_(y0, x0)]; b = lat[np.ix_(y0, x1)]
    c = lat[np.ix_(y1, x0)]; d = lat[np.ix_(y1, x1)]
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy

def fbm(rng, w, h, nx, ny, octaves, gain):
    """X-periodic FBM, output normalized to [0,1]."""
    out = np.zeros((h, w))
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        out += amp * value_noise(rng, w, h, nx << o, ny << o)
        tot += amp
        amp *= gain
    return out / tot

def warp_x(field, offs):
    """Bilinear horizontal displacement with X wrap."""
    h, w = field.shape
    xs = (np.arange(w)[None, :] + offs) % w
    x0 = np.floor(xs).astype(int) % w
    x1 = (x0 + 1) % w
    f = xs - np.floor(xs)
    rows = np.arange(h)[:, None]
    return field[rows, x0] * (1.0 - f) + field[rows, x1] * f

def warp_y(field, offs):
    """Bilinear vertical displacement, clamped (strip edges feather anyway)."""
    h, w = field.shape
    ys = np.clip(np.arange(h)[:, None] + offs, 0, h - 1.001)
    y0 = np.floor(ys).astype(int)
    y1 = np.minimum(y0 + 1, h - 1)
    f = ys - y0
    cols = np.arange(w)[None, :]
    return field[y0, cols] * (1.0 - f) + field[y1, cols] * f

def _ridged(rng, w, h, nx, ny):
    f = fbm(rng, w, h, nx, ny, 4, 0.55)
    return 1.0 - np.abs(2.0 * f - 1.0)

def _profile_for(layer, storm=False):
    if storm:
        if layer not in PROFILES_STORM:
            raise ValueError(f"no storm profile for layer {layer}")
        return PROFILES_STORM[layer]
    return PROFILES[layer]

def gen_density(layer, seed, w=800, h=768, storm=False):
    """Cloud density field in [0,1], seamlessly tiling in X."""
    p = _profile_for(layer, storm)
    rng = np.random.default_rng(np.random.SeedSequence([seed, layer, int(storm)]))
    nx = max(2, int(round(p["base_nx"] / p["stretch_x"])))
    base = fbm(rng, w, h, nx, p["base_ny"], p["octaves"], p["gain"])
    wx = (fbm(rng, w, h, 4, 4, 3, 0.5) - 0.5) * 2.0 * p["warp"]
    wy = (fbm(rng, w, h, 4, 4, 3, 0.5) - 0.5) * 2.0 * p["warp"] * p.get("warp_y_scale", 0.5)
    D = warp_y(warp_x(base, wx), wy)

    if p.get("ridge", 0.0) > 0.0:
        ridge = _ridged(rng, w, h, max(2, nx // 2), max(2, p["base_ny"]))
        y_norm = np.linspace(0.0, 1.0, h)[:, None]
        upper = np.clip(1.0 - y_norm, 0.0, 1.0) ** 1.6 * p.get("ridge_upper", 0.0)
        lower = np.clip(y_norm, 0.0, 1.0) ** 1.4 * p.get("ridge_lower", 0.0)
        mask = np.clip(upper + lower, 0.0, 1.0)
        D = np.clip(D + ridge * mask * p["ridge"], 0.0, 1.0)

    D = np.clip((D - p["cover"]) / p["soft"], 0.0, 1.0)
    D = _smooth(D) * p["dens_gain"]
    fade = FEATHER
    env = np.ones(h)
    ramp = _smooth(np.linspace(0.0, 1.0, fade))
    env[:fade] = ramp
    env[-fade:] = ramp[::-1]
    return np.clip(D * env[:, None], 0.0, 1.0)

def gen_density_raw(layer, seed, w=800, h=768, storm=False):
    """Density before vertical edge feather — for viewport margin checks."""
    p = _profile_for(layer, storm)
    rng = np.random.default_rng(np.random.SeedSequence([seed, layer, int(storm)]))
    nx = max(2, int(round(p["base_nx"] / p["stretch_x"])))
    base = fbm(rng, w, h, nx, p["base_ny"], p["octaves"], p["gain"])
    wx = (fbm(rng, w, h, 4, 4, 3, 0.5) - 0.5) * 2.0 * p["warp"]
    wy = (fbm(rng, w, h, 4, 4, 3, 0.5) - 0.5) * 2.0 * p["warp"] * p.get("warp_y_scale", 0.5)
    D = warp_y(warp_x(base, wx), wy)
    if p.get("ridge", 0.0) > 0.0:
        ridge = _ridged(rng, w, h, max(2, nx // 2), max(2, p["base_ny"]))
        y_norm = np.linspace(0.0, 1.0, h)[:, None]
        upper = np.clip(1.0 - y_norm, 0.0, 1.0) ** 1.6 * p.get("ridge_upper", 0.0)
        lower = np.clip(y_norm, 0.0, 1.0) ** 1.4 * p.get("ridge_lower", 0.0)
        mask = np.clip(upper + lower, 0.0, 1.0)
        D = np.clip(D + ridge * mask * p["ridge"], 0.0, 1.0)
    D = np.clip((D - p["cover"]) / p["soft"], 0.0, 1.0)
    return np.clip(_smooth(D) * p["dens_gain"], 0.0, 1.0)

def decompose(D, p):
    """Split density into light/shadow/core A8 masks.

    IMPORTANT: the device renders ONLY the light plane (light-only
    optimization in blend_layer_variant), so light must carry the bulk of
    the density or clouds vanish on hardware.

    Shading uses LOCAL depth into each cloud mass (rows since the current
    cloud segment started in this column), NOT a global column cumsum — a
    global optical depth produces one near-horizontal extinction line
    across the whole strip ("cut texture" artifact seen on hardware
    2026-07-03) and a flat featureless sheet below it. With local depth
    every mass gets its own bright top and a smooth falloff into its
    belly; shadow keeps the remainder for previews / a future 3-plane
    re-enable."""
    eps = 0.04
    inside = D > eps
    c = np.cumsum(inside, axis=0)
    # Depth-in-segment: cumsum minus its value at the last gap row.
    last_reset = np.maximum.accumulate(np.where(~inside, c, 0), axis=0)
    depth = (c - last_reset).astype(np.float64)
    lo = p.get("shade_floor", 0.62)
    srows = p.get("shade_rows", 48.0)
    shading = lo + (1.0 - lo) * np.exp(-depth / srows)
    # Depth is per-column, and a ragged mass top makes it jump between
    # neighbouring columns — on the panel that reads as thin vertical
    # "texture cut" lines (hardware artifact 2026-07-03). Smooth the
    # shading horizontally (X wraps, 3 box passes ≈ gaussian σ≈15 px);
    # the vertical falloff is untouched.
    k = 15
    kernel = np.ones(k) / k
    for _ in range(3):
        pad = np.concatenate([shading[:, -k:], shading, shading[:, :k]], axis=1)
        sm = np.apply_along_axis(lambda r: np.convolve(r, kernel, mode="same"),
                                 1, pad)
        shading = sm[:, k:-k]
    light = D * shading
    shadow = D * (1.0 - shading)
    core_t = np.clip((D - p["core_lo"]) / (p["core_hi"] - p["core_lo"]), 0.0, 1.0)
    core = _smooth(core_t) * (0.35 + 0.65 * (1.0 - np.exp(-depth / srows))) \
           * p["core_gain"]

    def a8(m):
        return (np.clip(m, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)

    total = a8(D)
    l8 = a8(light)
    s8 = (total.astype(int) - l8.astype(int)).clip(0, 255).astype(np.uint8)
    return l8, s8, a8(core)

def merge_strips(d_upper, d_lower):
    """Composite two density fields into one strip (over operator, clamped).
    Upper (MID) sits behind lower (LOW) from the viewer; lower dominates."""
    a = np.asarray(d_upper, dtype=np.float32)
    b = np.asarray(d_lower, dtype=np.float32)
    if a.shape != b.shape:
        h = max(a.shape[0], b.shape[0]); w = max(a.shape[1], b.shape[1])
        def fit(x):
            y = np.zeros((h, w), np.float32); y[:x.shape[0], :x.shape[1]] = x; return y
        a, b = fit(a), fit(b)
    return np.clip(b + a * (1.0 - b), 0.0, 1.0)

def decompose_lit(D, p):
    """Storm strike variant: brighter light plane (internal illumination)."""
    light, shadow, core = decompose(D, p)
    lit = np.clip(light.astype(np.int16) + (D * 90).astype(np.int16), 0, 255).astype(np.uint8)
    return lit, shadow, core

def coverage(D):
    return float((D > 0.05).mean())

def viewport_margin_violation(D, layer, w=800, h=STRIP_H, view_h=VIEW_H):
    """At max depth scale, viewport sampling must not reach the hard strip
    margins (beyond the 21-row feather band)."""
    scale = LAYER_MAX_SCALE[layer]
    cy = h * 0.5
    step_y = (h / view_h) / scale
    hard_lo = FEATHER // 2
    hard_hi = h - FEATHER // 2
    for vy in range(view_h):
        sy = cy + (vy - view_h * 0.5) * step_y
        if sy < hard_lo or sy > hard_hi - 1:
            iy = int(np.clip(np.floor(sy), 0, h - 1))
            if D[iy, :].max() > 0.12:
                return True
    return False

def validate_pool_file(layer, seed, storm=False, w=800, h=STRIP_H):
    """Quality gates for one generated variant. Raises AssertionError on fail."""
    p = _profile_for(layer, storm)
    D = gen_density(layer, seed, w=w, h=h, storm=storm)
    cov = coverage(D)
    assert p["cover_min"] <= cov <= p["cover_max"] + 0.08, \
        f"L{layer}{'s' if storm else ''} seed {seed}: coverage {cov:.2f} outside [{p['cover_min']}, {p['cover_max']}]"
    light, shadow, core = decompose(D, p)
    total = light.astype(int) + shadow.astype(int)
    ref = (D * 255.0 + 0.5).astype(int)
    assert np.abs(total - ref).max() <= 2
    assert not viewport_margin_violation(D, layer, w=w, h=h), \
        f"L{layer}{'s' if storm else ''} seed {seed}: viewport margin violation at scale {LAYER_MAX_SCALE[layer]}"
    return D, (light, shadow, core)
