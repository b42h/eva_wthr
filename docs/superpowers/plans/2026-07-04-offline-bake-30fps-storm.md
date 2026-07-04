# Offline-Bake for ≥30 FPS Night Storm — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut the per-frame pixel-moving floor so the worst-case scene — a night thunderstorm — renders at a stable ≥30 FPS, day and night, by moving more work off the device (offline-baked assets) and removing whole stages from the render critical path.

**Architecture:** The device already mmaps offline cloud/sprite assets; the remaining cost is *moving pixels*, not generating them. This plan removes the per-frame text blit (cache into an intermediate buffer), merges storm cloud layers offline (fewer PPA blends), turns rain/fog into offline A8 loops (fewer CPU particles), and finally removes the PPA rotation stage by rendering natively in portrait. Each step is a separate build/flash/measure cycle validated via the CDC `perf` command.

**Tech Stack:** ESP-IDF v5.5.4, ESP32-P4, C (firmware `main/`), Python (offline asset toolchain `tools/cloudgen/`), PPA hardware blend/rotate, CDC serial for measurement.

---

## Status (verified 2026-07-04)

Implementation complete for every task **except Task 1**. Task 1 (portrait-native
render) is **descoped by the user** — the whole project was already authored
natively for the current 270° screen rotation, so removing the PPA rotation
stage is unnecessary.

Verified against the working tree (not just descriptions):

| Task | Evidence |
|---|---|
| 0 — sunrise/sunset fix | debug/pin fallback added in `main/main.c:1678-1681` (fills `st.sunrise_min` from live when <0); no `SUNDIAG` logs left; `sun_events()` 06:00/18:00 kept only as safety net |
| 2 — text cache | `s_scene_base` / `s_scene_base_dirty` / `s_scene_text_done_this_frame` / `scene_text_can_cache()` present and wired in `eva_weather_canvas.c` hot path (`:5044-5081`), allocated in both init paths (`:5413,:5530`) |
| 2.5 — repartition | `partitions.csv` = factory 4M / storage 11M; `flash_args` flashes `../assets/clouds.bin` at `0x410000`; build reports app 0x400000 partition 63% free |
| 3 — merged storm | `CLOUD_POOL_STORM_MERGED=2` in `eva_cloud_assets.h`; `use_merged_storm_layers()` + single-blend path (`:5096`); pack TOC has **3** merged cloud entries |
| 4 — rain/fog offline | `EVA_CLP_TYPE_RAIN/FOG` (6/7); `rain.py`/`fog.py` present; genpool wired; pack TOC has **RAIN=32, FOG=1**; `fill_sky_rows` unaffected |
| 5 — lit deck | `CLOUD_POOL_STORM_LIT=3`, `s_storm_lit_active`, `decompose_lit`; pack TOC has **9** lit cloud entries |
| 6 — sky keyframes | `EVA_CLP_TYPE_SKY=8`; `skygen.py`; `fill_sky_rows` reads `EVA_CLP_TYPE_SKY` (`:1361`); pack TOC has **SKY=32** |
| 1 — portrait-native | **DESCOPED** — not needed (project already native to current rotation) |

**Build:** `idf.py build` succeeds (ESP-IDF v5.5.4); `eva_weather.bin` 1.56 MB.
**Pack:** `assets/clouds.bin` grew 6.65 MB → 9.93 MB (126 TOC entries) — the new
offline assets are really baked in and flash into the enlarged storage partition.

**Not verified here (needs hardware):** on-device `perf` FPS numbers and
screenshot regression per task. The host cloudgen pytest suite was not run —
`tools/cloudgen/.venv` lacks `pytest`/`numpy`. Recommend: install those into the
venv and run `python -m pytest test_cloudgen.py`, then flash and capture the
final DoD (`thunderstorm` night ≥30 Hz, `fog` ≥25 Hz, screenshots vs baseline).

---

## Preconditions & conventions

**Not a git repo.** The project root reports `Is a git repository: false`. Commit steps in this plan use the project's snapshot convention instead of `git commit`: after each task passes, copy the working tree to a snapshot directory (see the snapshot ladder in `../../../CLAUDE.md §8`) named `phase7_eva_weather-snapshot-2026-07-04-<step>` (tree copy, no `build/`, no `.venv`). If the user initializes git later, replace snapshot steps with real commits. **Do NOT run `git` commands** — they will fail or prompt.

**Build/flash/measure loop** (used at the end of every task that changes firmware):

```sh
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf"
source $IDF_PATH/export.sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
idf.py build
# Flash (works while firmware is alive; see CLAUDE.md §3):
cd build
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem1234561 \
    -b 460800 --before usb_reset --after hard_reset write_flash @flash_args
cd ..
```

**Measure** (per `next_update.md` protocol — wait ≥10 s after boot, sample ≥8 s):
`perf` over CDC prints `tick=N Hz, work_us=W (bg=… tx=… cl=… pa=… li=… gl=… ppa_rot=… vsync=… clb=… mfr=…) jitter=min..max`. The log line is emitted every ~270 ms from `native_render_task` in `main/eva_weather_canvas.c:5078`.

**Test scenes** (force via CDC, then measure): `weatherdebug clear-day`, `weatherdebug partly-cloudy-day`, `weatherdebug thunderstorm`, `weatherdebug fog`. For **night** variants use `clockoffset <hours>` to move wall-clock into the night window, or `weatherdebug clear-night` etc. Pin the scene with `weatherpin on` before a long run (live fetch overwrites debug state — `CLAUDE.md §6`).

