# Pre-baked Cloud Masks Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace on-device procedural cloud-strip generation with offline-generated, LZ4-compressed A8 masks loaded from SPIFFS, adding depth-breathing scale and vertical drift.

**Architecture:** A Python tool (`tools/cloudgen/`) generates 5 variants × 3 layers of 800×768 light/shadow/core A8 masks (periodic-X FBM + domain warp + transmittance lighting), packs them as `.clm` files into `spiffs_image/`, flashed into the existing 7 MB `storage` partition. On the device a new focused module `main/eva_cloud_assets.c` mounts SPIFFS, scans the pool and decompresses variants (with optional X-mirror and bilinear depth scale) into the existing `cloud_variant_t` PSRAM buffers. `cloud_bake_task` keeps its FSM; only its body changes from compute-bake to load. The old bake stays as `bake_strip_fallback_*`.

**Tech Stack:** Python 3 (numpy, Pillow, lz4), ESP-IDF v5.5.4, `espressif/lz4` registry component, SPIFFS.

**Spec:** `docs/2026-07-02-prebaked-cloud-masks-design.md`

**⚠ No git repo in this project.** Skip all commit steps; verification via host tests + build + hardware. At the very end take a tree snapshot per project convention (CLAUDE.md §8).

**Build env (from CLAUDE.md §3):**
```sh
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf"
source $IDF_PATH/export.sh
```

---

## `.clm` file format (single source of truth)

```
offset 0:  magic "CLM1"            (4 bytes)
offset 4:  u16 LE width            (800 or 400)
offset 6:  u16 LE height           (768 or 384)
offset 8:  u32 LE comp_size_light
offset 12: u32 LE comp_size_shadow
offset 16: u32 LE comp_size_core
offset 20: LZ4 block (raw stream, no size prefix): light mask, w*h bytes decompressed
then:      LZ4 block: shadow mask
then:      LZ4 block: core mask
```

File naming: `cloud_L<layer>_v<n>.clm`, layer 0=HIGH, 1=MID, 2=LOW, n from 0.

---

### Task 1: Python generator core — noise, profiles, mask decomposition

**Files:**
- Create: `tools/cloudgen/cloudgen.py`
- Create: `tools/cloudgen/test_cloudgen.py`

- [ ] **Step 1: Write the failing test**

`tools/cloudgen/test_cloudgen.py` (plain asserts, no pytest dependency — same spirit as `tools/test_cloud_bake_fsm.c`):

```python
#!/usr/bin/env python3
"""Host test: cloudgen noise + decomposition invariants.
Run: python3 tools/cloudgen/test_cloudgen.py
"""
import numpy as np
import cloudgen

W, H = 800, 768

def test_density_seamless_x():
    for layer in range(3):
        D = cloudgen.gen_density(layer, seed=42, w=W, h=H)
        # Periodic lattice → the field must wrap: compare the gradient across
        # the seam with the typical interior gradient.
        seam = np.abs(D[:, 0] - D[:, -1])
        interior = np.abs(np.diff(D, axis=1)).mean()
        assert seam.mean() < interior * 3.0, \
            f"layer {layer}: seam step {seam.mean():.4f} vs interior {interior:.4f}"

def test_density_range_and_coverage():
    for layer in range(3):
        D = cloudgen.gen_density(layer, seed=1, w=W, h=H)
        assert D.dtype == np.float64
        assert D.min() >= 0.0 and D.max() <= 1.0
        cov = (D > 0.05).mean()
        assert 0.10 < cov < 0.90, f"layer {layer}: coverage {cov:.2f} out of range"

def test_decompose_masks():
    D = cloudgen.gen_density(2, seed=7, w=W, h=H)
    light, shadow, core = cloudgen.decompose(D, cloudgen.PROFILES[2])
    for m in (light, shadow, core):
        assert m.shape == (H, W) and m.dtype == np.uint8
    # light + shadow must reconstruct total density (same invariant as the
    # device's add_l + add_s == total in blob_gaussian_triple).
    total = light.astype(int) + shadow.astype(int)
    ref = (D * 255.0 + 0.5).astype(int)
    assert np.abs(total - ref).max() <= 2
    # core must be sparser than the density itself
    assert (core > 0).mean() < (ref > 0).mean()

def test_variants_differ():
    a = cloudgen.gen_density(1, seed=10, w=W, h=H)
    b = cloudgen.gen_density(1, seed=11, w=W, h=H)
    assert np.abs(a - b).mean() > 0.01

if __name__ == "__main__":
    test_density_seamless_x()
    test_density_range_and_coverage()
    test_decompose_masks()
    test_variants_differ()
    print("OK")
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd tools/cloudgen && python3 test_cloudgen.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'cloudgen'`

- [ ] **Step 3: Write the generator core**

`tools/cloudgen/cloudgen.py`:

