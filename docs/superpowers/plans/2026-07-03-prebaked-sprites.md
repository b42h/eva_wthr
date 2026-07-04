# Pre-baked Scene Sprites Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace hand-drawn lightning, sun god rays, moon disc and glass rain drops with offline-generated sprites shipped in a typed CLP2 pack — visual upgrade, FPS-neutral by design.

**Architecture:** The CLP1 pack gains a `type` byte per TOC entry (CLP2). The `.clm` 3-plane container is reused with per-type plane semantics. Sprites decompress once at init into resident PSRAM planes served by `eva_cloud_assets_sprite()`. Each element keeps its existing state machinery (strike envelope, drop FSM, bg repaint) — only the drawing switches to sprite blits, with the old drawing kept as the missing-asset fallback.

**Tech Stack:** Python (numpy/Pillow/lz4) generators, ESP-IDF v5.5.4, mmap + LZ4, CPU A8 blits.

**Spec:** `docs/superpowers/specs/2026-07-03-prebaked-sprites-design.md`

**⚠ No git repo** — no commits; verify per task; snapshot at the end.
**Build env:** `export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf" && source $IDF_PATH/export.sh`
**Flash (from `build/`):** `python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* -b 460800 --before usb_reset --after hard_reset write_flash @flash_args` (falls to ROM port `usbmodem101` with `--before default_reset` if the CDC port vanished).
**Device session helpers:** CDC `weatherpin on`, `weatherraw 100 100 100 100 0 thunder 80`, `lightning`, `status` (shows backlight), scratchpad `cdc_cmd.py` / `burst_shots.py`.
**Phase gates:** hardware verification closes each phase (Tasks 5, 6, 7) before the next starts.

---

## Execution status (updated 2026-07-04)

All 8 tasks implemented, host tests + `idf.py build` pass, firmware + CLP2
pack flashed to hardware. CDC smoke: `cloudinfo` loads pack, `lightning`
queues strikes in thunderstorm, `perf` reports `li`/`gl` buckets.

| Task | Status | Notes |
|------|--------|-------|
| 1. CLP2 typed TOC | ✅ **done** | `eva_clp_toc.h` carries a `type` byte; `test_clp_toc.c` + genpool updated; pack magic `CLP2`. Reviews passed, quality nits fixed. |
| 2. Sprite generators + gates | ✅ **done** | `tools/cloudgen/sprites.py` (bolt/ray/moon/drop/trail) + 4 host gates in `test_cloudgen.py`. `gen_bolt` rewritten to render depth-2/3 branches (were silently dropped); shading/branch constants named. |
| 3. genpool packs sprites + preview | ✅ **done** | 31 sprite entries packed into `assets/clouds.bin` (grand total **6.34 MB**, budget 6.9 MB, no contingency needed). `preview.py --sprites` → `sprites_sheet.png`. Type constants + array-derived dims (no size drift). |
| 4. Device sprite loader (CLP2) | ✅ **done** | `eva_sprite_t` + `eva_cloud_assets_sprite()`; non-cloud entries decompressed once into resident PSRAM. `clm_decompress_plane()` helper factored out of cloud + sprite paths; stale SPIFFS comments/log fixed. Builds clean. |
| 5. Lightning sprites | ✅ **done** | Bolt sprite blit (glow then core) in `composite_lightning_on_render()`; polyline fallback; variant + mirror randomisation. Flashed + CDC `lightning` verified (`li` bucket active during strikes). |
| 6. Sun rays + moon phases | ✅ **done** | Ray sprite blit (4 phases) at top of `draw_sun_fib_light()`; 8-phase moon sprite in `draw_sun_or_moon()` bg repaint with waning mirror + luminance blit. Fibonacci ring fallback retained. |
| 7. Glass rain drops + trails | ✅ **done** | Drop sprites (3 size × 3 shape), wet-glass A8 accumulation buffer + trail stamp + decay/composite in glass overlay pass. FSM untouched; primitive bead fallback kept. |
| 8. Docs + snapshot | ✅ **done** | CLAUDE.md §6/§8 updated; snapshot `phase7_eva_weather-snapshot-2026-07-03-sprites`. **eva_wthr port still pending** (see deferred). |

