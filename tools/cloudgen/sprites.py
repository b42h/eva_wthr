#!/usr/bin/env python3
"""Sprite generators: lightning bolts, sun rays, moon phases, glass drops.
Shares the .clm container with clouds; plane semantics per sprite type —
see the sprite type map in the plan / spec."""
import numpy as np

def _blur(a, passes=3, k=5):
    """Cheap separable box blur (≈ gaussian after 3 passes)."""
    out = a.astype(np.float64)
    kern = np.ones(k) / k
    for _ in range(passes):
        out = np.apply_along_axis(lambda r: np.convolve(r, kern, "same"), 1, out)
        out = np.apply_along_axis(lambda c: np.convolve(c, kern, "same"), 0, out)
    return out

def _a8(m):
    return (np.clip(m, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)

# --- lightning -------------------------------------------------------------

BOLT_BRANCH_PROB = 0.045      # chance per walker step to spawn a side branch
BOLT_BRANCH_LEN = (18, 70)    # branch length range in steps, before depth decay
BOLT_BRANCH_DEPTH_MAX = 3     # branches stop spawning sub-branches past this depth
BOLT_BRANCH_LEN_DECAY = 0.7   # per-depth-level shrink of BOLT_BRANCH_LEN

def gen_bolt(seed, w=360, h=560):
    """Branched bolt via a biased random walker: a main channel descends the
    full height with lateral jitter; side branches split off with decaying
    intensity and die early. Core = sharp channel, glow = blurred halo.

    `walk()` deposits every point it visits straight into `core` (rather
    than buffering into a list the caller merges), so a branch that itself
    spawns a depth-2/3 sub-branch has that sub-branch actually rendered —
    an earlier version buffered sub-branch points into a list nobody read,
    silently dropping anything past depth 1."""
    rng = np.random.default_rng(seed)
    core = np.zeros((h, w))

    def deposit(x, y, intensity):
        px, py = int(x), int(y)
        core[py, px] = max(core[py, px], intensity)
        if intensity > 0.5:          # main channel is 2 px wide
            core[py, min(px + 1, w - 1)] = max(core[py, min(px + 1, w - 1)], intensity * 0.8)

    def walk(x, y, intensity, max_len, depth):
        n = 0
        while y < h - 2 and n < max_len and intensity > 0.08:
            deposit(x, y, intensity)
            y += rng.uniform(1.2, 3.2)
            x += rng.normal(0.0, 2.6) + rng.uniform(-0.6, 0.6)
            x = float(np.clip(x, 4, w - 5))
            n += 1
            if depth < BOLT_BRANCH_DEPTH_MAX and rng.random() < BOLT_BRANCH_PROB:
                bx = x + rng.normal(0.0, 2.0)
                blen = int(rng.uniform(*BOLT_BRANCH_LEN) *
                          (BOLT_BRANCH_LEN_DECAY ** depth))
                walk(bx, y, intensity * rng.uniform(0.35, 0.6), blen, depth + 1)
            intensity *= 0.995        # branches fade as they run

    # Main trunk: force it to keep going until it reaches near the bottom,
    # regardless of intensity decay (branches are separate, shorter walks
    # spawned via the same `walk()` at depth 1).
    x, y = rng.uniform(w * 0.3, w * 0.7), 0.0
    while y < h - 2:
        deposit(x, y, 1.0)
        y += rng.uniform(1.2, 3.2)
        x += rng.normal(0.0, 2.6) + rng.uniform(-0.6, 0.6)
        x = float(np.clip(x, 4, w - 5))
        if rng.random() < BOLT_BRANCH_PROB:
            bx = x + rng.normal(0.0, 2.0)
            blen = int(rng.uniform(*BOLT_BRANCH_LEN))
            walk(bx, y, rng.uniform(0.35, 0.6), blen, 1)

    # Guarantee row continuity of the main channel: fill any skipped rows
    # by interpolating between neighbours (walker can jump 3 rows).
    ys = np.where(core.max(axis=1) > 0.5)[0]
    for y0, y1 in zip(ys[:-1], ys[1:]):
        if y1 - y0 > 1:
            x0 = core[y0].argmax(); x1 = core[y1].argmax()
            for yy in range(y0 + 1, y1):
                t = (yy - y0) / (y1 - y0)
                xi = int(round(x0 + (x1 - x0) * t))
                xi = int(np.clip(xi, 0, w - 1))
                core[yy, xi] = 1.0
    glow = _blur(core, passes=3, k=9) * 3.0
    return _a8(core), _a8(glow)

# --- sun rays ---------------------------------------------------------------

def gen_rays(phase, size=480, n_rays=13):
    """Radial ray fan with angular noise; `phase` (0-3) rotates slightly and
    modulates intensity so cycling phases reads as breathing light."""
    rng = np.random.default_rng(1234)          # same fan every phase
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    cx = cy = size / 2.0
    dx, dy = xx - cx, yy - cy
    r = np.sqrt(dx * dx + dy * dy) / (size / 2.0)
    ang = np.arctan2(dy, dx)
    rot = phase * 0.035                        # subtle per-phase rotation
    widths = rng.uniform(0.05, 0.16, n_rays)
    angles = np.sort(rng.uniform(-np.pi, np.pi, n_rays))
    gains = rng.uniform(0.5, 1.0, n_rays) * (0.85 + 0.15 * np.cos(phase * 1.7))
    field = np.zeros_like(r)
    for a0, wdt, g in zip(angles, widths, gains):
        d = np.angle(np.exp(1j * (ang - a0 - rot)))
        field += g * np.exp(-(d / wdt) ** 2)
    falloff = np.clip(1.0 - r, 0.0, 1.0) ** 1.6
    return _a8(field * falloff * 0.55)

# --- moon -------------------------------------------------------------------

def gen_moon(phase, size=120, n_craters=26):
    """Cratered moon with a phase terminator. phase 0 = thin crescent,
    7 = full. Waning is handled at runtime by mirroring."""
    rng = np.random.default_rng(77)            # same craters every phase
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    c = (size - 1) / 2.0
    dx, dy = (xx - c) / c, (yy - c) / c
    r2 = dx * dx + dy * dy
    disc = np.clip((1.0 - r2) * 8.0, 0.0, 1.0)          # soft-edged disc
    lum = np.full_like(disc, 0.82)
    for _ in range(n_craters):
        a = rng.uniform(0, 2 * np.pi); rad = np.sqrt(rng.uniform(0, 0.85))
        px, py = np.cos(a) * rad, np.sin(a) * rad
        cr = rng.uniform(0.03, 0.14)
        d2 = (dx - px) ** 2 + (dy - py) ** 2
        # 0.28 = max crater darkness; the uniform(0.4, 1.0) factor varies it
        # per-crater so the surface doesn't look like uniform stamped dots.
        lum -= 0.28 * np.exp(-d2 / (cr * cr)) * rng.uniform(0.4, 1.0)
    # Terminator: illuminated fraction grows with phase; the shadow edge is
    # an ellipse sweeping across (simple but reads correctly at 120 px).
    frac = (phase + 1) / 8.0                    # 0.125 .. 1.0
    term = -1.0 + 2.0 * frac                    # -0.75 .. +1.0
    lit = np.clip((term - dx) * 6.0 + 0.5, 0.0, 1.0) if frac < 1.0 else np.ones_like(dx)
    lum = np.clip(lum * lit, 0.0, 1.0)
    return _a8(disc), _a8(lum)

# --- glass drops ------------------------------------------------------------

def gen_drop(size_class, shape, base=24):
    """Water bead on glass: dome alpha + top-left specular + darker lower rim."""
    size = base + size_class * 12               # 24 / 36 / 48
    rng = np.random.default_rng(500 + size_class * 10 + shape)
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float64)
    c = (size - 1) / 2.0
    ex = rng.uniform(0.85, 1.15)                # slight ellipse per shape
    dx, dy = (xx - c) / (c * ex), (yy - c) / c
    r2 = dx * dx + dy * dy
    dome = np.clip(1.0 - r2, 0.0, 1.0) ** 0.7
    alpha = dome * 0.55
    rim = np.clip(r2 - 0.55, 0, 1) * dome * 0.9     # darker edge = lens read
    alpha = np.clip(alpha + rim * 0.4, 0, 1)
    sx, sy = -0.35, -0.4                             # top-left highlight
    spec = np.exp(-(((dx - sx) ** 2 + (dy - sy) ** 2) / 0.06)) * dome
    spec[alpha < 30 / 255.0] = 0.0
    return _a8(alpha), _a8(spec)

def gen_trail(variant, w=16, h=160):
    """Wet streak a sliding drop leaves behind; ragged edges, fades upward."""
    rng = np.random.default_rng(900 + variant)
    yy = np.linspace(0.0, 1.0, h)[:, None]
    xx = np.linspace(-1.0, 1.0, w)[None, :]
    width = 0.55 + 0.25 * np.sin(yy * 9.0 + variant) + rng.normal(0, 0.05, (h, 1))
    body = np.clip(1.0 - (np.abs(xx) / np.clip(width, 0.2, 1.0)), 0, 1)
    fade = yy ** 0.7                            # strongest near the drop
    return _a8(body * fade * 0.45)
