"""Offline rain-streak A8 loop generator. Frames tile vertically so a
scrolling blit loops seamlessly. Device blits one frame per tick via PPA."""
import numpy as np

H, W = 480, 800  # landscape render size (matches EVA_WEATHER_RENDER_*)

def gen_rain_loop(intensity="storm", wind_tilt=0.2, n=8, seed=0x4880):
    rng = np.random.default_rng(seed)
    density = {"light": 240, "storm": 900}[intensity]
    frames = []
    xs = rng.uniform(0, W, density)
    ys = rng.uniform(0, H, density)
    lens = rng.uniform(8, 22, density)
    for k in range(n):
        f = np.zeros((H, W), np.uint8)
        phase = (k / n) * H
        for i in range(density):
            y0 = (ys[i] + phase) % H
            for t in range(int(lens[i])):
                yy = int((y0 + t) % H)
                xx = int((xs[i] + t * wind_tilt) % W)
                a = int(160 * (1 - t / lens[i]))
                if a > f[yy, xx]:
                    f[yy, xx] = a
                xx2 = (xx + 1) % W
                if a > f[yy, xx2]:
                    f[yy, xx2] = a
        frames.append(f)
    return frames
