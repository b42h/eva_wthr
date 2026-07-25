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
#
# Fractal midpoint-displacement bolt. A start->end segment (whose direction is
# chosen by `direction`) is recursively subdivided; each midpoint is pushed
# perpendicular by a decaying random offset, giving the sharp self-similar zig
# of real lightning. Branches fork off endpoints at shallow recursion depth and
# are themselves subdivided with decayed displacement + intensity. Core is
# rasterized 1px THIN; the wide read comes from the separate blurred `glow`
# plane, not a fat core. Direction is BAKED so the device just picks a cached
# sprite by (subtype=direction, variant=seed) — no runtime geometry.

BOLT_DIR_NAMES = ["down", "down_left", "down_right", "up", "up_left", "up_right", "intracloud"]
BOLT_DIRECTIONS = len(BOLT_DIR_NAMES)
BOLT_VARIANTS_PER_DIR = 4
BOLT_SUBDIV_DEPTH = 6         # 2^6 = 64 base segments along the trunk
BOLT_BRANCH_PROB = 0.16       # per trunk segment-endpoint chance to fork
BOLT_BRANCH_DEPTH_MAX = 2     # branches may spawn sub-branches up to here

def _bolt_endpoints(rng, direction, w, h):
    """Return (sx,sy,ex,ey) for a named direction, in pixels. 'down' enters top,
    exits bottom; 'up' is vertically mirrored; *_left/right add horizontal span;
    'intracloud' is a shallow horizontal crawl in the upper sky."""
    name = BOLT_DIR_NAMES[direction]
    if name == "down":
        return (w*rng.uniform(0.4,0.6), 0.0,        w*rng.uniform(0.4,0.6), h*rng.uniform(0.9,1.0))
    if name == "down_left":
        return (w*rng.uniform(0.7,0.95), 0.0,       w*rng.uniform(0.05,0.3), h*rng.uniform(0.85,1.0))
    if name == "down_right":
        return (w*rng.uniform(0.05,0.3), 0.0,       w*rng.uniform(0.7,0.95), h*rng.uniform(0.85,1.0))
    if name == "up":
        return (w*rng.uniform(0.4,0.6), h*1.0,       w*rng.uniform(0.4,0.6), h*rng.uniform(0.0,0.1))
    if name == "up_left":
        return (w*rng.uniform(0.7,0.95), h*1.0,      w*rng.uniform(0.05,0.3), h*rng.uniform(0.0,0.15))
    if name == "up_right":
        return (w*rng.uniform(0.05,0.3), h*1.0,      w*rng.uniform(0.7,0.95), h*rng.uniform(0.0,0.15))
    # intracloud: shallow near-horizontal crawl high in the frame
    y0 = h*rng.uniform(0.08, 0.30)
    return (w*rng.uniform(0.05,0.25), y0,            w*rng.uniform(0.75,0.95), y0 + h*rng.uniform(0.02,0.12))

def _subdivide(rng, x0, y0, x1, y1, disp, depth, out):
    if depth == 0:
        out.append((x0, y0, x1, y1))
        return
    mx, my = (x0+x1)*0.5, (y0+y1)*0.5
    dx, dy = x1-x0, y1-y0
    L = (dx*dx + dy*dy) ** 0.5 + 1e-6
    nx, ny = -dy/L, dx/L
    off = rng.normal(0.0, disp)
    mx += nx*off; my += ny*off
    _subdivide(rng, x0, y0, mx, my, disp*0.55, depth-1, out)
    _subdivide(rng, mx, my, x1, y1, disp*0.55, depth-1, out)

def _rasterize(core, x0, y0, x1, y1, intensity):
    h, w = core.shape
    n = int(((x1-x0)**2 + (y1-y0)**2) ** 0.5) + 1
    for i in range(n):
        t = i / max(n-1, 1)
        xi = int(min(max(x0 + (x1-x0)*t, 0), w-1))
        yi = int(min(max(y0 + (y1-y0)*t, 0), h-1))
        if intensity > core[yi, xi]:
            core[yi, xi] = intensity

