#!/usr/bin/env python3
"""Animated cloud-mask preview GIFs (Pillow only).

Examples:
  python3 animate.py --layer low --kind cloudy --wind 20
  python3 animate.py --all
"""
import argparse
import math
import os

import numpy as np
from PIL import Image
import cloudgen

W, H = 800, 768
VIEW_H = 480
FPS = 10
DURATION_S = 10.0

LAYER_MAP = {"high": 0, "mid": 1, "low": 2}
FIB = [1, 2, 3, 5, 8, 13, 21, 34, 55, 89]
BASE_SPEED = {0: 5.0, 1: 13.0, 2: 21.0}
MORPH_HOLD = {0: 55.0, 1: 34.0, 2: 21.0}
MORPH_DUR = {0: 8.0, 1: 8.0, 2: 13.0}
DEPTH_LO = {0: 0.95, 1: 0.90, 2: 0.85}
DEPTH_HI = {0: 1.05, 1: 1.15, 2: 1.25}

SCENES = {
    "day":   ((86, 148, 212), (168, 204, 236), (250, 250, 252), (150, 160, 178), (110, 120, 140)),
    "night": ((10, 14, 30), (24, 30, 52), (74, 80, 100), (34, 38, 54), (18, 20, 32)),
    "storm": ((60, 66, 80), (110, 116, 128), (170, 172, 180), (62, 64, 74), (28, 30, 38)),
    "cloudy": ((78, 92, 108), (150, 160, 170), (245, 246, 250), (140, 150, 165), (95, 105, 120)),
}

def wind_multiplier(kph):
    if kph <= 0:
        return 1.0
    m = 1.0 + math.sqrt(kph) * (1.6180339 / 3.0)
    return min(m, 8.0)

def layer_wind_exp(layer):
    return {0: 0.5, 1: 1.0 / 1.6180339, 2: 1.0}[layer]

def composite_frame(masks_a, masks_b, morph_t, scroll_x, scroll_y, drift_y,
                    scale_a, scale_b, scene, storm=False):
    top, bot, tl, ts, tc = scene
    y0 = (H - VIEW_H) // 2
    ys = np.linspace(0.0, 1.0, VIEW_H)[:, None, None]
    sky = (1 - ys) * np.array(top)[None, None, :] + ys * np.array(bot)[None, None, :]
    out = sky.copy()
    eased = morph_t * morph_t * (3.0 - 2.0 * morph_t)
    scale = scale_a + (scale_b - scale_a) * eased

    def sample(mask, x_off, y_off, sc):
        h, w = mask.shape
        cy = h * 0.5
        step_x = (w / W) / sc
        step_y = (h / VIEW_H) / sc
        frame = np.zeros((VIEW_H, W), dtype=np.float64)
        for vy in range(VIEW_H):
            sy = cy + (vy - VIEW_H * 0.5 + y_off + drift_y) * step_y
            iy = int(np.clip(sy, 0, h - 1))
            xs = (np.arange(W) + x_off) * step_x
            ix = (np.floor(xs).astype(int) % w)
            frame[vy, :] = mask[iy, ix]
        return frame

    for mi, tint in enumerate((tl, ts, tc)):
        ma = sample(masks_a[mi], scroll_x, scroll_y, scale_a)
        mb = sample(masks_b[mi], scroll_x * 1.03, scroll_y * 0.97, scale_b)
        a = (ma * (1.0 - eased) + mb * eased) / 255.0
        out = out * (1 - a[:, :, None]) + np.array(tint)[None, None, :] * a[:, :, None]
    return out.astype(np.uint8)

def load_variant(layer, seed, storm=False):
    D = cloudgen.gen_density(layer, seed=seed, w=W, h=H, storm=storm)
    p = cloudgen._profile_for(layer, storm)
    return cloudgen.decompose(D, p)