```python
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
        base_nx=6, base_ny=24, octaves=5, gain=0.55,
        stretch_x=4.0, warp=18.0,
        cover=0.62, soft=0.16, dens_gain=0.62,
        atten=2.2, core_lo=0.55, core_hi=0.95, core_gain=0.25),
    1: dict(  # altocumulus: broken clumps
        base_nx=10, base_ny=8, octaves=6, gain=0.50,
        stretch_x=1.4, warp=34.0,
        cover=0.52, soft=0.20, dens_gain=0.88,
        atten=3.0, core_lo=0.45, core_hi=0.85, core_gain=0.65),
    2: dict(  # cumulus: big volumetric masses, dense bellies
        base_nx=5, base_ny=4, octaves=6, gain=0.52,
        stretch_x=1.1, warp=48.0,
        cover=0.46, soft=0.24, dens_gain=1.00,
        atten=3.8, core_lo=0.40, core_hi=0.80, core_gain=1.00),
}

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

def gen_density(layer, seed, w=800, h=768):
    """Cloud density field in [0,1], seamlessly tiling in X."""
    p = PROFILES[layer]
    rng = np.random.default_rng(np.random.SeedSequence([seed, layer]))
    nx = max(2, int(round(p["base_nx"] / p["stretch_x"])))
    base = fbm(rng, w, h, nx, p["base_ny"], p["octaves"], p["gain"])
    wx = (fbm(rng, w, h, 4, 4, 3, 0.5) - 0.5) * 2.0 * p["warp"]
    wy = (fbm(rng, w, h, 4, 4, 3, 0.5) - 0.5) * 2.0 * p["warp"] * 0.5
    D = warp_y(warp_x(base, wx), wy)
    # Coverage threshold with a soft shoulder → torn cloud edges.
    D = np.clip((D - p["cover"]) / p["soft"], 0.0, 1.0)
    D = _smooth(D) * p["dens_gain"]
    # Feather top/bottom strip edges (same job as feather_strip_edges()).
    fade = 21
    env = np.ones(h)
    ramp = _smooth(np.linspace(0.0, 1.0, fade))
    env[:fade] = ramp
    env[-fade:] = ramp[::-1]
    return np.clip(D * env[:, None], 0.0, 1.0)

def decompose(D, p):
    """Split density into light/shadow/core A8 masks.

    Same semantics as blob_gaussian_triple() on the device: light+shadow
    partition the total alpha by how much cloud sits ABOVE each pixel
    (vertical transmittance), core marks the dense belly, biased downward.
    """
    T = np.cumsum(D, axis=0) - D            # optical depth above the pixel
    trans = np.exp(-p["atten"] * T / 32.0)  # /32: T is in row units
    light = D * trans
    shadow = D * (1.0 - trans)
    core_t = np.clip((D - p["core_lo"]) / (p["core_hi"] - p["core_lo"]), 0.0, 1.0)
    core = _smooth(core_t) * (0.35 + 0.65 * (1.0 - trans)) * p["core_gain"]

    def a8(m):
        return (np.clip(m, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8)

    # Force exact light+shadow == round(D*255) so device tint math sees the
    # same total alpha as the old bake.
    total = a8(D)
    l8 = a8(light)
    s8 = (total.astype(int) - l8.astype(int)).clip(0, 255).astype(np.uint8)
    return l8, s8, a8(core)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `cd tools/cloudgen && python3 test_cloudgen.py`
Expected: `OK`
(If numpy/Pillow/lz4 missing: `pip3 install numpy pillow lz4`.)

---

### Task 2: `.clm` writer, size report, contact-sheet preview

**Files:**
- Create: `tools/cloudgen/clm.py`
- Create: `tools/cloudgen/preview.py`
- Modify: `tools/cloudgen/test_cloudgen.py` (append roundtrip test)

- [ ] **Step 1: Append the failing roundtrip test to `test_cloudgen.py`**

```python
def test_clm_roundtrip(tmpdir="/tmp"):
    import os, lz4.block, clm
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
```

And add `test_clm_roundtrip()` to the `__main__` block.

- [ ] **Step 2: Run to verify it fails**

Run: `cd tools/cloudgen && python3 test_cloudgen.py`
Expected: FAIL with `ModuleNotFoundError: No module named 'clm'`

- [ ] **Step 3: Write `clm.py`**

```python
#!/usr/bin/env python3
"""CLM1 container writer: header + 3 raw LZ4 blocks (light/shadow/core)."""
import struct
import lz4.block

def write_clm(path, w, h, light, shadow, core):
    blocks = [
        lz4.block.compress(m.tobytes(), mode="high_compression",
                           store_size=False)
        for m in (light, shadow, core)
    ]
    with open(path, "wb") as f:
        f.write(b"CLM1")
        f.write(struct.pack("<HH", w, h))
        f.write(struct.pack("<III", *(len(b) for b in blocks)))
        for b in blocks:
            f.write(b)
    return sum(len(b) for b in blocks) + 20
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd tools/cloudgen && python3 test_cloudgen.py`
Expected: `OK`

- [ ] **Step 5: Write `preview.py` (contact sheet, no test — visual output)**

```python
#!/usr/bin/env python3
"""Render preview_contact_sheet.png: every variant × day/night/storm tints
composited over a sky gradient. Approve the look here BEFORE flashing."""
import sys
import numpy as np
from PIL import Image
import cloudgen

