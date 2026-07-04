# Pre-baked Cloud Masks — Design

**Date:** 2026-07-02
**Status:** Approved (design), pending implementation plan
**Owner file:** `main/eva_weather_canvas.c`, new tool `tools/cloudgen/`

## 1. Motivation

Cloud strips are currently procedurally baked **on the device**:
`bake_strip_high/mid/low()` paint hundreds of Gaussian blobs with per-pixel
`expf()` into three 800×768 A8 masks (light/shadow/core) per layer. The
morph lifecycle re-bakes a variant every 34–89 s in `cloud_bake_task`,
causing 30–80 ms frame dips and general instability under load.

The bake output does **not** depend on live weather — coverage % and tints
are applied at draw time. The masks depend only on the random seed. That
makes them a perfect candidate for offline pre-baking: generate the masks
on a PC, ship them in flash, and turn the on-device "bake" into a cheap
load + decompress.

Bonus: offline generation has no CPU budget, so the clouds can look
significantly better (fractal noise instead of Gaussian blobs).

## 2. Decisions made (with user)

| Decision | Choice |
|---|---|
| Visual target | **Better/more realistic** than current (FBM + domain warping), not a port of the Gaussian look |
| Source | Procedural offline generator (Python), not photos or 3D renders |
| Pool size | **4–5 variants per layer**, LZ4-compressed, ~4–6 MB in the 7 MB `storage` partition |
| Depth motion | **Variant-load-time scale ("depth breathing")** — no per-frame scaling passes |

## 3. Offline generator — `tools/cloudgen/` (Python)

Dependencies: numpy + Pillow only.

- **Shapes:** 2D FBM (5–6 octaves of simplex/value noise) with domain
  warping for torn edges. Noise is **periodic in X** → perfectly seamless
  horizontal tiling (replaces the runtime wrap workarounds in blob painting).
- **Three cloud-type profiles** matching the existing layers:
  - `cirrus` (HIGH): X-stretched streaks, low density;
  - `altocumulus` (MID): broken clumps;
  - `cumulus` (LOW): large volumetric masses with dense bellies.
- **Mask decomposition** keeps the runtime semantics intact: from the
  density field D(x,y), `light` = upper sun-facing part (vertical gradient
  inside each mass, same idea as the current `light_frac` smoothstep),
  `shadow` = lower part, `core` = high-density threshold biased downward.
  Device tint pipeline (`tint_light_* / tint_shadow_* / tint_core_*`)
  works unchanged.
- **Mask dimensions unchanged:** 800×768 (`CLOUD_STRIP_W` ×
  (`EVA_WEATHER_RENDER_H` + 2·`CLOUD_STRIP_OVERFLOW_Y`)) so nothing in the
  runtime needs recomputing.
- **Output format `.clm`:** small header (magic, version, w, h, 3
  compressed block sizes) followed by 3 LZ4 blocks (light, shadow, core).
  File naming: `cloud_L<layer>_v<n>.clm`, layer ∈ {0=HIGH,1=MID,2=LOW}.
- **Preview:** generator renders `preview_contact_sheet.png` — all variants
  composited over a sky gradient with real day/night/storm tints, so the
  look is approved on the PC before flashing.
- **Size-budget contingency:** noise-rich A8 compresses worse than smooth
  gradients; the LZ4 ratio is unknown until the generator exists. The
  generator must print the total compressed size. If the full pool
  (4–5 × 3 layers) exceeds ~6 MB, fall back in this order:
  1. drop to 4 variants per layer;
  2. bake at half resolution (400×384) and bilinear-upscale during
     decompression — clouds are soft, the upscale is visually lossless
     and quarters the storage.
  The `.clm` header carries w/h, so the device loader supports both
  resolutions from day one.

## 4. Device changes — `main/eva_weather_canvas.c`

### Build / flashing

- `.clm` files live in `spiffs_image/` (committed to the tree).
- `main/CMakeLists.txt` adds
  `spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)`
  — the image builds and flashes with the firmware. The 7 MB `storage`
  partition already exists in `partitions.csv` and is currently unused.
- LZ4 decoder: the `lz4` component from the IDF Component Registry
  (BSD, ~10 KB of code).

### Loader replaces generator