def animate_layer(layer, scene_name="cloudy", wind=20, storm=False, out_path="preview_anim.gif"):
    scene = SCENES.get(scene_name, SCENES["cloudy"])
    wf = wind_multiplier(wind)
    speed = BASE_SPEED[layer] * wf ** layer_wind_exp(layer)
    hold, dur = MORPH_HOLD[layer], MORPH_DUR[layer]
    seed_a, seed_b = 100 * layer, 100 * layer + 1
    if storm:
        seed_a, seed_b = 900 + 100 * layer, 900 + 100 * layer + 1
    masks_a = load_variant(layer, seed_a, storm)
    masks_b = load_variant(layer, seed_b, storm)
    scale_a, scale_b = DEPTH_LO[layer], DEPTH_HI[layer]
    scroll_x = scroll_y = drift_y = 0.0
    drift_speed = 2.5 if layer == 2 else 1.2
    frames = []
    n = int(DURATION_S * FPS)
    t = 0.0
    dt = 1.0 / FPS
    cycle = hold + dur
    for _ in range(n):
        phase = t % cycle
        morph_t = max(0.0, min(1.0, (phase - hold) / dur)) if phase >= hold else 0.0
        scroll_x += speed * dt
        scroll_y += math.sin(t * 0.7 + layer) * 0.35
        drift_y += drift_speed * dt
        frames.append(Image.fromarray(
            composite_frame(masks_a, masks_b, morph_t, scroll_x, scroll_y, drift_y,
                            scale_a, scale_b, scene, storm)))
        t += dt
    frames[0].save(out_path, save_all=True, append_images=frames[1:],
                   duration=int(1000 / FPS), loop=0)
    print(f"wrote {out_path}")

def combined_scene(wind=20, out_path="preview_scene.gif"):
    layers = [0, 1, 2]
    wf = wind_multiplier(wind)
    n = int(DURATION_S * FPS)
    dt = 1.0 / FPS
    state = []
    for layer in layers:
        state.append({
            "masks_a": load_variant(layer, 100 * layer),
            "masks_b": load_variant(layer, 100 * layer + 1),
            "scroll_x": 0.0, "scroll_y": 0.0, "drift_y": 0.0,
            "scale_a": DEPTH_LO[layer], "scale_b": DEPTH_HI[layer],
            "speed": BASE_SPEED[layer] * wf ** layer_wind_exp(layer),
            "hold": MORPH_HOLD[layer], "dur": MORPH_DUR[layer], "t": 0.0,
        })
    scene = SCENES["cloudy"]
    frames = []
    for _ in range(n):
        acc = np.zeros((VIEW_H, W, 3), dtype=np.float64)
        for st in state:
            cycle = st["hold"] + st["dur"]
            phase = st["t"] % cycle
            morph_t = max(0.0, min(1.0, (phase - st["hold"]) / st["dur"])) if phase >= st["hold"] else 0.0
            st["scroll_x"] += st["speed"] * dt
            st["scroll_y"] += math.sin(st["t"] * 0.7) * 0.2
            st["drift_y"] += 1.5 * dt
            layer_img = composite_frame(st["masks_a"], st["masks_b"], morph_t,
                                        st["scroll_x"], st["scroll_y"], st["drift_y"],
                                        st["scale_a"], st["scale_b"], scene)
            acc = acc * 0.55 + layer_img.astype(np.float64) * 0.45
            st["t"] += dt
        frames.append(Image.fromarray(np.clip(acc, 0, 255).astype(np.uint8)))
    frames[0].save(out_path, save_all=True, append_images=frames[1:],
                   duration=int(1000 / FPS), loop=0)
    print(f"wrote {out_path}")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--layer", choices=["high", "mid", "low"])
    ap.add_argument("--kind", default="cloudy", choices=list(SCENES.keys()))
    ap.add_argument("--wind", type=float, default=20)
    ap.add_argument("--storm", action="store_true")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--out", default="preview_anim.gif")
    args = ap.parse_args()

    if args.all:
        for name in ("high", "mid", "low"):
            animate_layer(LAYER_MAP[name], scene_name=args.kind, wind=args.wind,
                          storm=(name != "high" and args.storm),
                          out_path=f"preview_{name}.gif")
        combined_scene(wind=args.wind, out_path="preview_scene.gif")
        return
    if not args.layer:
        ap.error("--layer required unless --all")
    animate_layer(LAYER_MAP[args.layer], scene_name=args.kind, wind=args.wind,
                  storm=args.storm, out_path=args.out)

if __name__ == "__main__":
    main()