### What remains to do (detailed)

**Package complete.** Optional follow-ups (not blocking sign-off):

1. **Visual QA on panel** — judge sprite art quality at real scale (bolts,
   moon craters, drop lenses). If unsatisfactory: tune `sprites.py` +
   `genpool.py`, re-flash `assets/clouds.bin` only.
2. **Lightning perf** — strike frames may spike `li` to ~11 ms (observed
   during CDC test); tune `BOLT_GLOW_SCALE_*` / bolt anchor if FPS dips
   bother you in practice.
3. **eva_wthr port** — all 2026-07-02/03 packages + this sprite package.

### Known deferred / open items (not blocking, recorded for later)
- **Sprite art quality**: on the Task 3 preview the bolts read thin with weak
  branching, the moon is dark with faint craters, drops are tiny. The user
  chose to judge these at real-panel scale during the HW gates rather than
  pre-tune. If unsatisfactory on hardware, iterate `sprites.py` `PROFILES`-
  equivalent params (branch density/decay, crater darkness `0.28`, drop
  dome/rim) and re-run `genpool.py` — no device rebuild needed for art-only
  changes, just re-flash `assets/clouds.bin`.
- **FPS is explicitly NOT improved by this package** (spec §1): storm ~15 Hz,
  light scenes ~19–20 Hz stay as-is. The real FPS lever ("D": pipeline
  overlap / double-buffered background rotation → ~22–27 Hz) and fog-sprite
  work remain separate future specs.
- **eva_wthr port** of all three 2026-07-02/03 packages (prebaked clouds,
  visual quality, FPS stabilization) + this sprite package is still pending,
  to be done after the whole sprite package is signed off on hardware.

---

## Sprite type map (single source of truth)

| type | name  | subtype (`layer` field)  | variants | size      | planes                          |
|------|-------|--------------------------|----------|-----------|---------------------------------|
| 0    | cloud | cloud layer 0–2          | 4/3      | 800×768   | light / shadow / core (as today)|
| 1    | bolt  | 0                        | 8        | 360×560   | core alpha / glow alpha / —     |
| 2    | ray   | phase 0–3                | 1        | 480×480   | alpha / — / —                   |
| 3    | moon  | phase 0–7                | 1        | 120×120   | alpha / luminance / —           |
| 4    | drop  | size class 0–2           | 3        | 24–48 px  | alpha / specular / —            |
| 5    | trail | 0                        | 2        | 16×160    | alpha / — / —                   |

Empty planes are stored as a zero-length LZ4 block (`comp_size == 0`).

---

### Task 1: CLP2 typed TOC (host TDD)

**Files:**
- Modify: `main/eva_clp_toc.h`
- Modify: `tools/test_clp_toc.c`
- Modify: `tools/cloudgen/genpool.py` (`write_pack`)

- [ ] **Step 1: Extend the host test for typed entries**

In `tools/test_clp_toc.c`, change `build_blob` to write magic `"CLP2"` and a
type byte instead of the reserved zero, and add assertions:

```c
static size_t build_blob(uint16_t count)
{
    memcpy(blob, "CLP2", 4);
    blob[4] = (uint8_t)count;
    blob[5] = (uint8_t)(count >> 8);
    size_t off = 6;
    for (int i = 0; i < count; ++i) {
        blob[off + 0] = (uint8_t)(i % 3);      /* layer / subtype */
        blob[off + 1] = (uint8_t)(i % 2);      /* pool  */
        blob[off + 2] = (uint8_t)i;            /* variant */
        blob[off + 3] = (uint8_t)(i % 6);      /* type */
        put32(&blob[off + 4], 6u + (uint32_t)count * 12u + i * 10u);
        put32(&blob[off + 8], 10u);
        off += 12;
    }
    return off + count * 10u;
}
```

In `test_parse_ok` add:

```c
    const eva_clp_entry_t *e2 = eva_clp_entry(&toc, 3);
    assert(e2 && e2->type == 3);
```

And a new case:

```c
static void test_old_magic_rejected(void)
{
    size_t total = build_blob(1);
    memcpy(blob, "CLP1", 4);
    eva_clp_toc_t toc;
    assert(!eva_clp_parse(blob, total, &toc));
}
```

(call it from `main`).

- [ ] **Step 2: Run — expect FAIL** (`type` member missing / magic mismatch)

Run: `cc -std=c11 -Wall -Wextra -o /tmp/test_clp_toc tools/test_clp_toc.c && /tmp/test_clp_toc`

- [ ] **Step 3: Update `main/eva_clp_toc.h`**

- Header comment: `"CLP2 ... {u8 layer, u8 pool, u8 variant, u8 type, u32 offset, u32 size}"`.
- `eva_clp_entry_t` gains `uint8_t type;` after `variant`.
- Parse: `memcmp(pack, "CLP2", 4)`; fill `e->type = p[3];`.
- `EVA_CLP_MAX_ENTRIES` 64 → 96 (21 clouds + ~25 sprites + headroom).
- Add the type constants:

```c
#define EVA_CLP_TYPE_CLOUD 0
#define EVA_CLP_TYPE_BOLT  1
#define EVA_CLP_TYPE_RAY   2
#define EVA_CLP_TYPE_MOON  3
#define EVA_CLP_TYPE_DROP  4
#define EVA_CLP_TYPE_TRAIL 5
```

- [ ] **Step 4: Run — expect `OK`**

- [ ] **Step 5: Teach `write_pack` the type byte**

In `tools/cloudgen/genpool.py` change `write_pack` entries to
`(type, layer, pool, variant, clm_path)` tuples, magic `b"CLP2"`, and the
struct line to `struct.pack("<BBBBII", layer, pool, variant, etype, off, len(blob))`.
Update `main()`'s two cloud loops to append
`entries.append((0, layer, 0, v, path))` / `(0, layer, 1, v, path)`.

- [ ] **Step 6: Regenerate and sanity-check**

Run: `tools/cloudgen/.venv/bin/python tools/cloudgen/genpool.py`
Expected: pack written, `CLP2` magic (`head -c4 assets/clouds.bin` → `CLP2`),
gates OK. (Device still runs CLP1 firmware — it will log
`no CLP1 pack — procedural fallback` until Task 4 flashes; that's the
designed fallback and is fine mid-plan.)

---

### Task 2: Sprite generators + quality gates (host TDD)

**Files:**
- Create: `tools/cloudgen/sprites.py`
- Modify: `tools/cloudgen/clm.py` (empty-plane support)
- Modify: `tools/cloudgen/test_cloudgen.py`

- [ ] **Step 1: Failing tests first — append to `test_cloudgen.py`**

```python
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
    lit_prev = -1
    for ph in range(8):
        alpha, lum = sprites.gen_moon(phase=ph)
        assert alpha.shape == (120, 120) and lum.shape == (120, 120)
        lit = int((lum > 60).sum())
        assert lit > lit_prev, f"phase {ph}: lit area must grow"
        lit_prev = lit
    # Full moon shows crater texture: luminance variance inside the disc.
    inside = alpha > 128
    assert lum[inside].std() > 8, "craters missing"

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
```

Register all four in `run_all()`.

- [ ] **Step 2: Run — expect FAIL (`No module named 'sprites'`)**

Run: `cd tools/cloudgen && .venv/bin/python test_cloudgen.py`

- [ ] **Step 3: Write `tools/cloudgen/sprites.py`**

