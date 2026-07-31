"""Offline fog A8: ONE pre-baked band, drifted horizontally on-device.

Design note (2026-07-28)
------------------------
Fog is the cheapest possible scene and should stay that way: one sprite, one
blend, no per-frame CPU synthesis and no per-particle work. Two earlier
approaches were both wrong for different reasons:

  * 64-144 CPU-blended soft circles ("P_FOG"): 33.5 ms/frame, ~13 Hz, and it
    looked like overlapping bubbles with visible rims.
  * A runtime 96x60 noise field expanded on the CPU each rebuild: correct
    look, but it needed 8 pieces of state, a 375 KB scratch buffer and a
    sliced-expand state machine — a lot of moving parts for haze.

This module bakes the field ONCE, offline, at full resolution. The device
then only scrolls X and issues a single PPA blend.

The band is deliberately X-PERIODIC so the device can scroll it forever with
no seam, and its top edge is noise-modulated (not a flat gaussian ridge) —
that ragged edge is what makes it read as rolling fog rather than a grey wash.
Energy is concentrated in the lower ~2/3 of the frame: fog hugs the ground.
"""
import numpy as np

H, W = 480, 800

# Rows above this are always empty, so the device can blend just the lower
# band instead of the whole screen. Keep in sync with EVA_FOG_SPRITE_Y0 in
# main/eva_weather_canvas.c.
BAND_Y0 = 112


def _smooth(t):
    return t * t * (3.0 - 2.0 * t)


def _vnoise_periodic(rng, w, h, nx, ny):
    """Value noise that wraps exactly in X (lattice column nx == column 0)."""
    lat = rng.random((ny + 1, nx + 1))
    lat[:, -1] = lat[:, 0]
    xs = np.linspace(0.0, nx, w, endpoint=False)
    ys = np.linspace(0.0, ny, h, endpoint=False)
    xi = np.floor(xs).astype(int)
    yi = np.floor(ys).astype(int)
    fx = _smooth(xs - xi)[None, :]
    fy = _smooth(ys - yi)[:, None]
    a = lat[np.ix_(yi, xi)]
    b = lat[np.ix_(yi, xi + 1)]
    c = lat[np.ix_(yi + 1, xi)]
    d = lat[np.ix_(yi + 1, xi + 1)]
    top = a + (b - a) * fx
    bot = c + (d - c) * fx
    return top + (bot - top) * fy


def _fbm(rng, w, h, nx, ny, octaves=4, gain=0.55):
    out = np.zeros((h, w))
    amp, tot = 1.0, 0.0
    for o in range(octaves):
        out += amp * _vnoise_periodic(rng, w, h, nx << o, ny << o)
        tot += amp
        amp *= gain
    return out / tot


def gen_fog_band(seed=0x0f06):
    """Single X-periodic fog band, A8. Bottom-weighted with a ragged top."""
    rng = np.random.default_rng(seed)
    body = _fbm(rng, W, H, 4, 3, octaves=4, gain=0.58)
    body = (body - body.min()) / (np.ptp(body) + 1e-9)

    # Ragged top edge: per-column start height from low-frequency noise. A
    # flat start reads as a wash; this is what gives the rolling silhouette.
    edge = 0.72 * _vnoise_periodic(rng, W, 1, 6, 1)[0] \
         + 0.28 * _vnoise_periodic(rng, W, 1, 13, 1)[0]
    start = 0.34 + (edge - 0.5) * 0.20               # fraction of H

    v = np.linspace(0.0, 1.0, H)[:, None]
    band = np.clip((v - start[None, :]) / 0.24, 0.0, 1.0)
    band = _smooth(band)

    dens = (0.45 + 0.55 * body) * band
    # Peak alpha ~200: fog is dense but never fully opaque.
    out = np.clip(dens * 200.0, 0, 200).astype(np.uint8)
    # Hard-guarantee the BAND_Y0 contract the device relies on to blend only
    # the lower band. The ragged edge is random, so clamp rather than trust a
    # hand-tuned constant: anything that strays above the line is cleared.
    out[:BAND_Y0, :] = 0
    return out