# (name, sky_top, sky_bottom, tint_light, tint_shadow, tint_core)
SCENES = [
    ("day",   (86, 148, 212), (168, 204, 236), (250, 250, 252), (150, 160, 178), (110, 120, 140)),
    ("night", (10, 14, 30),   (24, 30, 52),    (74, 80, 100),   (34, 38, 54),    (18, 20, 32)),
    ("storm", (60, 66, 80),   (110, 116, 128), (170, 172, 180), (62, 64, 74),    (28, 30, 38)),
]
VIEW_H = 480  # visible viewport rows inside the 768-row strip

def composite(D_masks, scene, w, h):
    _, top, bot, tl, ts, tc = (scene[0], *scene[1:])
    ys = np.linspace(0.0, 1.0, VIEW_H)[:, None, None]
    sky = (1 - ys) * np.array(top)[None, None, :] + ys * np.array(bot)[None, None, :]
    y0 = (h - VIEW_H) // 2
    out = sky.copy()
    for mask, tint in zip(D_masks, (tl, ts, tc)):
        a = mask[y0:y0 + VIEW_H].astype(np.float64)[:, :, None] / 255.0
        out = out * (1 - a) + np.array(tint)[None, None, :] * a
    return out.astype(np.uint8)

def main(variants=5, w=800, h=768, out="preview_contact_sheet.png"):
    thumb_w, thumb_h = 400, 240
    rows = []
    for layer in range(3):
        for v in range(variants):
            D = cloudgen.gen_density(layer, seed=100 * layer + v, w=w, h=h)
            masks = cloudgen.decompose(D, cloudgen.PROFILES[layer])
            row = [Image.fromarray(composite(masks, s, w, h)).resize((thumb_w, thumb_h))
                   for s in SCENES]
            rows.append((layer, v, row))
    sheet = Image.new("RGB", (thumb_w * len(SCENES), thumb_h * len(rows)))
    for i, (_, _, row) in enumerate(rows):
        for j, im in enumerate(row):
            sheet.paste(im, (j * thumb_w, i * thumb_h))
    sheet.save(out)
    print(f"wrote {out}: rows = L0v0..L0v{variants-1}, L1..., L2...; cols = day/night/storm")

if __name__ == "__main__":
    main(*(int(a) for a in sys.argv[1:2]))
```

- [ ] **Step 6: Smoke-run the preview**

Run: `cd tools/cloudgen && python3 preview.py && ls -la preview_contact_sheet.png`
Expected: file exists, no traceback. **Open it and show it to the user for visual approval before Task 3.** Iterate on `PROFILES` if rejected.

---

### Task 3: Generate the pool into `spiffs_image/` + size budget check

**Files:**
- Create: `tools/cloudgen/genpool.py`
- Create: `spiffs_image/` (output `.clm` files)

- [ ] **Step 1: Write `genpool.py`**

```python
#!/usr/bin/env python3
"""Generate the full .clm pool into ../../spiffs_image/ and print sizes."""
import os
import cloudgen, clm

VARIANTS = 5
W, H = 800, 768
OUT = os.path.join(os.path.dirname(__file__), "..", "..", "spiffs_image")

def main():
    os.makedirs(OUT, exist_ok=True)
    total = 0
    for layer in range(3):
        for v in range(VARIANTS):
            D = cloudgen.gen_density(layer, seed=100 * layer + v, w=W, h=H)
            masks = cloudgen.decompose(D, cloudgen.PROFILES[layer])
            path = os.path.join(OUT, f"cloud_L{layer}_v{v}.clm")
            size = clm.write_clm(path, W, H, *masks)
            total += size
            print(f"{os.path.basename(path)}: {size/1024:.0f} KB")
    print(f"TOTAL: {total/1024/1024:.2f} MB (budget: ~6 MB in the 7 MB partition)")
    if total > 6 * 1024 * 1024:
        print("OVER BUDGET — apply spec §3 contingency: fewer variants or 400x384 half-res")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it and check the budget**

Run: `cd tools/cloudgen && python3 genpool.py`
Expected: 15 files in `spiffs_image/`, `TOTAL` under 6 MB.

**If over budget** (spec §3 contingency, in order):
1. Set `VARIANTS = 4` and rerun.
2. Still over → regenerate with `W, H = 400, 384` (the device loader upscales; Task 5 handles any source size). Update `genpool.py` and rerun.

- [ ] **Step 3: Record the outcome**

Note the final variant count / resolution — Task 9 verification references it.

---

### Task 4: Device build plumbing — lz4 component, SPIFFS image, mount

**Files:**
- Modify: `main/idf_component.yml`
- Modify: `main/CMakeLists.txt`
- Create: `main/eva_cloud_assets.h`
- Create: `main/eva_cloud_assets.c` (mount + scan only in this task)

- [ ] **Step 1: Add the lz4 dependency**

In `main/idf_component.yml`, add to `dependencies:`:

```yaml
  espressif/lz4: "^1.9.4"
```

- [ ] **Step 2: Add sources + SPIFFS image to `main/CMakeLists.txt`**

Add `eva_cloud_assets.c` to `SRCS`, `spiffs` to `PRIV_REQUIRES`, and after `idf_component_register(...)`:

```cmake
spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)
```

- [ ] **Step 3: Write `main/eva_cloud_assets.h`**