```python
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

def gen_bolt(seed, w=360, h=560):
    """Branched bolt via a biased random walker: a main channel descends the
    full height with lateral jitter; side branches split off with decaying
    intensity and die early. Core = sharp channel, glow = blurred halo."""
    rng = np.random.default_rng(seed)
    core = np.zeros((h, w))

    def walk(x, y, intensity, max_len, can_branch, depth):
        pts = []
        while y < h - 2 and len(pts) < max_len and intensity > 0.08:
            pts.append((int(x), int(y), intensity))
            y += rng.uniform(1.2, 3.2)
            x += rng.normal(0.0, 2.6) + rng.uniform(-0.6, 0.6)
            x = float(np.clip(x, 4, w - 5))
            if can_branch and depth < 3 and rng.random() < 0.045:
                bx = x + rng.normal(0.0, 2.0)
                blen = int(rng.uniform(18, 70) * (0.7 ** depth))
                walk(bx, y, intensity * rng.uniform(0.35, 0.6),
                     blen, True, depth + 1)
            if depth > 0:
                intensity *= 0.995   # branches fade as they run
        for (px, py, pi) in pts:
            core[py, px] = max(core[py, px], pi)
            if pi > 0.5:             # main channel is 2 px wide
                core[py, min(px + 1, w - 1)] = max(core[py, min(px + 1, w - 1)], pi * 0.8)

    walk(rng.uniform(w * 0.3, w * 0.7), 0.0, 1.0, 10 ** 6, True, 0)
    # Guarantee row continuity of the main channel: fill any skipped rows
    # by interpolating between neighbours (walker can jump 3 rows).
    ys = np.where(core.max(axis=1) > 0.5)[0]
    for y0, y1 in zip(ys[:-1], ys[1:]):
        if y1 - y0 > 1:
            x0 = core[y0].argmax(); x1 = core[y1].argmax()
            for yy in range(y0 + 1, y1):
                t = (yy - y0) / (y1 - y0)
                core[yy, int(x0 + (x1 - x0) * t)] = 1.0
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
        lum -= 0.28 * np.exp(-d2 / (cr * cr)) * rng.uniform(0.4, 1.0)
    # Terminator: illuminated fraction grows with phase; the shadow edge is
    # an ellipse sweeping across (simple but reads correctly at 120 px).
    frac = (phase + 1) / 8.0                    # 0.125 .. 1.0
    term = -1.0 + 2.0 * frac                    # -0.75 .. +1.0
    lit = np.clip((term - dx) * 6.0 + 0.5, 0.0, 1.0) if frac < 1.0 else 1.0
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
```

- [ ] **Step 4: Empty-plane support in `clm.py`**

`write_clm` already writes whatever bytes are given — add a guard so an
empty plane writes a zero-length block instead of calling lz4 on `b""`:

```python
    blocks = []
    for m in (light, shadow, core):
        raw = m.tobytes() if m is not None and m.size else b""
        blocks.append(lz4.block.compress(raw, mode="high_compression",
                                         store_size=False) if raw else b"")
```

(Pass `None` or an empty array for unused planes.)

- [ ] **Step 5: Run tests — expect `OK`**

Run: `cd tools/cloudgen && .venv/bin/python test_cloudgen.py`

---

### Task 3: genpool packs sprites + preview galleries

**Files:**
- Modify: `tools/cloudgen/genpool.py`
- Modify: `tools/cloudgen/preview.py`

- [ ] **Step 1: Generate sprites into the pack**

In `genpool.py` after the storm-cloud loop add (types per the sprite map):

```python
    import sprites as spr
    def add_sprite(etype, subtype, variant, planes, w, h, name):
        path = os.path.join(OUT, name)
        a, b, c3 = (list(planes) + [None, None])[:3]
        size = clm.write_clm(path, w, h, a, b, c3)
        entries.append((etype, subtype, 0, variant, path))
        print(f"{name}: {size/1024:.0f} KB")
        return size

    total_sprites = 0
    for v in range(8):
        core, glow = spr.gen_bolt(seed=40 + v)
        total_sprites += add_sprite(1, 0, v, (core, glow), 360, 560,
                                    f"bolt_v{v}.clm")
    for ph in range(4):
        total_sprites += add_sprite(2, ph, 0, (spr.gen_rays(ph),), 480, 480,
                                    f"ray_p{ph}.clm")
    for ph in range(8):
        a, lum = spr.gen_moon(ph)
        total_sprites += add_sprite(3, ph, 0, (a, lum), 120, 120,
                                    f"moon_p{ph}.clm")
    for size_c in range(3):
        for shape in range(3):
            a, s = spr.gen_drop(size_c, shape)
            d = 24 + size_c * 12
            total_sprites += add_sprite(4, size_c, shape, (a, s), d, d,
                                        f"drop_s{size_c}_{shape}.clm")
    for v in range(2):
        total_sprites += add_sprite(5, 0, v, (spr.gen_trail(v),), 16, 160,
                                    f"trail_v{v}.clm")
    print(f"SPRITES: {total_sprites/1024:.0f} KB")
```