def gen_bolt(seed, direction=0, w=360, h=560):
    """Fractal branched bolt for one (direction, seed). Returns (core, glow) A8.
    Direction is one of BOLT_DIR_NAMES indices; the device keys the sprite by
    (subtype=direction, variant=seed) and never recomputes geometry at runtime."""
    import math
    rng = np.random.default_rng(1000 + direction*97 + seed)
    core = np.zeros((h, w), np.float64)
    sx, sy, ex, ey = _bolt_endpoints(rng, direction, w, h)
    trunk = []
    _subdivide(rng, sx, sy, ex, ey, disp=h*0.06, depth=BOLT_SUBDIV_DEPTH, out=trunk)
    for (x0, y0, x1, y1) in trunk:
        _rasterize(core, x0, y0, x1, y1, 1.0)
    # branches off trunk endpoints
    def maybe_branch(px, py, base_ang, depth):
        if depth > BOLT_BRANCH_DEPTH_MAX:
            return
        ang = base_ang + rng.uniform(-0.9, 0.9)
        blen = rng.uniform(0.10, 0.28) * h * (0.6 ** (depth-1))
        bx, by = px + math.cos(ang)*blen, py + math.sin(ang)*blen
        segs = []
        _subdivide(rng, px, py, bx, by, disp=blen*0.20, depth=4, out=segs)
        inten = rng.uniform(0.45, 0.7) * (0.7 ** (depth-1))
        for (a, b, c, d) in segs:
            _rasterize(core, a, b, c, d, inten)
        # sub-branch
        if rng.random() < BOLT_BRANCH_PROB:
            maybe_branch(bx, by, ang, depth+1)
    for (x0, y0, x1, y1) in trunk:
        if rng.random() < BOLT_BRANCH_PROB:
            base = math.atan2(y1-y0, x1-x0)
            maybe_branch(x1, y1, base, 1)
    glow = _blur(core, passes=3, k=9) * 2.6
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

MOON_PHASE_COUNT = 32   # keep in sync with genpool.py and eva_weather_canvas.c's `ph` quantizer

def gen_moon(phase, size=120, n_craters=26, phase_count=MOON_PHASE_COUNT):
    """Cratered moon with a phase terminator. phase 0 = thin crescent,
    phase_count-1 = full. The lit side is baked on the LEFT edge; the device
    mirrors the sprite for WAXING phases (lit side right, northern
    hemisphere) and draws it as stored for waning — see the moon blit in
    main/eva_weather_canvas.c. Do not flip the terminator here without
    also flipping that runtime mirror."""
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
    # Terminator: standard lunar-phase geometry. The terminator is the
    # projection of a great circle on the sphere, seen edge-on — an ELLIPSE
    # with semi-minor axis |k| = |cos(phase_angle)|, not a straight vertical
    # chord. A straight chord was tried first (and briefly "area-corrected"
    # with a circle-segment solve, 2026-07-18 first pass) but a straight
    # line can never produce the classic thin-crescent silhouette: the
    # correct crescent tip comes from two arcs (the disc edge + the
    # terminator ellipse) meeting at the poles, not a disc-edge-plus-line.
    # phase_angle: 0 = full, pi = new. illum = (1 - cos(phase_angle)) / 2.
    frac = (phase + 1) / phase_count             # illum fraction, e.g. 1/32 .. 1.0
    phase_angle = np.arccos(np.clip(2.0 * frac - 1.0, -1.0, 1.0))
    k = np.cos(phase_angle)                     # -1 (new) .. 0 (quarter) .. 1 (full)
    term_curve = k * np.sqrt(np.clip(1.0 - dy * dy, 0.0, 1.0))
    edge_soft = 6.0                             # feather width, matches old sharpness
    lit = np.clip((term_curve - dx) * edge_soft + 0.5, 0.0, 1.0) if frac < 1.0 else np.ones_like(dx)
    # The unlit side must be TRANSPARENT, not black: gate the ALPHA plane
    # (disc) with the terminator so the shadowed part lets the sky show
    # through instead of stamping a dark circle. Luminance keeps full crater
    # detail on the lit side. (2026-07-06 — was `lum *= lit`, a black disc by
    # day; the runtime blends alpha=disc × colour=lum, so alpha must carry
    # the phase.)
    disc = np.clip(disc * lit, 0.0, 1.0)
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