**Screenshot regression** (visual DoD on every task): `python tools/eva-screenshot.py` captures the current frame; keep a baseline JPEG per scene from the pre-change state and diff. Batch: `python tools/eva-batch-screenshots.py`.

**Python host tests** (offline toolchain): run from `tools/cloudgen/` with `python -m pytest test_cloudgen.py -v`.

**Golden baseline (do this FIRST, before Task 0):**

- [ ] **Step: Capture the pre-change baseline**

Run the build/flash loop to ensure the currently-shipped tree is on-device, then for each scene (`clear-day`, `partly-cloudy-day`, `thunderstorm`, `fog`, and each at night via `clockoffset 12`) capture: (a) a `perf` line, (b) a screenshot. Save `perf` lines to `docs/superpowers/plans/baseline-2026-07-04.txt` and screenshots to `screenshots/baseline-2026-07-04/`. These are the regression references for every task below.

Expected baseline (from `CLAUDE.md §7`, confirm on-device): thunderstorm ~17–19 Hz work ~52–58 ms; fog ~8–9 Hz.

---

## File structure

Files this plan creates or modifies, by responsibility:

**Firmware (`main/`):**
- `eva_weather_canvas.c` — the render loop and all buffers. Touched by every step. It is 5711 lines; this plan does **not** restructure it, only adds/edits the targeted functions.
- `weather_fetch.c` / `weather_fetch_openmeteo.c` — sunrise/sunset data path (Task 0 only).
- `eva_cloud_assets.h` / `eva_cloud_assets.c` — asset pool enum + loader (Task 3 merged pool, Task 4 rain/fog types).
- `eva_clp_toc.h` — CLP asset type constants (Task 4 adds RAIN/FOG types; keep in sync with `genpool.py`).

**Offline toolchain (`tools/cloudgen/`):**
- `genpool.py` — pack builder. Touched by Tasks 3, 4, 6, 1.
- `cloudgen.py` — cloud profiles + decompose (Task 3 merged-strip bake).
- `rain.py` *(new)* — offline rain-streak A8 loop generator (Task 4).
- `fog.py` *(new)* — offline fog-band A8 loop generator (Task 4).
- `skygen.py` *(new)* — offline sky keyframe-column generator (Task 6).
- `test_cloudgen.py` — host gate tests (extended per task).

**Config:**
- `partitions.csv` — flash repartition (between Task 2 and Task 3).

**Host tools:**
- `tools/eva-screenshot.py` — add portrait→landscape rotation for screenshots (Task 1 only).

---

## Task 0: Fix sunrise/sunset reaching the canvas

**Why first:** `sun_events()` (`eva_weather_canvas.c:1383`) falls back to 06:00/18:00, so day/night palette switches at fixed hours regardless of season. "Day and night" validation of the night storm is meaningless until real sunrise/sunset reach the canvas. This is a diagnostic-first task — the loss is somewhere between the openmeteo parse and `s_sunrise_min`.

**Files:**
- Diagnose: `main/weather_fetch.c:125-184`, `main/weather_fetch_openmeteo.c:208-235`, `main/eva_weather_canvas.c:1383-1387,5500-5503`
- Modify: whichever of the above drops the value (determined in Step 1)

- [ ] **Step 1: Add diagnostic logging to trace the value**

In `main/weather_fetch.c`, right after the sun merge block (`main/weather_fetch.c:184`, after the `else if (om && om->has_sun)` branch), add:

```c
ESP_LOGW(TAG, "SUNDIAG merge: co_fresh=%d co_has_sun=%d om_has_sun=%d -> st.sunrise=%d st.sunset=%d",
         co && co_fresh ? 1 : 0,
         (co && co->has_sun) ? 1 : 0,
         (om && om->has_sun) ? 1 : 0,
         (int)st.sunrise_min, (int)st.sunset_min);
```

In `main/eva_weather_canvas.c`, in the state-apply function right after `main/eva_weather_canvas.c:5503` (`s_moonset_min = st->moonset_min;`), add:

```c
ESP_LOGW(TAG, "SUNDIAG canvas: applied s_sunrise_min=%d s_sunset_min=%d",
         (int)s_sunrise_min, (int)s_sunset_min);
```

- [ ] **Step 2: Build, flash, observe the two diag lines**

Run the build/flash loop. Watch the CDC log for both `SUNDIAG` lines after Wi-Fi/NTP + first fetch (~15–30 s). Record the values. Three possible outcomes:
- `st.sunrise` already wrong at merge → bug is in the openmeteo parse (`iso_to_minutes` or `has_sun` never set). Inspect `main/weather_fetch_openmeteo.c:208-223`.
- `st.sunrise` correct at merge but canvas shows 6:00/18:00 → the state passed to the canvas isn't the merged `st` (a different code path calls `eva_weather_canvas_set`, or a pinned/debug state clobbers it). Grep callers of the canvas state-apply.
- Both correct but palette still wrong → the bug is downstream in `sky_ctx_now`/`eva_sky_nightness`, not the data path.

- [ ] **Step 3: Apply the targeted fix for the outcome from Step 2**

Fix only the confirmed break. Do not speculatively change all three sites. Example shapes (pick the one that matches):
- If openmeteo `has_sun` never sets: verify `daily` object is non-null and the ISO strings are ≥16 chars; `iso_to_minutes` needs `iso[11..15]` to be `HH:MM` — confirm the openmeteo response uses that format with `timezone=auto`.
- If a debug/pin path clobbers: ensure `weatherdebug` synthesizes plausible sunrise/sunset (currently it likely leaves them 0/-1). In `main/main.c` `debug_weather_in_lvgl`/`weatherdebug` state fill, set `st.sunrise_min` / `st.sunset_min` to the live cached values instead of leaving them zero.

