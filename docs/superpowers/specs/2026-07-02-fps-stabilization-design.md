# FPS Stabilization Package (A+B+C) — Design

**Date:** 2026-07-02
**Status:** Approved scope (user picked A+B+C), pending plan
**Investigation:** measured on hardware 2026-07-02; root causes confirmed in
code. Out of scope: D (pipeline overlap / double-buffered background → 25–30
Hz) — separate phase after A+B+C stabilizes.

## 1. Measured problem

10–14 Hz on all scenes (even 4 %-cloud clear-day), jitter spikes to 246 ms.
Per-frame breakdown (avg): `tx` 16–18 ms + `cl` 22–37 ms + `ppa_rot` ~15 ms +
bg copy ~6–8 ms. Confirmed root causes:

1. **`tx` 17 ms/frame:** all four text lines bake into ONE 800×480 A8 slot
   whose union bbox spans nearly the whole screen; `blit_text_slot` walks the
   bbox TWICE (drop shadow + fill) ≈ 600 K pixel ops/frame — even when text
   didn't change. Regression from the 288 px clock + single-slot refactor
   (post-2026-05-30 baseline, unrelated to cloud/dither packages).
2. **`cl` doubles during morph:** `blend_layer()` blends BOTH variants during
   the 8 s crossfade (up to 18 PPA passes vs 9) → FPS dips 14→10 every
   30–60 s per layer clock.
3. **SPIFFS loads stall the renderer:** each ~0.7 s variant load issues SPI
   flash transactions that suspend the flash cache; the render task
   micro-stalls for the whole read (observed 114/187/246 ms frame spikes).

## 2. Fix A — span-based text blit

- In `bake_scene_text_slot()`, after the A8 bake, build a per-row span table
  inside the bbox: for each row, a list of `(x_start, x_len)` runs of
  non-zero alpha (cap: 16 spans/row; if exceeded, merge into one row-wide
  span). Stored in the slot (`spans` array + `row_index`), rebuilt only on
  text rebake (once a minute / on weather change).
- `blit_text_mask_at()` gains a span-walking path: shadow pass and fill pass
  both iterate only the spans (shadow = same spans shifted +2/+2).
- Expected: `tx` 17 → ~3–5 ms. No visual change — identical pixels.
- Host test: extend the pure logic into `main/eva_text_spans.h` (span build
  from an A8 buffer + walk callback) with `tools/test_text_spans.c`
  (deterministic A8 fixtures: empty rows skipped, runs found, cap merge).

## 3. Fix B — raw asset partition + mmap (kills the freezes)

- **Pack format `clouds.bin`** (built by `genpool.py` after the gates):
  header `CLP1`, `u16 entry_count`, TOC of `{u8 layer, u8 pool, u8 variant,
  u8 reserved, u32 offset, u32 size}`, then the existing `.clm` blobs
  back-to-back. Individual `.clm` files remain on disk for preview tooling;
  `spiffs_image/` directory is retired.
- **Partition:** `storage` entry in `partitions.csv` changes subtype
  `spiffs` → `0x40` (custom). Build flashes `clouds.bin` at the storage
  offset via `esptool_py_flash_to_partition` custom target in
  `main/CMakeLists.txt` (replaces `spiffs_create_partition_image`).
- **Device:** `eva_cloud_assets_init()` does `esp_partition_find` +
  `esp_partition_mmap(_DATA)` once; scans the TOC instead of the VFS.
  `eva_cloud_assets_load()` LZ4-decompresses straight from the mapped
  pointer — **zero SPI transactions, zero cache suspension**. SPIFFS mount,
  `fopen` scan and the compressed-block heap buffer all go away.
- Expected: load 0.7 s → ~0.1–0.25 s (LZ4 + resample only), and — the point —
  **no render stalls during loads**.
- Fallback unchanged: bad magic/empty TOC → procedural bake.
- Host test: `tools/test_clp_toc.c` — TOC parse on a synthetic blob
  (valid/corrupt magic, out-of-range offsets rejected).

## 4. Fix C — cloud blend cost (REVISED after deeper code reading)

**Correction:** `blend_layer_variant()` already runs **light-only** — the
shadow/core passes were disabled for performance (see the "Light-only
rendering" comment). So per-frame cost is 1 mask/layer (×2 bands at scroll
wrap, ×2 variants during morph), and the original "9 → 3 passes" claim was
wrong. The measured `cl` 22–37 ms is explained by full-height 800×480 bands
per layer plus morph doubling and wrap bands. C is therefore re-scoped:

- **C1 — attribute first:** extend the perf log with a per-window PPA-blend
  band counter (`clb=N`) and a morphing-layers count, to confirm exactly
  where `cl` goes before touching it.
- **C2 — content-row clipping:** at variant load compute the light mask's
  populated row range; clip each blend band vertically to
  (content rows ∩ viewport) instead of always blending the full 480-row
  band. Thin cirrus (HIGH) and low-coverage scenes stop paying for empty
  rows.
- **C4 — morph collision cap:** never start a layer's crossfade while
  another layer is morphing (staggered independent clocks can collide
  today). Worst-case simultaneous double-blends drop from 3 to 1 →
  bounded `cl` during morphs.
- **C3 (gated, quality not speed):** pre-tinted compose of light+shadow+core
  into one PPA texture would restore the currently-lost cloud depth at
  today's per-frame cost. Only attempted if criteria in §5 are already met
  after C1/C2/C4, and only if `esp_driver_ppa` blend accepts a
  per-pixel-alpha FG format (probe ARGB8888 first; PSRAM +~0.6 MB/variant
  net after freeing A8 planes).
- Expected from C2+C4: `cl` steady ≤ 15 ms on clear/partly scenes, morph
  worst case ≤ ~30 ms with exactly one layer crossfading.

## 5. Expected outcome (success criteria)

Measured via `perf` + FPS log over ≥3 morph cycles per scene
(clear-day / cloudy / thunderstorm):

1. Steady tick ≥ 18 Hz on every scene, **including morph windows**.
2. No frame gap > 100 ms at any point (jitter max), including during
   variant loads and kind switches.
3. `tx` ≤ 5 ms; `cl` ≤ 15 ms steady and ≤ 20 ms during morph.
4. Pixel-identical text rendering (screenshot diff before/after A).
5. Storm/normal pool switching still works (`cloudinfo`), fallback intact.

## 6. Order of implementation

A (safe, isolated) → measure → B (storage swap) → measure → C (probe PPA
first; largest change) → measure. Each step lands and is verified on
hardware before the next starts, so any regression is attributable.