Note: `write_clm(path, w, h, ...)` — drop/trail sprites are square/rect with
their own w/h; the `.clm` header already carries per-file dimensions.

- [ ] **Step 2: Run genpool; check budget**

Run: `.venv/bin/python genpool.py`
Expected: pack total ≤ 6.9 MB (clouds 6.16 + sprites ≈ 0.5). If over:
regenerate bolts at 180×280 (`gen_bolt(seed, w=180, h=280)`), the loader
upscales nothing — the blit just draws a smaller bolt, acceptable.

- [ ] **Step 3: Preview galleries**

Add to `preview.py` a `--sprites` mode rendering `sprites_sheet.png`:
bolt gallery (each variant composited white-on-storm-grey background with
its glow), moon 8-phase strip, drop sheet (each size/shape over a blue-grey
gradient with specular visible), 4 ray phases. Implementation sketch
(complete, follows the existing Pillow patterns in the file):

```python
def render_sprites_sheet(out="sprites_sheet.png"):
    import sprites as spr
    tiles = []
    storm = np.full((560, 360, 3), (70, 74, 86), np.uint8)
    for v in range(8):
        core, glow = spr.gen_bolt(seed=40 + v)
        img = storm.astype(np.float64)
        for plane, tint in ((glow, (188, 210, 255)), (core, (248, 252, 255))):
            a = plane.astype(np.float64)[:, :, None] / 255.0
            img = img * (1 - a) + np.array(tint) * a
        tiles.append(("bolt %d" % v, img.astype(np.uint8)))
    for ph in range(8):
        a, lum = spr.gen_moon(ph)
        night = np.full((120, 120, 3), (12, 16, 30), np.uint8).astype(np.float64)
        al = a.astype(np.float64)[:, :, None] / 255.0
        col = (lum.astype(np.float64)[:, :, None] / 255.0) * np.array((228, 228, 218))
        tiles.append(("moon %d" % ph, (night * (1 - al) + col * al).astype(np.uint8)))
    for ph in range(4):
        a = spr.gen_rays(ph).astype(np.float64)[:, :, None] / 255.0
        day = np.full((480, 480, 3), (120, 178, 224), np.uint8).astype(np.float64)
        tiles.append(("rays %d" % ph,
                      (day * (1 - a) + np.array((255, 246, 214)) * a).astype(np.uint8)))
    for size_c in range(3):
        for shape in range(3):
            al, sp = spr.gen_drop(size_c, shape)
            bg = np.full(al.shape + (3,), (96, 108, 122), np.uint8).astype(np.float64)
            aa = al.astype(np.float64)[:, :, None] / 255.0
            img = bg * (1 - aa * 0.5) + np.array((210, 220, 232)) * aa * 0.5
            ss = sp.astype(np.float64)[:, :, None] / 255.0
            img = img * (1 - ss) + 255.0 * ss
            tiles.append(("drop %d/%d" % (size_c, shape), img.astype(np.uint8)))
    # Assemble: paste tiles onto one sheet with labels, exactly like
    # render_sheet() does for cloud thumbs (same Image.new + paste loop,
    # tile sizes vary so lay out left-to-right, wrap at 1200 px).
```

- [ ] **Step 4: Generate + view**

Run: `.venv/bin/python preview.py --sprites` → open `sprites_sheet.png`,
**show it to the user for approval before flashing anything.**

---

### Task 4: Device sprite loader (CLP2)

**Files:**
- Modify: `main/eva_cloud_assets.h`
- Modify: `main/eva_cloud_assets.c`

- [ ] **Step 1: API in the header**

```c
typedef struct {
    const uint8_t *plane[3];   /* NULL when the pack stored an empty plane */
    uint16_t w;
    uint16_t h;
} eva_sprite_t;

/* Resident sprite lookup (decompressed once at init). type/subtype/variant
 * per the CLP2 sprite map. Returns false if the pack lacks that sprite —
 * caller falls back to its procedural drawing. */
bool eva_cloud_assets_sprite(int type, int subtype, int variant,
                             eva_sprite_t *out);
```