- `esp_vfs_spiffs_register()` at canvas init; scan
  `/storage/cloud_L*_v*.clm` → per-layer table of available variants
  (count is discovered, not hardcoded).
- `cloud_bake_task` keeps its name, priority, and FSM
  (`BAKE_REQUESTED → RUNNING → DONE`); only the body changes:
  `bake_strip_for_layer()` → `load_strip_variant(layer, variant_idx)` =
  file read + LZ4 decompress into the **existing** `a8_light/a8_shadow/
  a8_core` PSRAM buffers. Morph logic, scroll, tints, PPA blending — all
  untouched; `cloud_variant_t` interface is unchanged.
- **Variant selection:** random from the layer pool, excluding the
  currently active and the previous variant (no A→B→A flicker).
  Additionally a 50 % chance of X-mirroring applied during decompression
  (rows written right-to-left) — doubles the visual pool for free.
- **Fallback:** if SPIFFS fails to mount or no `.clm` files are found
  (first flash without the image, corrupted data), fall back to the old
  procedural bake with an `ESP_LOGW`. The old bake code is kept and
  renamed `bake_strip_fallback_*` in this phase; removal is a later
  cleanup once pre-baked masks are proven on hardware.

### What leaves the hot path

All per-pixel `expf()` loops over 614 KB masks. LZ4 decompression of
~1.8 MB on the ESP32-P4 is on the order of 10–20 ms; with the bilinear
depth-breathing resample (§5) the whole load is estimated at 30–60 ms in
the background task — and, critically, deterministic in time, unlike the
compute bake.

## 5. Depth motion ("breathing") + vertical drift

- On each `load_strip_variant()` a target scale is picked per layer:
  - HIGH: 0.95–1.05 (distant cirrus, barely breathes)
  - MID: 0.90–1.15
  - LOW: 0.85–1.25 (foreground breathes the most)
- Resampling happens **during decompression** (bilinear, CPU, in the
  background task): LZ4 decodes into a temporary row buffer, rows are
  written to the target buffer already scaled. Zero per-frame cost.
- Scale is centred on the viewport's vertical centre so clouds move
  "toward the viewer", not toward an edge. The existing morph crossfade
  smooths the transition — perceived as a slow depth change every
  34–89 s.
- Vertical motion: `scroll_y_off` (bob only today) is extended to
  drift + bob. Per-variant drift direction and speed, ±8–20 px per
  lifecycle.
- Together with wind-driven X scroll this covers X (wind), Y (drift),
  Z (breathing) and their combinations.
- Zoom margin already exists: 144 px overflow above/below the viewport
  and seamless X tiling — scales up to ~1.3 don't expose edges.
- Explicitly **rejected** (2026-07-02): continuous per-frame zoom via PPA
  SRM. The cloud-scene frame budget is already at its ~48–52 ms ceiling
  (see CLAUDE.md §7); another PSRAM-bandwidth-bound pass would cost FPS.
  May be revisited as an experiment after pre-baked masks ship.

## 6. Error handling

- Corrupt/missing files → procedural fallback (§4), warning log.
- Partial pool (e.g. only 2 variants for a layer) → work with what's
  there; selection rules degrade gracefully (with 1 variant, reuse it).
- Header validation: magic + version + dimensions must match compile-time
  constants; mismatch treats the file as absent.

## 7. Verification

- **PC:** contact sheet with all variants × day/night/storm tints,
  approved visually before flashing.
- **Device:** new CDC command `cloudinfo` (active variant per layer,
  current scales, last load duration) + existing `screenshot`.
- **Success criteria:**
  1. The 30–80 ms morph dips are gone (FPS chip / frame profiling).
  2. Cloud-scene FPS ≥ the current ~20 Hz baseline.
  3. Morph crossfades look smooth on hardware (no pop at variant swap).
- **Rollback:** old generator remains as fallback → rollback = flash
  without the SPIFFS image.

## 8. Out of scope

- Removing the fallback bake code (later cleanup phase).
- Continuous per-frame zoom (PPA SRM experiment, separate task).
- Any change to weather-kind logic, tints, precipitation particles,
  sky gradients.
- Porting to `eva_wthr` (GitHub copy) happens after hardware verification
  here, per the "keep both feature-equal" rule.