```c
/* Pre-baked cloud mask assets (.clm files in SPIFFS).
 * See docs/2026-07-02-prebaked-cloud-masks-design.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVA_CLOUD_LAYERS       3
#define EVA_CLOUD_MAX_VARIANTS 8

/* Mount SPIFFS and scan /storage/cloud_L<l>_v<n>.clm.
 * Returns true if at least one layer has at least one variant. */
bool eva_cloud_assets_init(void);

/* Number of variants discovered for `layer` (0 if none / init failed). */
int eva_cloud_assets_count(int layer);

/* Decompress variant `idx` of `layer` into the caller's A8 buffers
 * (dst_w × dst_h each), applying optional X-mirror and a bilinear
 * depth scale (>1 = clouds closer / magnified around the centre).
 * Any source resolution in the file is resampled to dst. Returns false
 * on read/decode error (caller falls back to procedural bake). */
bool eva_cloud_assets_load(int layer, int idx,
                           uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core,
                           int dst_w, int dst_h,
                           bool mirror_x, float scale);

/* Duration of the last successful load, in microseconds (for cloudinfo). */
int64_t eva_cloud_assets_last_load_us(void);

#ifdef __cplusplus
}
#endif
```

- [ ] **Step 4: Write mount + scan in `main/eva_cloud_assets.c`**

```c
#include "eva_cloud_assets.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_timer.h"

static const char *TAG = "cloud_assets";
static int s_counts[EVA_CLOUD_LAYERS];
static bool s_mounted;
static int64_t s_last_load_us;

bool eva_cloud_assets_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/storage",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "spiffs mount failed (%s) — procedural fallback",
                 esp_err_to_name(err));
        return false;
    }
    s_mounted = true;
    bool any = false;
    for (int l = 0; l < EVA_CLOUD_LAYERS; ++l) {
        int n = 0;
        while (n < EVA_CLOUD_MAX_VARIANTS) {
            char path[48];
            snprintf(path, sizeof path, "/storage/cloud_L%d_v%d.clm", l, n);
            FILE *f = fopen(path, "rb");
            if (!f) break;
            fclose(f);
            ++n;
        }
        s_counts[l] = n;
        any = any || n > 0;
        ESP_LOGI(TAG, "layer %d: %d variants", l, n);
    }
    return any;
}

int eva_cloud_assets_count(int layer)
{
    if (layer < 0 || layer >= EVA_CLOUD_LAYERS) return 0;
    return s_counts[layer];
}

int64_t eva_cloud_assets_last_load_us(void)
{
    return s_last_load_us;
}
```

(`eva_cloud_assets_load` is a stub for now so the build links:)

```c
bool eva_cloud_assets_load(int layer, int idx,
                           uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core,
                           int dst_w, int dst_h,
                           bool mirror_x, float scale)
{
    (void)layer; (void)idx; (void)a8_light; (void)a8_shadow; (void)a8_core;
    (void)dst_w; (void)dst_h; (void)mirror_x; (void)scale;
    return false;
}
```

- [ ] **Step 5: Build**

Run:
```sh
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf" && source $IDF_PATH/export.sh
cd phase7_eva_weather && idf.py build
```
Expected: build succeeds; log mentions generating the `storage` SPIFFS image. (If the `espressif/lz4` component name fails dependency resolution, run `idf.py add-dependency "espressif/lz4"` and use the version it pins.)

---

### Task 5: The loader — header parse + LZ4 + mirror/scale resample

**Files:**
- Create: `main/eva_cloud_scale.h` (pure, host-testable)
- Create: `tools/test_clm_scale.c` (host test)
- Modify: `main/eva_cloud_assets.c` (real `eva_cloud_assets_load`)

- [ ] **Step 1: Write the failing host test `tools/test_clm_scale.c`**

```c
/* Host test: .clm header parse + bilinear scale/mirror resampler.
 * Build: cc -std=c11 -Wall -Wextra -lm -o /tmp/test_clm_scale tools/test_clm_scale.c && /tmp/test_clm_scale
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/eva_cloud_scale.h"

static void test_header_parse(void)
{
    uint8_t hdr[20] = { 'C', 'L', 'M', '1',
                        0x20, 0x03,   /* w = 800 */
                        0x00, 0x03,   /* h = 768 */
                        1, 0, 0, 0,  2, 0, 0, 0,  3, 0, 0, 0 };
    clm_header_t h;
    assert(clm_parse_header(hdr, sizeof hdr, &h));
    assert(h.w == 800 && h.h == 768);
    assert(h.comp_size[0] == 1 && h.comp_size[1] == 2 && h.comp_size[2] == 3);
    hdr[3] = '2';
    assert(!clm_parse_header(hdr, sizeof hdr, &h));
}

static void test_scale_identity(void)
{
    enum { W = 64, H = 48 };
    static uint8_t src[W * H], dst[W * H];
    for (int i = 0; i < W * H; ++i) src[i] = (uint8_t)(i * 7u);
    clm_scale_mask(src, W, H, dst, W, H, 1.0f, false);
    assert(memcmp(src, dst, sizeof src) == 0);
}

static void test_scale_mirror(void)
{
    enum { W = 8, H = 2 };
    uint8_t src[W * H] = { 0 }, dst[W * H];
    src[0] = 200;                       /* leftmost pixel of row 0 */
    clm_scale_mask(src, W, H, dst, W, H, 1.0f, true);
    assert(dst[W - 1] == 200 && dst[0] == 0);
}

static void test_scale_zoom_centred(void)
{
    enum { W = 100, H = 100 };
    static uint8_t src[W * H], dst[W * H];
    memset(src, 0, sizeof src);
    src[50 * W + 50] = 255;             /* centre pixel */
    clm_scale_mask(src, W, H, dst, W, H, 1.25f, false);
    /* Zoom is centred → the bright spot stays near the centre and spreads. */
    int best_x = -1, best_y = -1, best = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (dst[y * W + x] > best) { best = dst[y * W + x]; best_x = x; best_y = y; }
    assert(best > 100);
    assert(abs(best_x - 50) <= 2 && abs(best_y - 50) <= 2);
}

static void test_scale_x_wraps(void)
{
    enum { W = 100, H = 10 };
    static uint8_t src[W * H], dst[W * H];
    memset(src, 0, sizeof src);
    for (int y = 0; y < H; ++y) src[y * W + 0] = 255;   /* bright seam column */
    clm_scale_mask(src, W, H, dst, W, H, 0.8f, false);  /* zoom OUT pulls in wrapped content */
    /* No hard zero gap where the wrap happened: the seam column must still
     * exist somewhere with high intensity. */
    int maxv = 0;
    for (int i = 0; i < W * H; ++i) if (dst[i] > maxv) maxv = dst[i];
    assert(maxv > 150);
}

int main(void)
{
    test_header_parse();
    test_scale_identity();
    test_scale_mirror();
    test_scale_zoom_centred();
    test_scale_x_wraps();
    printf("OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -lm -o /tmp/test_clm_scale tools/test_clm_scale.c && /tmp/test_clm_scale`