- [ ] **Step 2: Loader implementation**

In `eva_cloud_assets.c`:
- keep the cloud index exactly as-is (entries with `type == EVA_CLP_TYPE_CLOUD`);
- add a sprite table `static struct { eva_sprite_t s; uint8_t type, sub, var; } s_sprites[64]; static int s_sprite_count;`
- in `eva_cloud_assets_init()`, after the cloud scan, iterate non-cloud
  entries: parse the `.clm` header from the mmap pointer, for each plane
  with `comp_size > 0` allocate `w*h` PSRAM and `LZ4_decompress_safe`
  directly from flash; `comp_size == 0` → `plane[i] = NULL`. Log a summary
  (`"sprites: %d resident, %u KB"`).
- `eva_cloud_assets_sprite()` = linear search of `s_sprites` (≤64 entries,
  called at init/strike frequency — no index needed).

- [ ] **Step 3: Build + flash + smoke check**

`idf.py build`, flash, boot log shows the sprite summary and clouds load as
before; `cloudinfo` unchanged; free PSRAM logged ≥ 8 MB
(`heap_caps_get_free_size(MALLOC_CAP_SPIRAM)` — add a one-line log at the
end of init).

---

### Task 5: Lightning sprites (phase gate: hardware)

**Files:**
- Modify: `main/eva_weather_canvas.c` (`generate_lightning_bolt` ~3771, `composite_lightning_on_render` ~4122)

- [ ] **Step 1: Sprite placement state**

Add near the lightning statics:

```c
static eva_sprite_t s_bolt_sprite;      /* active strike's sprite */
static bool s_bolt_sprite_ok;
static int s_bolt_x, s_bolt_y;          /* top-left blit anchor */
static bool s_bolt_mirror;
```

- [ ] **Step 2: Pick sprite in `generate_lightning_bolt()`**

At the top, try the sprite path; keep the whole existing polyline generator
as the `else` (fallback):

```c
    int variant = (int)rndf(0.0f, 7.999f);
    s_bolt_sprite_ok = eva_cloud_assets_sprite(EVA_CLP_TYPE_BOLT, 0, variant,
                                               &s_bolt_sprite);
    if (s_bolt_sprite_ok) {
        s_bolt_mirror = rndf(0.0f, 1.0f) < 0.5f;
        s_bolt_x = (int)rndf(0.0f,
                     (float)(EVA_WEATHER_RENDER_W - s_bolt_sprite.w));
        s_bolt_y = (int)rndf(-40.0f, 20.0f);   /* channel starts in cloud */
        s_lightning_flash_x = s_bolt_x + s_bolt_sprite.w / 2;
        s_lightning_flash_y = s_bolt_y + s_bolt_sprite.h / 3;
        /* sheet_only still rolled by the existing code above. */
        return;
    }
```

- [ ] **Step 3: Blit in `composite_lightning_on_render()`**

Add a small helper above it:

```c
/* Additive-ish A8 sprite blit for lightning planes: dst is brightened
 * toward `tint` by plane alpha × `scale`/255. Rows outside the viewport
 * are clipped; mirror flips X. */
static void blit_bolt_plane(const uint8_t *plane, int w, int h,
                            int x0, int y0, bool mirror,
                            uint16_t tint, uint8_t scale)
{
    if (!plane || scale == 0) return;
    for (int y = 0; y < h; ++y) {
        int dy = y0 + y;
        if ((unsigned)dy >= EVA_WEATHER_RENDER_H) continue;
        const uint8_t *src = &plane[y * w];
        uint16_t *dst = &s_buf[dy * EVA_WEATHER_RENDER_W];
        for (int x = 0; x < w; ++x) {
            uint8_t m = src[mirror ? (w - 1 - x) : x];
            if (m < 6) continue;
            int dx = x0 + x;
            if ((unsigned)dx >= EVA_WEATHER_RENDER_W) continue;
            uint8_t a = (uint8_t)(((uint16_t)m * scale) / 255U);
            if (a) dst[dx] = blend565(dst[dx], tint, a);
        }
    }
}
```