- [ ] **Step 4: Verify on device**

`weatherdebug clear-night` at **14:00 wall-clock** (`clockoffset` to daytime) must show a **daytime** sky, not a warm dusk. Then `clockoffset` into real evening — the night palette must arrive at the real sunset minute, not 18:00. Capture a `perf` line (no FPS change expected — this is correctness only).

- [ ] **Step 5: Remove the SUNDIAG logs and snapshot**

Delete both `ESP_LOGW(TAG, "SUNDIAG ...")` lines. Rebuild, flash, confirm the fix still holds. Snapshot the tree as `phase7_eva_weather-snapshot-2026-07-04-sunfix`.

---

## Task 2: Cache scene text into an intermediate buffer (remove per-frame `tx`)

**Why:** `tx ≈ 8–9 ms` is a per-frame **blit** (shadow pass + main pass) of the already-baked A8 mask onto `s_buf` in PSRAM (`draw_scene_text_overlays` → `blit_text_slot`, `eva_weather_canvas.c:1273-1291,4898`). `bake_scene_text_slot` already caches the mask (rebakes only on text change). We remove the per-frame blit by keeping a `s_scene_base` = "sky+sun+text" buffer, rebuilt only when the bg-cache rebakes OR the text changes.

**Z-order constraint (`eva_weather_canvas.c:4874`):** bottom→top is sky+sun → rain/lightning → **text** → clouds → glass. Text is *above* rain. So for **precip kinds**, baking text into the base would move rain on top of the clock. Therefore:
- **Non-precip kinds:** cache "sky+sun+text" unconditionally (the main win).
- **Precip kinds:** keep per-frame text by default; only enable the cache if a screenshot proves rain-over-digits looks acceptable (Step 8).

**Files:**
- Modify: `main/eva_weather_canvas.c` — add `s_scene_base` buffer (near `s_bg_buf` decl `:239-245`), allocate it (`:5320`), populate it, and branch the hot path (`:4877-4900`).

- [ ] **Step 1: Declare the intermediate buffer and its dirty flag**

Near `main/eva_weather_canvas.c:245` (after `static uint16_t *s_bg_next;`), add:

```c
/* "sky+sun+text" composite: bg cache with the scene text pre-blitted.
 * Rebuilt when the bg cache rebakes OR the text mask changes, so the hot
 * path copies this instead of (copy bg) + (blit text every frame). Used for
 * non-precip kinds; precip kinds keep per-frame text (rain sits above text). */
static uint16_t *s_scene_base;
static bool s_scene_base_dirty = true;   /* force first build */
```

- [ ] **Step 2: Allocate it alongside the other buffers**

After `main/eva_weather_canvas.c:5334` (the `s_bg_next` alloc + warn block), add:

```c
s_scene_base = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                       EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
if (!s_scene_base) {
    ESP_LOGW(TAG, "scene-base buffer alloc failed — per-frame text fallback");
}
```

Do the same after the *other* alloc site at `main/eva_weather_canvas.c:5218` (the code has two init paths — `eva_weather_canvas_init` and the native path; both allocate `s_bg_next`). Add the identical block after each.

- [ ] **Step 3: Add a helper to classify precip kinds for the cache**

There is already `weather_kind_has_precip_particles()` (declared `:275`). Reuse it: precip kinds (rain/heavy-rain/storm/snow/sleet/hail) return true → those keep per-frame text. Add a small predicate near the render helpers:

```c
static inline bool scene_text_can_cache(weather_kind_t kind)
{
    /* Non-precip kinds bake text into s_scene_base (rain would otherwise
     * sit above the digits). Precip default: per-frame text. Flip to true
     * for precip only after screenshot approval (Task 2 Step 8). */
    return !weather_kind_has_precip_particles(kind);
}
```

- [ ] **Step 4: Mark the base dirty when text changes**

In `bake_scene_text_slot` (`main/eva_weather_canvas.c:1032`), at the very end of the *rebake* path (after `slot->spans_valid = ...`, `:1184`), add:

```c
    s_scene_base_dirty = true;   /* text mask changed → scene-base stale */
```

Also set `s_scene_base_dirty = true;` in `render_weather` wherever the bg cache swaps (after `main/eva_weather_canvas.c:4867`, the final-slice pointer swap block) and on kind change (in the `s_kind != s_prev_kind` block, `:4793-4807`).

- [ ] **Step 5: Build the base and branch the hot path**

Replace the copy block at `main/eva_weather_canvas.c:4877-4879`:

```c
    if (s_bg_buf && !sky_refreshed) {
        (void)ppa_copy_rgb565(s_buf, s_bg_buf);
    }
```

with:

```c
    if (s_scene_base && scene_text_can_cache(s_kind)) {
        if (s_scene_base_dirty && s_bg_buf) {
            /* Rebuild "sky+sun+text": bg cache + baked text, once. */
            (void)ppa_copy_rgb565(s_scene_base, s_bg_buf);
            uint16_t *save = s_buf;
            s_buf = s_scene_base;
            draw_scene_text_overlays();   /* blits into s_scene_base now */
            s_buf = save;
            s_scene_base_dirty = false;
        }
        (void)ppa_copy_rgb565(s_buf, s_scene_base);
        /* Text already in s_buf via s_scene_base; skip the per-frame blit
         * below by leaving a flag the text stage checks. */
        s_scene_text_done_this_frame = true;
    } else if (s_bg_buf && !sky_refreshed) {
        (void)ppa_copy_rgb565(s_buf, s_bg_buf);
        s_scene_text_done_this_frame = false;
    } else {
        s_scene_text_done_this_frame = false;
    }
```

