# Visual Quality Package — Design

**Date:** 2026-07-02
**Status:** Approved (design), pending implementation plan
**Builds on:** `docs/2026-07-02-prebaked-cloud-masks-design.md` (shipped 2026-07-02)

## 1. Motivation

With cloud masks now pre-baked offline, visual quality can be iterated
entirely on the PC. This package covers the "prettier" half of the
prettier/smoother split (frame-rate work is explicitly out of scope):

1. Kill RGB565 banding on sky gradients (dithering).
2. Improve cloud shapes: cumulus volume, cirrus raggedness, distinct
   storm shapes (today storm differs only by tints).
3. Build a local preview rig so every change is approved on the PC
   before flashing: animated motion preview, device-exact sky rendering,
   golden-diff, pool quality gates.

## 2. Decisions made (with user)

| Decision | Choice |
|---|---|
| Scope | Base set + sky-palette shared header + PC scene simulator + golden-diff |
| Visual targets | LOW cumulus volume, HIGH cirrus raggedness, storm look |
| Storm approach | **Kind-aware pools**: separate storm mask set for LOW+MID, selected by weather kind (runtime-knobs-only variant rejected — shapes must differ) |
| Frame rate | Out of scope (separate pipeline work) |

## 3. Generator: profile iteration + storm pool (`tools/cloudgen/`)

- **LOW (cumulus):** add a ridged-noise component (inverted `|2·fbm−1|`)
  to the upper part of masses — cauliflower tops; heavier bellies via the
  existing transmittance term (`core_gain` biased down).
- **HIGH (cirrus):** anisotropic warp — displacement along X only with
  6–10× stretch; slightly higher Y frequency for thin filaments.
- **MID:** formulas unchanged; regenerated (golden-diff confirms no
  accidental change).
- **Storm pool:** new `PROFILES_STORM` for LOW and MID: coverage
  0.75–0.95, near-solid base with a ragged lower edge (ridged component
  at the bottom), strong core. Files `cloud_L<layer>s_v<n>.clm`
  (suffix `s`), 3 variants each for LOW/MID. HIGH has no storm pool —
  cirrus above a storm wall is invisible anyway (alpha_scale mutes it).
- `genpool.py` prints combined size of both pools; the same ~6 MB budget
  applies to everything in `spiffs_image/`.

## 4. Sky dithering (device-exact shared header)

- **New pure header `main/eva_dither.h`** (no ESP includes, same pattern
  as `eva_cloud_scale.h`): ordered Bayer 8×8 dithering.
  `dither565(r8, g8, b8, x, y)` adds the matrix threshold scaled to the
  per-channel quantisation step (R/B: 3 dropped bits, G: 2) before
  packing RGB565.
- **Device usage:** only in sky-gradient painting into the background
  buffer (`fill_gradient` and its bg-rebake equivalents) — cold path,
  zero per-frame cost. Clouds/sun/text untouched (no banding there).
- **Host tool `tools/skypreview.c`:** compiles with `cc` on the host,
  includes `eva_dither.h` + `eva_sky_palette.h` (§5), renders gradients
  to PNG/PPM. What you see on the PC is byte-identical to the panel.
- **Host test `tools/test_dither.c`:** determinism; 50 %-grey renders as
  a checker of adjacent levels; no clipping at 0/255; 8×8 block average
  closer to the target colour than undithered quantisation.

## 5. Sky palettes → shared header + PC scene simulator

- Extract from `eva_weather_canvas.c` into pure `main/eva_sky_palette.h`:
  `sky_for_kind()`, `clear_sky_palette()`, `sky_nightness()` (dusk ramp)
  and their colour constants. The canvas includes the header;
  **byte-identical behaviour — a move, not a rewrite.** Most invasive
  step of the package → separate task with a before/after device
  screenshot check. **Ordering constraint:** the extraction ships and is
  verified BEFORE dithering is enabled on the device — otherwise the
  screenshot-identity check is confounded by the intentional dither
  change.
- `skypreview` renders the grid: **all weather kinds × 4 times of day
  (day / sunset / deep night / sunrise)**, dithered — the local reference
  for how the panel sky looks in any combination.
- Python previews get sky colours from `skypreview --dump-json` (palettes
  exported once per run) — no palette duplication in Python.

## 6. Preview rig (`tools/cloudgen/`)

- **`animate.py`:** renders GIFs (Pillow only, no ffmpeg): 2 variants of
  a layer, real scroll speeds (FIB ladder × wind factor), morph
  crossfade on the real timeline (hold → crossfade), vertical drift and
  depth-scale change between variants. Presets:
  `--layer low --kind cloudy --wind 20`; `--all` → one GIF per layer +
  a combined scene. ~10 s of simulated time at ~10 fps preview is enough
  to judge swap pops, tiling seams, and depth breathing.
- **Golden-diff in `preview.py`:** `--golden` snapshots the current
  contact sheet into `golden/`; subsequent runs render a
  was/now/diff triptych (diff = amplified absolute difference).
- **Pool quality gates in `test_cloudgen.py`** (also run by `genpool.py`;
  generation fails if violated), for every file in both pools:
  - coverage within the profile's declared range;
  - light+shadow == round(density·255) (existing invariant);
  - unfeathered content never enters the viewport at the layer's max
    depth scale (zero-margin check at 1.25 / 1.15 / 1.05);
  - variants differ pairwise (mean |diff| above threshold).

## 7. Device: kind-aware pool selection

- `eva_cloud_assets_init()` scans both sets: `cloud_L<l>_v*.clm` and
  `cloud_L<l>s_v*.clm` → two count tables. API gains a pool dimension:
  `eva_cloud_assets_count(layer, pool)`,
  `eva_cloud_assets_load(layer, pool, idx, ...)`,
  enum `CLOUD_POOL_NORMAL / CLOUD_POOL_STORM`.
- `load_or_bake_variant()`: pool = STORM when the current `s_kind` ∈
  {THUNDERSTORM, HEAVY_RAIN} **and** the layer's storm pool is non-empty;
  otherwise NORMAL (HIGH always NORMAL). `pool_cur`/`pool_prev` carry the
  pool tag so the no-repeat rule works within a pool.
- Switching needs no new sync code: a kind change already triggers the
  conditional scene reset (stabilisation fix A3) → storm shapes appear
  immediately; adjacent-kind drift (RAIN→HEAVY_RAIN without reset) is
  picked up by the morph cycle within ≤89 s.
- Fallback unchanged: empty storm pool → NORMAL; nothing at all →
  procedural bake.
- `cloudinfo` reports the active pool per layer.

## 8. Verification

- **Host:** `test_dither`, extended `test_cloudgen`, `skypreview` grid
  and `animate.py` GIFs visually approved **before** flashing.
- **Device:**
  - before/after screenshots around the palette extraction (§5) must be
    identical;
  - `weatherdebug thunderstorm` → storm shapes visible in `screenshot`;
  - `cloudinfo` shows the active pool;
  - night gradient banding checked on a photo of the physical panel
    (banding is a panel artifact, not visible in RGB565 screenshots).
- **Success criteria:** banding gone; storm differs by shape, not just
  tint; FPS unchanged (dither is cold-path, pools don't change frame
  cost).

## 9. Out of scope

- Frame-rate work: frame pacing, pre-tinted single-pass strips, pipeline
  refactor (separate spec when prioritised).
- Golden-hour rim-light mask (possible later addition on top of this
  package).
- Aerial perspective baked into masks (later generator iteration).
- Porting to `eva_wthr` — after hardware verification, per the
  feature-parity rule.