Replace the two `draw_lightning_path(...)` calls with:

```c
    if (s_bolt_sprite_ok) {
        blit_bolt_plane(s_bolt_sprite.plane[1], s_bolt_sprite.w, s_bolt_sprite.h,
                        s_bolt_x, s_bolt_y, s_bolt_mirror,
                        rgb565(188, 210, 255), (uint8_t)((bolt_a * 3) / 4));
        blit_bolt_plane(s_bolt_sprite.plane[0], s_bolt_sprite.w, s_bolt_sprite.h,
                        s_bolt_x, s_bolt_y, s_bolt_mirror,
                        rgb565(248, 252, 255), bolt_a);
    } else {
        /* fallback: original polyline */
        draw_lightning_path(...unchanged...);
        if (s_lightning_has_branch) { ...unchanged... }
    }
```

- [ ] **Step 4: Build, flash, verify on hardware**

`weatherpin on`, `weatherraw 100 100 100 100 0 thunder 80`, then
`lightning` + immediate burst capture (scratchpad script): expect a
**branched** bolt with soft glow over the cloud deck in at least one frame;
`perf` during strikes: work must stay ≤ ~75 ms in storm (blit budget);
re-stroke variant switching visible across strikes. Show frames to the user.

---

### Task 6: Sun rays + moon phases (phase gate: hardware)

**Files:**
- Modify: `main/eva_weather_canvas.c` (`draw_sun_fib_light` ~2745, moon branch of `draw_sun_or_moon`)

- [ ] **Step 1: Rays**

At the top of `draw_sun_fib_light(float t)`:

```c
    eva_sprite_t ray;
    int phase = ((int)(t * 1.6f)) & 3;          /* ~0.6 s per phase */
    if (eva_cloud_assets_sprite(EVA_CLP_TYPE_RAY, phase, 0, &ray)) {
        int cx = (int)(s_sun_pos.x_n * EVA_WEATHER_RENDER_W) - ray.w / 2;
        int cy = (int)(s_sun_pos.y_n * EVA_WEATHER_RENDER_H) - ray.h / 2;
        blit_bolt_plane(ray.plane[0], ray.w, ray.h, cx, cy, false,
                        s_ray_tint_565(), s_ray_alpha());
        return;
    }
    /* fallback: existing wedge march below, unchanged */
```

