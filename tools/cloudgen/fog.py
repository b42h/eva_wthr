"""Offline fog A8: a few soft horizontal bands. One frame + slow horizontal
drift on-device (no per-particle CPU). Replaces the 144-particle 32 ms path."""
import numpy as np

H, W = 480, 800

def gen_fog_band(seed=0x0f06):
    rng = np.random.default_rng(seed)
    f = np.zeros((H, W), np.float32)
    for _ in range(5):
        cy = rng.uniform(0.2 * H, 0.9 * H)
        thick = rng.uniform(30, 80)
        peak = rng.uniform(60, 110)
        yy = np.arange(H)[:, None]
        band = np.exp(-((yy - cy) ** 2) / (2 * thick ** 2)) * peak
        f += band
    return np.clip(f, 0, 200).astype(np.uint8)