Expected: FAIL — `eva_cloud_scale.h: No such file or directory`

- [ ] **Step 3: Write `main/eva_cloud_scale.h`**

Pure static-inline helpers, no ESP-IDF includes — compilable on host:

```c
/* .clm header parsing + A8 mask resampling (pure, host-testable).
 * Used by eva_cloud_assets.c on device and tools/test_clm_scale.c on host. */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint16_t w;
    uint16_t h;
    uint32_t comp_size[3];   /* light, shadow, core */
} clm_header_t;

#define CLM_HEADER_BYTES 20

static inline bool clm_parse_header(const uint8_t *buf, size_t len,
                                    clm_header_t *out)
{
    if (len < CLM_HEADER_BYTES || memcmp(buf, "CLM1", 4) != 0) return false;
    out->w = (uint16_t)(buf[4] | (buf[5] << 8));
    out->h = (uint16_t)(buf[6] | (buf[7] << 8));
    for (int i = 0; i < 3; ++i) {
        const uint8_t *p = &buf[8 + i * 4];
        out->comp_size[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return out->w > 0 && out->h > 0;
}

/* Bilinear resample of an A8 mask from src (sw×sh) into dst (dw×dh).
 * `scale` > 1 magnifies around the centre ("clouds closer"); X wraps
 * (strips tile seamlessly in X), Y clamps (strip edges are feathered).
 * `mirror_x` flips the output horizontally. scale==1 with equal dims and
 * no mirror is a straight copy. */
static inline void clm_scale_mask(const uint8_t *src, int sw, int sh,
                                  uint8_t *dst, int dw, int dh,
                                  float scale, bool mirror_x)
{
    if (sw == dw && sh == dh && !mirror_x && fabsf(scale - 1.0f) < 1e-3f) {
        memcpy(dst, src, (size_t)sw * (size_t)sh);
        return;
    }
    /* Total source step per dst pixel: resolution ratio / depth scale. */
    float step_x = ((float)sw / (float)dw) / scale;
    float step_y = ((float)sh / (float)dh) / scale;
    float cx = (float)sw * 0.5f, cy = (float)sh * 0.5f;
    for (int y = 0; y < dh; ++y) {
        float sy = cy + ((float)y - (float)dh * 0.5f) * step_y;
        int y0 = (int)floorf(sy);
        float fy = sy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) { y0 = 0; y1 = 0; fy = 0.0f; }
        if (y1 >= sh) { y1 = sh - 1; if (y0 > y1) y0 = y1; fy = 0.0f; }
        const uint8_t *r0 = &src[y0 * sw];
        const uint8_t *r1 = &src[y1 * sw];
        uint8_t *drow = &dst[y * dw];
        for (int x = 0; x < dw; ++x) {
            float sx = cx + ((float)x - (float)dw * 0.5f) * step_x;
            float sxf = floorf(sx);
            float fx = sx - sxf;
            int x0 = (int)sxf % sw;
            if (x0 < 0) x0 += sw;
            int x1 = x0 + 1;
            if (x1 >= sw) x1 = 0;
            float top = (float)r0[x0] + ((float)r0[x1] - (float)r0[x0]) * fx;
            float bot = (float)r1[x0] + ((float)r1[x1] - (float)r1[x0]) * fx;
            float v = top + (bot - top) * fy;
            drow[mirror_x ? (dw - 1 - x) : x] = (uint8_t)(v + 0.5f);
        }
    }
}
```

- [ ] **Step 4: Run the host test**

Run: `cc -std=c11 -Wall -Wextra -lm -o /tmp/test_clm_scale tools/test_clm_scale.c && /tmp/test_clm_scale`
Expected: `OK`