where `s_ray_tint_565()` / `s_ray_alpha()` are the tint+alpha the wedge code
already computes (extract those two expressions into the helpers instead of
duplicating — the exact expressions live at the top of the current wedge
loop; move, don't copy).

- [ ] **Step 2: Moon**

In the moon branch of `draw_sun_or_moon()` (drawn during bg repaint), before
the flat-disc drawing:

```c
    eva_sprite_t moon;
    int ph = (s_moon_phase_pct * 8) / 101;      /* 0..7 */
    if (eva_cloud_assets_sprite(EVA_CLP_TYPE_MOON, ph, 0, &moon)) {
        /* luminance-modulated blit: colour = moon_grey × lum, alpha = disc */
        for (int y = 0; y < moon.h; ++y) {
            int dy = moon_y - moon.h / 2 + y;
            if ((unsigned)dy >= EVA_WEATHER_RENDER_H) continue;
            for (int x = 0; x < moon.w; ++x) {
                int dx = moon_x - moon.w / 2 + x;
                if ((unsigned)dx >= EVA_WEATHER_RENDER_W) continue;
                int sx = s_moon_waning ? (moon.w - 1 - x) : x;
                uint8_t a = moon.plane[0][y * moon.w + sx];
                if (a < 8) continue;
                uint8_t l = moon.plane[1][y * moon.w + sx];
                uint16_t c = rgb565((l * 236) / 255, (l * 236) / 255,
                                    (l * 226) / 255);
                s_buf[dy * EVA_WEATHER_RENDER_W + dx] =
                    blend565(s_buf[dy * EVA_WEATHER_RENDER_W + dx], c, a);
            }
        }
        return;   /* skip the flat disc */
    }
```

(`moon_x/moon_y/s_moon_phase_pct/s_moon_waning` — use the exact variable
names present in that branch; they exist because the flat disc and phase
data are already wired.)

- [ ] **Step 3: Build, flash, verify**

Clear-day: rays visible, breathing, no wedge artifacts; FPS unchanged on
clear scenes. Clear-night (`weatherdebug clear-night 0` at a night offset):
moon shows craters + terminator matching the live phase from `status`.
Screenshots to the user.

---

### Task 7: Glass rain drops + trails (phase gate: hardware)

**Files:**
- Modify: `main/eva_weather_canvas.c` (`update_and_draw_glass_drops` ~4322, glass statics ~78–123)

- [ ] **Step 1: Wet-glass accumulation buffer**

Statics:

```c
static uint8_t *s_wet_glass;            /* 800×480 A8, PSRAM, lazily alloc */
static int s_wet_y0 = 1 << 30, s_wet_y1 = -1;   /* dirty row range */
static uint8_t s_wet_decay_tick;
```

Allocate on first rain frame (`heap_caps_calloc`, 384 KB). On trail
deposit, stamp the trail sprite (`EVA_CLP_TYPE_TRAIL`, variant by drop
index parity) at the drop's previous position into `s_wet_glass`
(max-blend), expanding `s_wet_y0/y1`. Every 8th frame decay the dirty band:
`v = (v * 247) >> 8`, shrinking the band when rows fall below 4.

- [ ] **Step 2: Sprite drops**

In the drawing part of `update_and_draw_glass_drops()` keep the FSM and
motion untouched; replace the primitive drawing per drop with:

```c
        eva_sprite_t dsp;
        int size_c = d->r < 9 ? 0 : (d->r < 15 ? 1 : 2);
        int shape = (int)(d->phase * 3.0f) % 3;
        if (eva_cloud_assets_sprite(EVA_CLP_TYPE_DROP, size_c, shape, &dsp)) {
            uint8_t a = (uint8_t)(d->alpha * 255.0f);
            int x0 = (int)d->x - dsp.w / 2, y0 = (int)d->y - dsp.h / 2;
            blit_bolt_plane(dsp.plane[0], dsp.w, dsp.h, x0, y0, false,
                            rgb565(210, 220, 232), (uint8_t)((a * 3) / 5));
            blit_bolt_plane(dsp.plane[1], dsp.w, dsp.h, x0, y0, false,
                            rgb565(255, 255, 255), a);
            continue;   /* skip the primitive path */
        }
        /* fallback: existing primitive drawing, unchanged */
```

- [ ] **Step 3: Composite the wet-glass band**

In `composite_glass_overlay()` before the drops, blend `s_wet_glass` rows
`[s_wet_y0, s_wet_y1]` over `s_buf` with a cool tint
(`rgb565(196, 206, 220)`, alpha = buffer value ×0.5) — the slow-drying
condensation. Skip entirely when the band is empty.

- [ ] **Step 4: Build, flash, verify**

Rain (`weatherraw 66 66 66 66 0 rain 40` + pin): drops read as lenses
(specular + rim), sliding drops leave fading trails; `gl` bucket ≤ ~2 ms;
FPS ≥ 14 Hz. Screenshots to the user.

---

### Task 8: Docs + snapshot

**Files:**
- Modify: `../CLAUDE.md`
- Create: snapshot

- [ ] **Step 1: CLAUDE.md**

§6: add a bullet — CLP2 typed pack (sprite map table), sprites resident in
PSRAM via `eva_cloud_assets_sprite()`, every element keeps a procedural
fallback; regenerating = `genpool.py` (gates included) + `preview.py
--sprites` approval. §8: snapshot line.

- [ ] **Step 2: Snapshot**

```sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW
rsync -a --exclude build --exclude tools/cloudgen/.venv --exclude tools/cloudgen/__pycache__ \
    phase7_eva_weather/ phase7_eva_weather-snapshot-2026-07-03-sprites/
```

- [ ] **Step 3: Remind about the eva_wthr port** (all 2026-07-02/03 packages).