Declare `static bool s_scene_text_done_this_frame;` near the buffer decls. Then guard the per-frame text call at `main/eva_weather_canvas.c:4897-4900`:

```c
    int64_t tb_text0 = esp_timer_get_time();
    if (!s_scene_text_done_this_frame) {
        draw_scene_text_overlays();
    }
    int64_t tb_after_text = esp_timer_get_time();
    s_prof_text_us += (tb_after_text - tb_text0);
```

- [ ] **Step 6: Build and flash**

Run the build/flash loop. Expected: compiles clean; boots; no `scene-base buffer alloc failed`.

- [ ] **Step 7: Measure non-precip win**

`weatherdebug clear-day` (pinned), wait ≥10 s, capture `perf`. Expected: `tx` drops from ~8–9 ms to **~0** (the base copy absorbs it; `bg` stays ~10 ms since it's the same memcpy count). `clear-day` tick should rise. Repeat for `partly-cloudy-day` and `cloudy`. Screenshot each; diff against baseline — text must be pixel-identical in position/appearance.

- [ ] **Step 8: Decide the precip variant by screenshot**

`weatherdebug thunderstorm`, screenshot. Temporarily flip `scene_text_can_cache` to `return true;` for all kinds, rebuild, flash, screenshot storm again. Compare: does rain over the clock digits look acceptable? 
- **If yes:** keep `return true` for precip → storm `tx≈0` too (extra win). 
- **If no:** revert to `return !weather_kind_has_precip_particles(kind);`. Storm keeps per-frame text; that's the documented default.

Record the decision in a comment at `scene_text_can_cache`.

- [ ] **Step 9: Snapshot**

Snapshot the tree as `phase7_eva_weather-snapshot-2026-07-04-text-cache`. Record the new `perf` numbers in `docs/superpowers/plans/baseline-2026-07-04.txt` under a "post-Task-2" heading.

---

## Task 2.5: Repartition flash for new offline assets

**Why:** `storage` (7M) is nearly full (`clouds.bin` = 6.65M, ~350 KB free). Merged storm masks (Task 3), rain/fog loops (Task 4), and sky keyframes (Task 6) won't fit. `factory` (8M) holds only 1.56M. Shift 4 MB from factory to storage.

**Files:**
- Modify: `partitions.csv`

- [ ] **Step 1: Edit the partition table**

`partitions.csv` currently:

```
factory,  app,  factory, ,        8M,
storage,  data, 0x40,    ,        7M,
```

Change to:

```
factory,  app,  factory, ,        4M,
storage,  data, 0x40,    ,        11M,
```

Verify total ≤ 16 MB (`CONFIG_ESPTOOLPY_FLASHSIZE="16MB"`): nvs 0x6000 + phy 0x1000 + factory 4M + storage 11M + offsets ≈ 15 MB. OK.

- [ ] **Step 2: Full erase + flash (one-time, repartition needs it)**

```sh
cd build
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem1234561 -b 460800 erase_flash
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem1234561 \
    -b 460800 --before usb_reset --after hard_reset write_flash @flash_args
```

- [ ] **Step 3: Verify boot + assets**

Device boots; `cloudinfo` over CDC reports `cloud assets: mmap CLP2` (the pack still mounts at the new storage offset). Capture a `perf` line to confirm no regression.

- [ ] **Step 4: Snapshot**

Snapshot as `phase7_eva_weather-snapshot-2026-07-04-repartition`.

---

## Task 3: Merged storm cloud layers offline (fewer PPA blends)

**Why:** For solid-overcast kinds (thunderstorm/heavy-rain/cloudy) the MID/LOW parallax is invisible, yet the device does 2–3 full-width PPA A8 blends (`cl` = 12–15 ms). Bake MID+LOW into one merged A8 strip offline → 1 blend for those kinds.

**Files:**
- Modify: `tools/cloudgen/cloudgen.py` (merged-strip decompose), `tools/cloudgen/genpool.py` (emit merged entries), `tools/cloudgen/test_cloudgen.py` (gate)
- Modify: `main/eva_cloud_assets.h` (pool enum), `main/eva_weather_canvas.c` (select merged pool for storm kinds, blend once)

- [ ] **Step 1: Host test — merged strip carries visible mass**

In `tools/cloudgen/test_cloudgen.py`, add:

```python
def test_merged_storm_strip_light_carries_mass():
    import cloudgen, numpy as np
    D_mid = cloudgen.density_field(cloudgen.PROFILES_STORM["MID"])
    D_low = cloudgen.density_field(cloudgen.PROFILES_STORM["LOW"])
    merged = cloudgen.merge_strips(D_mid, D_low)
    light, _shadow, _core = cloudgen.decompose(merged, cloudgen.PROFILES_STORM["LOW"])
    # Device renders LIGHT only (CLAUDE.md §6). Merged strip must have mass there.
    assert light.mean() > 12, "merged storm light plane too sparse to see on panel"
```

- [ ] **Step 2: Run it — verify it fails**

Run: `cd tools/cloudgen && python -m pytest test_cloudgen.py::test_merged_storm_strip_light_carries_mass -v`
Expected: FAIL — `merge_strips` not defined.

- [ ] **Step 3: Implement `merge_strips`**

In `tools/cloudgen/cloudgen.py`, add:

```python
def merge_strips(d_upper, d_lower):
    """Composite two density fields into one strip (over operator, clamped).
    Upper (MID) sits behind lower (LOW) from the viewer; lower dominates."""
    import numpy as np
    a = np.asarray(d_upper, dtype=np.float32)
    b = np.asarray(d_lower, dtype=np.float32)
    if a.shape != b.shape:
        h = max(a.shape[0], b.shape[0]); w = max(a.shape[1], b.shape[1])
        def fit(x):
            import numpy as np
            y = np.zeros((h, w), np.float32); y[:x.shape[0], :x.shape[1]] = x; return y
        a, b = fit(a), fit(b)
    return np.clip(b + a * (1.0 - b), 0.0, 1.0)
```

(If `density_field` doesn't exist as a name, use whatever `cloudgen.decompose` already consumes — inspect the current `main()` in `genpool.py:52` which calls `cloudgen.decompose(D, ...)`; `D` is the density field. Name the merge helper to take two such `D`s.)

- [ ] **Step 4: Run the test — verify it passes**

Run: `python -m pytest test_cloudgen.py::test_merged_storm_strip_light_carries_mass -v`
Expected: PASS.

- [ ] **Step 5: Add a merged pool enum value**

In `main/eva_cloud_assets.h:17-19`, extend:

```c
typedef enum {
    CLOUD_POOL_NORMAL       = 0,
    CLOUD_POOL_STORM        = 1,
    CLOUD_POOL_STORM_MERGED = 2,   /* MID+LOW pre-composited, 1 blend */
} cloud_pool_t;
```

- [ ] **Step 6: Emit merged entries from genpool**

In `tools/cloudgen/genpool.py`, mirror the constant and add a merged-bake loop after the storm loop (`genpool.py:59-67`). Use `layer = 0` under `subtype = 2` (`CLOUD_POOL_STORM_MERGED`) for the single merged strip; bake `STORM_VARIANTS` variants:

```python
CLOUD_POOL_STORM_MERGED = 2
for v in range(STORM_VARIANTS):
    D_mid = cloudgen.density_field(cloudgen.PROFILES_STORM["MID"])
    D_low = cloudgen.density_field(cloudgen.PROFILES_STORM["LOW"])
    merged = cloudgen.merge_strips(D_mid, D_low)
    masks = cloudgen.decompose(merged, cloudgen.PROFILES_STORM["LOW"])
    path = clm.write(masks, f"out/cloud_merged_v{v}.clm")
    entries.append((CLP_TYPE_CLOUD, 0, CLOUD_POOL_STORM_MERGED, v, path))
```

(Match the exact `entries.append` tuple arity the current code uses — `genpool.py:56` shows `(CLP_TYPE_CLOUD, layer, poolsubtype, v, path)`.)

- [ ] **Step 7: Regenerate the pack**

Run: `cd tools/cloudgen && python genpool.py` then review: `python preview.py` (contact sheet) confirms the merged strip looks like a solid storm deck. Rebuild the app (pack flashes with it via `esptool_py_flash_to_partition`).

- [ ] **Step 8: Device — select merged pool for storm kinds, blend once**

In `main/eva_weather_canvas.c`, `pick_pool_variant`/`load_or_bake_variant` (`:1941,1967`) currently choose STORM for storm kinds. Add: when kind is THUNDERSTORM/HEAVY_RAIN and `eva_cloud_assets_count(0, CLOUD_POOL_STORM_MERGED) > 0`, load the merged strip into a single active layer and, in the blend block (`:4902-4914`), blend only that one layer instead of MID+LOW:

```c
    if ((s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_HEAVY_RAIN) &&
        s_merged_storm_active) {
        (void)blend_layer(&s_strip[CLOUD_LAYER_LOW], false);  /* holds merged */
    } else {
        bool sky_wrap = false;
        if (blend_layer(&s_strip[CLOUD_LAYER_HIGH], sky_wrap)) sky_wrap = false;
        if (blend_layer(&s_strip[CLOUD_LAYER_MID], sky_wrap)) sky_wrap = false;
        (void)blend_layer(&s_strip[CLOUD_LAYER_LOW], sky_wrap);
    }
```

Set `s_merged_storm_active` where the pool is selected. HIGH cirrus is invisible under solid overcast — decide by screenshot (Step 10) whether to keep it as a 2nd blend or drop it.

- [ ] **Step 9: Build, flash, measure**

`weatherdebug thunderstorm` (pinned, day and night). Expected: `cl` drops from 12–15 ms to **≤7 ms**; `clb` (blend bands) count halves.

- [ ] **Step 10: Screenshot regression**

Storm screenshot (day + night) vs baseline. The deck must read as a full storm — no visible seam from the merge, no loss of density. If HIGH was dropped, confirm no visible gap at the top. Adjust `merge_strips` opacity or keep HIGH if needed.

- [ ] **Step 11: Snapshot**

Snapshot as `phase7_eva_weather-snapshot-2026-07-04-merged-storm`. Update the baseline file with post-Task-3 numbers.

---

## Task 4: Offline A8 rain + fog loops (fewer CPU particles)

**Why:** Rain is ~314 CPU particles (`pa` = 3–6 ms, *variable* cost → `adapt_budget` oscillation). Fog is 144 CPU particles ≈ 32 ms (→ 8–9 Hz). Replace both with offline A8 loop frames blitted in one PPA pass. Glass drops stay live (cheap, local, a feature).

**Files:**
- Create: `tools/cloudgen/rain.py`, `tools/cloudgen/fog.py`
- Modify: `tools/cloudgen/genpool.py` (emit rain/fog entries), `main/eva_clp_toc.h` (RAIN/FOG types), `main/eva_cloud_assets.h`/`.c` (loader), `main/eva_weather_canvas.c` (blit loop, fallback to particles)
- Test: `tools/cloudgen/test_cloudgen.py`

- [ ] **Step 1: Host test — rain loop frames are non-empty and cyclic**

In `test_cloudgen.py`:

```python
def test_rain_loop_frames_nonempty_and_tileable():
    import rain, numpy as np
    frames = rain.gen_rain_loop(intensity="storm", wind_tilt=0.2, n=8)
    assert len(frames) == 8
    for f in frames:
        assert f.dtype == np.uint8 and f.mean() > 3, "rain frame too sparse"
    # Vertical tileability: top edge streak mass ≈ bottom edge (loop seam).
    top = frames[0][:8, :].mean(); bot = frames[0][-8:, :].mean()
    assert abs(top - bot) < 6, "rain frame not vertically tileable (visible seam)"
```

- [ ] **Step 2: Run — verify it fails**

Run: `cd tools/cloudgen && python -m pytest test_cloudgen.py::test_rain_loop_frames_nonempty_and_tileable -v`
Expected: FAIL — no module `rain`.

- [ ] **Step 3: Implement `rain.py`**

Create `tools/cloudgen/rain.py`:

```python
"""Offline rain-streak A8 loop generator. Frames tile vertically so a
scrolling blit loops seamlessly. Device blits one frame per tick via PPA."""
import numpy as np

H, W = 480, 800  # landscape render size (matches EVA_WEATHER_RENDER_*)

def gen_rain_loop(intensity="storm", wind_tilt=0.2, n=8, seed=0x4880):
    rng = np.random.default_rng(seed)
    density = {"light": 120, "storm": 340}[intensity]
    frames = []
    # Persistent streaks scrolled by phase so the set loops after n frames.
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
        frames.append(f)
    return frames
```

- [ ] **Step 4: Run — verify it passes**

Run: `python -m pytest test_cloudgen.py::test_rain_loop_frames_nonempty_and_tileable -v`
Expected: PASS. (If the seam assertion fails, the phase wrap needs `% H` on both `ys` and streak `t` — already applied above.)

- [ ] **Step 5: Implement `fog.py` (analogous, static+slow-drift bands)**

Create `tools/cloudgen/fog.py`:

```python
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
```

Add a host test `test_fog_band_nonempty` asserting `gen_fog_band().mean() > 8`; run it (fail → pass) as in Steps 1–4.

- [ ] **Step 6: Add CLP types + emit entries**

In `main/eva_clp_toc.h:24-29`, add:

```c
#define EVA_CLP_TYPE_RAIN  6   /* offline rain-streak A8 loop frame */
#define EVA_CLP_TYPE_FOG   7   /* offline fog band A8 (single, drifted) */
```

Mirror in `genpool.py:20-25` and emit: for rain, `n` frames × 2 intensities × 2 tilts under `EVA_CLP_TYPE_RAIN`; for fog, one frame under `EVA_CLP_TYPE_FOG`. Follow the `add_sprite(...)` pattern at `genpool.py:94-109`.

- [ ] **Step 7: Regenerate pack + rebuild**

`cd tools/cloudgen && python genpool.py`; review `python preview.py --sprites`; rebuild app.

- [ ] **Step 8: Device — blit rain loop instead of particles**

In `main/eva_weather_canvas.c`, `update_and_draw_particles` (called `:4884`): when kind is a rain kind and `eva_cloud_assets_sprite(EVA_CLP_TYPE_RAIN, ...)` exists, advance a frame index by `dt` and PPA-blend that A8 frame over `s_buf` (one blend), returning early before the particle loop. Keep the existing particle path as the `else` fallback. Same for fog with `EVA_CLP_TYPE_FOG` (single frame, horizontal drift offset by `t`).

- [ ] **Step 9: Build, flash, measure**

`weatherdebug thunderstorm` and `weatherdebug fog` (day + night). Expected: rain `pa` ≤2 ms and *flat* (jitter min..max narrows — no once-a-second dip); **fog ≥25 Hz** (was 8–9). Screenshot both vs baseline — rain look and fog density must match.

- [ ] **Step 10: Snapshot**

Snapshot as `phase7_eva_weather-snapshot-2026-07-04-rain-fog-offline`. Update baseline file.

---

## Task 1: Portrait-native render (remove the PPA rotation stage)

**Why:** `ppa_rot` ≈ 15 ms per frame rotates 800×480 → 480×800 (`rotate_render_to_dpi_fb`, `eva_weather_canvas.c:4960-4995`). Render natively in portrait 480×800 straight into the DPI back-fb; the rotation stage and the separate `s_render_buf` disappear. **Most invasive** (touches the whole canvas coordinate system) — hence last, after the cheap wins confirmed the budget model.

**Files:**
- Modify: `main/eva_weather_canvas.c` (render size constants, all draw coords, drop rotation, render into `s_dpi_back_fb`), `tools/cloudgen/genpool.py` + `cloudgen.py` (bake portrait-oriented), `tools/eva-screenshot.py` (host-side rotate for screenshots)

- [ ] **Step 1: Flip the render dimensions**

At `main/eva_weather_canvas.c:53-54`, the render size is 800×480. Introduce portrait render constants and switch the render buffer + all layout math to 480 wide × 800 tall. This is the crux: every `EVA_WEATHER_RENDER_W/H` use, the cloud strip Y bands (`CLAUDE.md §6`: HIGH y=20..120 etc. — these become X or rotated Y depending on chosen mapping), the sun trajectory, text slot layout. **Choose the mapping once and document it**: physical panel is portrait 480×800; the current landscape scene is rotated 270° CCW. Native-portrait means the scene is authored in portrait — the clock is horizontal across the 480-wide axis, clouds drift along the 800-tall axis.

- [ ] **Step 2: Render directly into the DPI back buffer**

In `native_render_task` (`:4997`), remove the `rotate_render_to_dpi_fb` call (`:5028`) and its semaphore wait. Point `s_buf`/`s_render_buf` at `s_dpi_back_fb` (portrait 480×800, already two of them, `:5379-5384`). After `render_weather`, go straight to `esp_lcd_panel_draw_bitmap` + vsync swap. Delete `s_render_buf` alloc (`:5304`) — reuse the DPI FBs.

- [ ] **Step 3: Bake assets portrait-oriented**

In `tools/cloudgen/`, add a `--portrait` transform so `.clm` masks and sprites are stored already rotated to portrait. Regenerate the pack. Text A8 masks: rotate once in `bake_scene_text_slot` (offline-of-frame, once per rebake) — or author the text layout portrait directly.

- [ ] **Step 4: Host-side screenshot rotation**

`tools/eva-screenshot.py` receives the raw 480×800 portrait buffer now. Add a `--rotate 90` (or as needed) so saved JPEGs match the previous landscape orientation for apples-to-apples baseline diff. Note in `CLAUDE.md §4` that the screenshot protocol now emits portrait.

- [ ] **Step 5: Build, flash, measure**

All scenes, day + night. Expected: `ppa_rot` **gone** (0/absent from the log line); storm work drops ~15 ms; tick rises to the target band. Confirm tick jitter did **not** grow (rotation was masking some timing — watch `jitter=min..max`).

- [ ] **Step 6: Full screenshot regression**

Every scene (day + night), host-rotated, vs baseline. This step has the highest regression risk — inspect clock position, sun trajectory (left→right must still read correctly in portrait), cloud bands, text. Fix coordinate mapping bugs before proceeding.

- [ ] **Step 7: Snapshot**

Snapshot as `phase7_eva_weather-snapshot-2026-07-04-portrait-native`. Update `CLAUDE.md §1` (render pipeline) and §4 (screenshot orientation) — these are load-bearing notes. Update baseline file.

---

## Task 5: Lit cloud-deck variant (lightning without a separate pass)

**Why:** A strike currently adds a regional-flash pass over the clouds (`composite_lightning_on_render`, `:4920`; `li` spikes 0–5 ms). Bake a second, internally-lit version of each storm mask offline; a strike frame swaps variant/tint at the same blend cost, plus the existing bolt sprite. No extra full-frame pass.

**Files:**
- Modify: `tools/cloudgen/genpool.py`/`cloudgen.py` (lit storm variant), `main/eva_weather_canvas.c` (strike = variant swap, drop regional flash pass)
- Test: `tools/cloudgen/test_cloudgen.py`

- [ ] **Step 1: Host test — lit variant is brighter than base**

```python
def test_lit_storm_variant_brighter():
    import cloudgen
    D = cloudgen.density_field(cloudgen.PROFILES_STORM["LOW"])
    base_l, _, _ = cloudgen.decompose(D, cloudgen.PROFILES_STORM["LOW"])
    lit_l, _, _ = cloudgen.decompose_lit(D, cloudgen.PROFILES_STORM["LOW"])
    assert lit_l.mean() > base_l.mean() + 15, "lit variant not visibly brighter"
```

- [ ] **Step 2: Run — fail**

Run: `python -m pytest test_cloudgen.py::test_lit_storm_variant_brighter -v` → FAIL (`decompose_lit` undefined).

- [ ] **Step 3: Implement `decompose_lit`**

In `cloudgen.py`, add a variant of `decompose` that pushes more mass into the light plane (internal illumination):

```python
def decompose_lit(D, profile):
    light, shadow, core = decompose(D, profile)
    import numpy as np
    lit = np.clip(light.astype(np.int16) + (D * 90).astype(np.int16), 0, 255).astype(np.uint8)
    return lit, shadow, core
```

- [ ] **Step 4: Run — pass.** `python -m pytest test_cloudgen.py::test_lit_storm_variant_brighter -v` → PASS.

- [ ] **Step 5: Emit lit variants + regenerate**

In `genpool.py`, for the storm (and storm-merged) pool, emit a parallel lit variant set under a distinguishing subtype/variant index. Regenerate pack; `preview.py` shows the lit deck glowing.

- [ ] **Step 6: Device — strike swaps to lit variant**

In `update_lightning`/`composite_lightning_on_render` (`:4893,4920`), on the flash frames set the active storm variant to the lit one (and/or apply the lit tint in the blend) instead of running the regional-flash overpaint. Keep the bolt sprite. Remove the separate full-frame flash composite.

- [ ] **Step 7: Build, flash, measure**

`weatherdebug thunderstorm`, force strikes with the CDC `lightning` command. Expected: strike frames no longer spike `li`; work stays flat through a strike. Screenshot a strike frame — the deck must illuminate from within (better than the old overpaint, per design).

- [ ] **Step 8: Snapshot.** `phase7_eva_weather-snapshot-2026-07-04-lit-deck`.

---

## Task 6: Offline sky keyframe columns (rebake jitter)

**Why:** `fill_sky_rows` (`:1304`) computes gradient + radial warm-glow + Bayer dither on CPU (~100–160 ms full fill, amortized in 64-row slices `:249`). Replace the math with 4 baked 480px RGB565 keyframe columns per kind (~46 KB total); rebake becomes row tiling/interpolation, shrinking slices and jitter.

**Files:**
- Create: `tools/cloudgen/skygen.py`
- Modify: `tools/cloudgen/genpool.py` (emit sky columns), `main/eva_sky_palette.h`/`eva_weather_canvas.c` (`fill_sky_rows` reads baked columns), `main/eva_clp_toc.h` (SKY type)
- Test: `tools/cloudgen/test_cloudgen.py`

- [ ] **Step 1: Host test — sky column matches the C palette**

`eva_sky_palette.h` is shared with the host tool `tools/skypreview.c` (`--dump-json`, `CLAUDE.md §6`). Test that `skygen.py` reproduces the same top/bottom colors the C palette dumps for a given kind+daypart:

```python
def test_skygen_matches_palette_dump():
    import skygen, json, subprocess
    dump = json.loads(subprocess.check_output(["./skypreview", "--dump-json"], cwd=".."))
    col = skygen.gen_column("thunderstorm", "night")
    # top row ≈ palette top, bottom row ≈ palette bottom (within RGB565 quant)
    assert skygen.close(col[0], dump["thunderstorm"]["night"]["top"], tol=8)
    assert skygen.close(col[-1], dump["thunderstorm"]["night"]["bottom"], tol=8)
```

- [ ] **Step 2: Run — fail** (`skygen` missing). `python -m pytest test_cloudgen.py::test_skygen_matches_palette_dump -v` → FAIL.

- [ ] **Step 3: Implement `skygen.py`**

Create `tools/cloudgen/skygen.py` generating a 480-tall RGB565 column per (kind, daypart keyframe) by the same gamma-2.2 vertical lerp `fill_sky_rows` uses (`:1310,1325-1331`), reading top/bottom from the palette JSON dump. Provide `gen_column(kind, daypart)` and `close(a, b, tol)`.

- [ ] **Step 4: Run — pass.** `python -m pytest test_cloudgen.py::test_skygen_matches_palette_dump -v` → PASS.

- [ ] **Step 5: Emit sky columns + CLP type**

Add `#define EVA_CLP_TYPE_SKY 8` to `eva_clp_toc.h` (mirror in `genpool.py`); emit 4 daypart keyframe columns per kind. Regenerate pack.

- [ ] **Step 6: Device — `fill_sky_rows` tiles the baked column**

Rewrite `fill_sky_rows` (`:1304`) to: pick/interpolate between the two bracketing daypart keyframe columns for the current minute, then broadcast the column across the width with the existing Bayer dither (`eva_dither565`, `:1335`). The radial warm-glow stays CPU (it's sun-position dependent) but only on `use_glow` kinds — clear/partly. Storm/overcast (no glow) become a pure column broadcast → near-free.

- [ ] **Step 7: Build, flash, measure**

`weatherdebug thunderstorm` (precip → frequent rebake). Expected: bg-slice cost drops; `jitter=min..max` narrows further. Screenshot storm/clear-day sky vs baseline — gradient identical.

- [ ] **Step 8: Snapshot + final validation**

Snapshot as `phase7_eva_weather-snapshot-2026-07-04-sky-keyframes`. Then run the **whole-design DoD**: `weatherdebug thunderstorm` at night (pinned) must hold **≥30 Hz** with jitter within the 30 Hz band; `fog` ≥25 Hz; screenshot every scene (day + night) vs baseline — no visual regression. Record final `perf` numbers in the baseline file under "FINAL".

---

## Self-review checklist result

- **Spec coverage:** design steps 0/2/flash/3/4/1/5/6 → plan Tasks 0/2/2.5/3/4/1/5/6. All covered. The precip-text z-order decision (design §3 step 2) → Task 2 Step 8. The sunrise fix precondition (design §3 step 0) → Task 0. Repartition (design "Перерозбивка флешу") → Task 2.5.
- **Ordering:** matches design §2 (cheap→invasive): text cache → flash → storm merge → rain/fog → portrait → lit deck → sky keyframes.
- **Placeholders:** none — every code step has concrete code; diagnostic Task 0 is deliberately branch-on-observation (a fix whose exact site is unknown until measured), with all three candidate sites and their fixes spelled out.
- **Type/name consistency:** `CLOUD_POOL_STORM_MERGED` (=2), `EVA_CLP_TYPE_RAIN` (=6)/`FOG` (=7)/`SKY` (=8), `merge_strips`, `gen_rain_loop`, `gen_fog_band`, `decompose_lit`, `gen_column`, `scene_text_can_cache`, `s_scene_base`/`s_scene_base_dirty`/`s_scene_text_done_this_frame` — used consistently across tasks.
- **Known unknowns flagged for the executor:** exact `entries.append` tuple arity (Task 3 Step 6 / Task 4 Step 6) and `cloudgen.density_field` naming must be confirmed against the live `genpool.py`/`cloudgen.py` before editing — the plan says so inline rather than guessing a signature.