- [ ] **Step 5: Implement the real `eva_cloud_assets_load` in `eva_cloud_assets.c`**

Replace the stub. Add includes at the top of the file:

```c
#include "eva_cloud_scale.h"
#include "lz4.h"
#include "esp_heap_caps.h"
```

Then:

```c
/* Scratch buffer for one decompressed source mask (allocated lazily,
 * kept for the app lifetime — loads recur every 34–89 s). */
static uint8_t *s_scratch;
static size_t s_scratch_bytes;

static bool ensure_scratch(size_t bytes)
{
    if (s_scratch && s_scratch_bytes >= bytes) return true;
    if (s_scratch) heap_caps_free(s_scratch);
    s_scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch_bytes = s_scratch ? bytes : 0;
    return s_scratch != NULL;
}

bool eva_cloud_assets_load(int layer, int idx,
                           uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core,
                           int dst_w, int dst_h,
                           bool mirror_x, float scale)
{
    if (!s_mounted || layer < 0 || layer >= EVA_CLOUD_LAYERS ||
        idx < 0 || idx >= s_counts[layer]) {
        return false;
    }
    int64_t t0 = esp_timer_get_time();

    char path[48];
    snprintf(path, sizeof path, "/storage/cloud_L%d_v%d.clm", layer, idx);
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    bool ok = false;
    uint8_t *comp = NULL;
    uint8_t hdr_buf[CLM_HEADER_BYTES];
    clm_header_t hdr;
    if (fread(hdr_buf, 1, sizeof hdr_buf, f) != sizeof hdr_buf ||
        !clm_parse_header(hdr_buf, sizeof hdr_buf, &hdr)) {
        ESP_LOGW(TAG, "%s: bad header", path);
        goto out;
    }
    size_t src_bytes = (size_t)hdr.w * hdr.h;
    if (!ensure_scratch(src_bytes)) goto out;

    uint8_t *dsts[3] = { a8_light, a8_shadow, a8_core };
    for (int m = 0; m < 3; ++m) {
        uint32_t csize = hdr.comp_size[m];
        uint8_t *tmp = heap_caps_realloc(comp, csize,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!tmp) goto out;
        comp = tmp;
        if (fread(comp, 1, csize, f) != csize) {
            ESP_LOGW(TAG, "%s: truncated block %d", path, m);
            goto out;
        }
        int dec = LZ4_decompress_safe((const char *)comp, (char *)s_scratch,
                                      (int)csize, (int)src_bytes);
        if (dec != (int)src_bytes) {
            ESP_LOGW(TAG, "%s: lz4 block %d decode failed (%d)", path, m, dec);
            goto out;
        }
        clm_scale_mask(s_scratch, hdr.w, hdr.h,
                       dsts[m], dst_w, dst_h, scale, mirror_x);
    }
    s_last_load_us = esp_timer_get_time() - t0;
    ok = true;
out:
    if (comp) heap_caps_free(comp);
    fclose(f);
    return ok;
}
```

- [ ] **Step 6: Build**

Run: `cd phase7_eva_weather && idf.py build`
Expected: success. (`lz4.h` comes from the `espressif/lz4` component added in Task 4.)

---

### Task 6: Canvas integration — bake task loads instead of computing

**Files:**
- Modify: `main/eva_weather_canvas.c`

- [ ] **Step 1: Rename the procedural bake to fallback**

In `eva_weather_canvas.c` rename (plain search/replace, all call sites):
- `bake_strip_high` → `bake_strip_fallback_high`
- `bake_strip_mid` → `bake_strip_fallback_mid`
- `bake_strip_low` → `bake_strip_fallback_low`
- `bake_strip_for_layer` → `bake_strip_fallback_for_layer`

- [ ] **Step 2: Add asset-pool state and the load wrapper**

Add `#include "eva_cloud_assets.h"` near the other includes. Add to `cloud_strip_t` (after `bake_variant`):

```c
    /* Pre-baked pool bookkeeping (indices into the .clm files of this layer). */
    uint8_t pool_cur;
    uint8_t pool_prev;
    float depth_scale;    /* scale baked into the active variant (cloudinfo) */
    bool mirrored;        /* mirror baked into the active variant (cloudinfo) */
```

Add file-scope state near `s_bake_task`:

```c
static bool s_cloud_assets_ok;   /* SPIFFS pool discovered at init */
```

Add above `cloud_bake_task()`:

```c
/* Per-layer depth-breathing scale ranges (spec §5). */
static const float k_depth_lo[CLOUD_LAYER_COUNT] = {
    [CLOUD_LAYER_HIGH] = 0.95f, [CLOUD_LAYER_MID] = 0.90f, [CLOUD_LAYER_LOW] = 0.85f,
};
static const float k_depth_hi[CLOUD_LAYER_COUNT] = {
    [CLOUD_LAYER_HIGH] = 1.05f, [CLOUD_LAYER_MID] = 1.15f, [CLOUD_LAYER_LOW] = 1.25f,
};

/* Pick the next pool variant: never the active one, and avoid the previous
 * one when the pool is big enough (no A→B→A flicker). */
static int pick_pool_variant(const cloud_strip_t *strip, int count)
{
    if (count <= 1) return 0;
    for (int guard = 0; guard < 16; ++guard) {
        int idx = (int)(rndf(0.0f, (float)count - 0.001f));
        if (idx == strip->pool_cur) continue;
        if (count >= 3 && idx == strip->pool_prev) continue;
        return idx;
    }
    return (strip->pool_cur + 1) % count;
}

/* Load the next pre-baked variant into `v`; falls back to the procedural
 * bake when the asset pool is unavailable or the file is bad. */
static void load_or_bake_variant(int layer, cloud_strip_t *strip,
                                 cloud_variant_t *v)
{
    int count = s_cloud_assets_ok ? eva_cloud_assets_count(layer) : 0;
    if (count > 0) {
        int idx = pick_pool_variant(strip, count);
        bool mirror = rndf(0.0f, 1.0f) < 0.5f;
        float scale = rndf(k_depth_lo[layer], k_depth_hi[layer]);
        if (eva_cloud_assets_load(layer, idx,
                                  v->a8_light, v->a8_shadow, v->a8_core,
                                  CLOUD_STRIP_W, strip->strip_h,
                                  mirror, scale)) {
            strip->pool_prev = strip->pool_cur;
            strip->pool_cur = (uint8_t)idx;
            strip->depth_scale = scale;
            strip->mirrored = mirror;
            return;
        }
        ESP_LOGW(TAG, "cloud asset L%d v%d load failed — procedural fallback",
                 layer, idx);
    }
    bake_strip_fallback_for_layer(layer, v, strip->strip_h);
    strip->depth_scale = 1.0f;
    strip->mirrored = false;
}
```

- [ ] **Step 3: Use it in `cloud_bake_task` and `init_cloud_strips`**

In `cloud_bake_task()` replace:
```c
            bake_strip_for_layer(layer, &strip->variant[strip->bake_variant], strip->strip_h);
```
(now renamed `bake_strip_fallback_for_layer`) with:
```c
            load_or_bake_variant(layer, strip, &strip->variant[strip->bake_variant]);
```

In `init_cloud_strips()`:
- at the top, after `wait_cloud_bake_idle();`, add:
```c
    static bool assets_probed;
    if (!assets_probed) {
        assets_probed = true;
        s_cloud_assets_ok = eva_cloud_assets_init();
        if (!s_cloud_assets_ok) {
            ESP_LOGW(TAG, "no pre-baked cloud assets — using procedural bake");
        }
    }
```
- replace the per-variant `bake_strip_for_layer(i, v, strip->strip_h);` call with:
```c
            load_or_bake_variant(i, strip, v);
```
- in the per-strip reset block (where `active_variant`/`bake_state` are reset) add:
```c
        strip->pool_prev = strip->pool_cur;
```

- [ ] **Step 4: Build**

Run: `cd phase7_eva_weather && idf.py build`
Expected: success, no `-Wunused` for the old bake functions (they're still called on the fallback path).

---

### Task 7: Vertical drift (spec §5)

**Files:**
- Modify: `main/eva_weather_canvas.c`

- [ ] **Step 1: Add drift state**

Add to `cloud_strip_t` (next to `scroll_y_off`):

```c
    float drift_y;        /* slow vertical drift accumulator, px */
    float drift_speed;    /* px/s, direction re-rolled per variant swap */
```

- [ ] **Step 2: Re-roll drift on variant swap**

In the morph-complete block (search for `strip->active_variant = hidden;` around line 3465), after `strip->bake_state = BAKE_REQUESTED;` add:

```c
            /* New variant, new gentle vertical drift (spec §5): ±8–20 px
             * over a 34–89 s lifecycle ≈ 0.10–0.35 px/s. */
            strip->drift_speed = rndf(0.10f, 0.35f) *
                                 (rndf(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f);
```

- [ ] **Step 3: Integrate drift into the scroll update**

In the scroll/bob update loop (around line 3186, where `scroll_y_off` is written), change to:

```c
        s_strip[i].drift_y += dt * s_strip[i].drift_speed;
        /* Bounce softly inside the overflow margin so clouds never expose
         * the strip edge (±CLOUD_STRIP_OVERFLOW_Y/2 with feather margin). */
        float drift_lim = (float)CLOUD_STRIP_OVERFLOW_Y * 0.5f;
        if (s_strip[i].drift_y > drift_lim) {
            s_strip[i].drift_y = drift_lim;
            s_strip[i].drift_speed = -fabsf(s_strip[i].drift_speed);
        } else if (s_strip[i].drift_y < -drift_lim) {
            s_strip[i].drift_y = -drift_lim;
            s_strip[i].drift_speed = fabsf(s_strip[i].drift_speed);
        }
        s_strip[i].scroll_y_off = s_strip[i].drift_y +
            bob_amp * sinf(t_now * bob_freq + (float)i * EVA_PHI + s_strip[i].scroll_x * 0.002f);
```

(keep the existing `bob_amp`/`bob_freq` lines above it unchanged).

- [ ] **Step 4: Build**

Run: `cd phase7_eva_weather && idf.py build`
Expected: success.

---

### Task 8: `cloudinfo` CDC command

**Files:**
- Modify: `main/eva_weather_canvas.h` (declare info API)
- Modify: `main/eva_weather_canvas.c` (implement)
- Modify: `main/main.c` (dispatch + help text)

- [ ] **Step 1: Declare in `eva_weather_canvas.h`**

```c
/* Fill `buf` with per-layer cloud pool status (for the CDC cloudinfo cmd). */
void eva_weather_canvas_cloud_info(char *buf, size_t buf_len);
```

- [ ] **Step 2: Implement in `eva_weather_canvas.c`**

```c
void eva_weather_canvas_cloud_info(char *buf, size_t buf_len)
{
    static const char *names[CLOUD_LAYER_COUNT] = { "HIGH", "MID", "LOW" };
    size_t off = 0;
    off += (size_t)snprintf(buf + off, buf_len - off,
                            "cloud assets: %s, last load %lld us\r\n",
                            s_cloud_assets_ok ? "spiffs pool" : "procedural fallback",
                            (long long)eva_cloud_assets_last_load_us());
    for (int i = 0; i < CLOUD_LAYER_COUNT && off < buf_len; ++i) {
        const cloud_strip_t *s = &s_strip[i];
        off += (size_t)snprintf(buf + off, buf_len - off,
                                "%s: pool %d/%d prev %d scale %.2f%s%s\r\n",
                                names[i], s->pool_cur,
                                eva_cloud_assets_count(i), s->pool_prev,
                                (double)s->depth_scale,
                                s->mirrored ? " mirrored" : "",
                                s->morphing ? " (morphing)" : "");
    }
}
```

- [ ] **Step 3: Dispatch in `main.c`**

In `cdc_handle_command()` (line ~1861), add to the strcmp chain:

```c
    } else if (strcmp(cmd, "cloudinfo") == 0) {
        char info[320];
        eva_weather_canvas_cloud_info(info, sizeof info);
        cdc_send(info);
```

And add one line to the `help` output: `cloudinfo — cloud asset pool status`.

- [ ] **Step 4: Build**

Run: `cd phase7_eva_weather && idf.py build`
Expected: success.

---

### Task 9: Flash + hardware verification (spec §7)

**Files:** none (verification)

- [ ] **Step 1: Flash**

```sh
cd phase7_eva_weather/build
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* -b 460800 \
    --before usb_reset --after hard_reset write_flash @flash_args
```
(If the port is held by an orphan python: `lsof /dev/cu.usbmodem*` → kill it. CLAUDE.md §3.)

- [ ] **Step 2: Check boot logs**

Expected in the monitor/log: `cloud_assets: layer 0: 5 variants` (×3 layers, count per Task 3 outcome), **no** `procedural fallback` warning.

- [ ] **Step 3: `cloudinfo` + `screenshot` over CDC**

Run `cloudinfo` → pool counts and a plausible `last load` (expect 30–60 ms = 30000–60000 us). Take `screenshot` (tools/eva-screenshot.py) at a cloudy kind (`weatherdebug cloudy`) — clouds must look at least as good as the contact sheet approved in Task 2, tints/day-night behaviour unchanged.

- [ ] **Step 4: Success criteria (spec §7)**

1. Watch the FPS chip across ≥2 morph cycles (up to ~3 min): the 30–80 ms morph dips must be gone.
2. Cloud-scene FPS ≥ ~20 Hz baseline.
3. Morph crossfade shows no pop at variant swap; depth breathing visible across swaps (compare consecutive screenshots).

- [ ] **Step 5: Fallback path check**

Verify the fallback once by erasing the storage partition. Look up the real offset/size of `storage` in `build/partition_table/partition-table.csv` first, then:

```sh
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* erase_region <storage_offset> <storage_size>
```

Reboot → expect the `procedural fallback` warning in the log and clouds still rendering (old Gaussian look). Then re-flash everything (`write_flash @flash_args` from Step 1) to restore the SPIFFS image.

---

### Task 10: Docs + snapshot

**Files:**
- Modify: `../CLAUDE.md` (42_EVE_FW/CLAUDE.md)
- Create: snapshot copy

- [ ] **Step 1: Update CLAUDE.md**

- §7 known limitations: replace the "Cloud bake morph … 30-80 ms dip … ЛІНІЯ J not done" bullet with a note that clouds are now pre-baked `.clm` assets from SPIFFS (`tools/cloudgen/`), morph load ≈30–60 ms in the background task, procedural bake kept as fallback.
- §6 recipes: add a bullet — "Cloud masks are generated OFFLINE by `tools/cloudgen/genpool.py` into `spiffs_image/`; to change the cloud look edit `PROFILES` in `cloudgen.py`, regenerate, review `preview.py` contact sheet, rebuild (SPIFFS image flashes with the app). Depth-breathing scale ranges live in `k_depth_lo/hi` in `eva_weather_canvas.c`."

- [ ] **Step 2: Take a snapshot (project convention, CLAUDE.md §8)**

```sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW
rsync -a --exclude build phase7_eva_weather/ \
    phase7_eva_weather-snapshot-$(date +%Y-%m-%d)-prebaked-clouds/
```
Add the snapshot line to CLAUDE.md §8 ladder.

- [ ] **Step 3: Note the eva_wthr port**

Per memory rule, `eva_wthr` (GitHub copy) must get the same feature after hardware verification — flag it to the user as the follow-up (out of scope here, spec §8).
