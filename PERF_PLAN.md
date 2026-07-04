# phase7_eva_weather performance plan

## Latest implementation snapshot (2026-05-25 21:20, build-verified)

Implemented in code and build-verified:

- Phase 1 core refactor: weather render moved from `lv_timer` to a dedicated
  FreeRTOS task (`canvas_render_task`) with double render buffers and async
  LVGL presentation.
- Phase 2 occlusion text path: clock, temperature, and description text now
  render directly into the weather canvas before cloud composition, so clouds
  naturally overlap these UI elements.
- Phase 3 visual pass: day sun trajectory is now left-horizon -> zenith ->
  right-horizon; added depth-layered 3D cloud billboards with horizon respawn,
  forward motion, and wind lateral drift.
- Phase 4 adaptive load control: 3D cloud active count now scales down when
  frames are over budget and scales back up when headroom returns.
- Phase 5 diagnostics: new CDC `perf` command reports tick/work breakdown and
  current 3D cloud budget.

Example expected CDC output:

```text
perf:
  tick: 33 Hz
  work: 15800 us
  breakdown_us: bg=3400 cl=5200 pa=2600 li=400 lvgl=4100 vsync=17300
  clouds_3d: 14/18
```

Hardware performance numbers for this snapshot still need to be captured on
device (post-boot 10s warmup, 8s window) for:
`clear-day`, `partly-cloudy-day`, `cloudy`, `thunderstorm`.

## Latest measurement (2026-05-25 18:30, "thunderstorm")

After P0.1–P0.3, P1.4 (bg-cache covers clouds), and the new upscale-skip:

```
thunderstorm tick=14-22 Hz, work_us=9-12 ms
  bg=2.6 ms  cl=1.0 ms (cached) / 1.9 ms (cold)
  pa=1.8 ms  li=0-1.3 ms  up=4 ms (every other frame)
```

- 3 Hz → **20 Hz median**, peaks 24 Hz. Target 30 Hz still missed.
- Cloud composition cached in `s_bg_buf` over 8-frame TTL: 12.5 ms → 1.0 ms
  on cache-hit ticks. Storm clouds drift one bake-step (~100 ms) per refresh.
- Upscale 7 ms → 4 ms average via skip-every-other-frame; particles still
  redraw on the low-res buffer each tick so motion stays smooth.
- `clock_label` now only invalidated when the minute string actually
  changes, and the rim-light/inner-shadow/drop-shadow labels are permanently
  HIDDEN. Glyph rasterisation cost effectively zero on the LVGL refresh
  task except once per minute.

The remaining gap to 30 Hz is **LVGL refresh-task throughput** — flush of
the 800×480 RGB565 panel buffer through MIPI DPI is ~30 ms / frame on its
own, so the canvas tick gets a slot at best every 60 ms. The next wins
require touching LVGL display config, not the canvas pipeline.

---

## Original baseline (2026-05-25 17:56, "cloudy" kind)

CDC log line, captured live:

```
cloudy tick=3 Hz, 0/80 particles, work_us=23926
```

- `tick=3 Hz` — actual rate at which `canvas_tick` (the procedural weather
  render) fires. Configured target is `TIMER_MS = FIB_13 = 13 ms` → ~77 Hz.
  Real rate is ~25× slower than target. **This is the headline bug.**
- `work_us=24 ms` — time spent inside `render_weather` + `upscale_render_to_display`
  per tick. By itself this would allow 40+ Hz; the remainder of the budget
  (~310 ms) is being eaten outside our canvas pipeline.
- `0/80 particles` — particle count is low because the kind switched at the
  exact moment of the log (transition between rain and cloudy). For "rain"
  the user observed roughly 2 fps, which is consistent with `tick≈3 Hz` plus
  the extra cost of 80–512 particle draws and ~40 line rasters per tick.

The user-visible symptom is "2 fps в дощі". The root cause is **not** the
particle loop — it's that the LVGL refresh task is saturated rendering the
clock label stack at 144 px every frame, which throttles `canvas_tick`
because it shares the same task.

---

## The cost map

### 1. Clock label stack (biggest single cost)

We have **seven copies** of the clock text rendered every LVGL refresh:

- `s_clock_shadow_labels[0..3]` — four blurred drop-shadow copies (now
  HIDDEN; cost = 0).
- `s_clock_light_label` — additive rim light.
- `s_clock_label` — the main white glyphs.
- `s_clock_inner_shadow_label` — multiply inner shadow.

Font is `eva_font_clock_144_extralight` / `eva_font_clock_144` /
`eva_font_clock_144_extrabold`, all at 144 px bpp=4. Glyph bitmaps are
1.5–2 KB each. With `transform_scale` up to 390/256 (≈1.5×) LVGL rebuilds
the scaled glyph cache every time the scale changes (every 1 s when the
clock ticks because `apply_clock_solar_style` recomputes `scale`).

Even with the 4 shadow layers hidden, three large labels × five `"H:M"`
glyphs each = ~15 glyph blits per refresh, all with blend modes set
(ADDITIVE for light, MULTIPLY for inner shadow). LVGL's SW renderer is the
only path here — no PPA acceleration for glyph blends — so each glyph
runs through the C blender on PSRAM at ~30 ns/pixel.

**Estimated saving:** going from 7→3 labels (done) cuts glyph blits by
~57 %. Going from 3→1 (drop both light and inner_shadow, paint everything
into the canvas instead) would buy another ~67 %.

### 2. PSRAM read-back of `s_buf`

`render_weather` writes pixel-by-pixel into `s_buf` (PSRAM, 400×240 RGB565
= 192 KB). Every blend operation does:

```c
uint16_t bg = s_buf[idx];     /* PSRAM read */
uint16_t blended = blend_rgb565(bg, fg, alpha);
s_buf[idx] = blended;          /* PSRAM write */
```

PSRAM reads are ~7× slower than internal SRAM. For 1000 raindrop pixels
that's ~70 µs just for the reads. The `blend_px` function is hit from
`draw_line` (thickness×length pixels each) and `draw_filled_circle`
(πr² pixels each).

**Estimated saving:** move `s_buf` to internal SRAM. 192 KB exceeds the
free internal heap, but we could keep just *one band* (say 32 rows = 25 KB)
in IRAM and DMA-flush bands as we render. Or: switch the inner loop to
batched 32-bit access where alpha ≥ 240 (opaque short-circuit) so PSRAM
reads are skipped.

### 3. Particle loop overhead in dense weather

`update_and_draw_particles` walks up to 512 particles every tick. For rain
each particle calls `draw_line` with thickness 0–1 over ~10 px, so
~10–30 `blend_px` calls per particle. 512 particles × 20 px = ~10000 blend
ops per tick. At 30 ns/pixel that's ~300 µs — not the bottleneck on its
own, but it stacks on top of the cloud + sun compositing.

### 4. Sun halo overpaint

`draw_sun_or_moon` draws four concentric filled circles of radius
`r×5, r×3, r×1.5, r`. With `r=42` (just reduced from 60) the outer halo
is 210 px radius = ~138 000 pixels. Even at low alpha, each pixel reads
`s_buf`, blends, writes back. That's ~4 ms per frame just for the sun
outer flare. The background-cache (`s_bg_buf`) hides this most of the
time because the sky/sun is only redrawn every `background_hold_frames(kind)`
ticks — but for `rain`/storm kinds that hold drops aggressively.

### 5. PPA upscale pipeline

`upscale_render_to_display` 2×-upscales `s_buf` (400×240) into the LVGL
canvas buffer `s_display_buf` (800×480). The PPA engine handles this in
~1.5 ms hardware time, *blocking*. That's fine. The CPU fallback path
runs at ~8 ms and only fires if PPA disabled. Not currently a problem.

### 6. LVGL render task contention

The clock labels live on the same LVGL screen as `s_canvas`. When we call
`lv_obj_invalidate(s_canvas)` from `canvas_tick`, LVGL's refresh task
schedules a redraw of the canvas area. If clock labels overlap that area
(they do — they're in the screen centre), the labels get redrawn too.
The LVGL refresh task is single-threaded against canvas_tick, so glyph
rendering blocks particle rendering.

**This is the actual cause of `tick=3 Hz`.** `lv_timer_handler` runs the
canvas tick only if the refresh task is idle. When the clock is showing,
the refresh task is rarely idle.

---

## Priority-ordered work

### P0 — fix the 2 fps right now

1. **HIDDEN flag on the 4 unused drop-shadow layers** (already done in the
   current commit). Saves four 144-px glyph blits per refresh.
2. **Stop invalidating clock labels every tick.** `apply_clock_solar_style`
   currently calls `clock_align_layer` (which does `lv_obj_align`) on
   every clock tick (~1 s) for *every* surviving layer. Each alignment
   change invalidates the label area. Cache the previous `light_x/inner_x`
   values and only realign when they actually changed.
3. **Drop transform_scale rebuilds.** `scale` changes every clock tick by
   1–2 units because `eased` is a continuous function. Quantise scale to
   8-step grid (`scale & ~7`) so the glyph cache stays warm.

Expected outcome: ≥30 fps in non-storm weather, ≥15 fps in rain.

### P1 — structural

4. **Bake rim-light/inner-shadow into the canvas instead of LVGL labels.**
   Render the clock digits directly into `s_buf` (or into a smaller A8
   mask) once per second and composite the rim/shadow there. Output to
   LVGL via a single image, not three labels. Saves the entire glyph
   blending budget every frame.
5. **Internal-SRAM band for `s_buf`.** Allocate a 32-row IRAM stripe
   (~25 KB), render bands one at a time into it, PPA-DMA each band into
   `s_buf` PSRAM. Cuts the PSRAM read-modify-write loop's wall time by
   ~5× on hot bands.
6. **Tag opaque blends and short-circuit `blend_px`.** Most particles and
   the sun disc paint with `alpha ≥ 240`. For those, skip the PSRAM read
   and just write the foreground colour. Single-line change in `blend_px`.

### P2 — long-tail polish

7. **Adaptive particle cap.** `ensure_particle_count` already adjusts
   `s_target` against the budget. Make the lower bound tighter for rain
   (≤256 instead of 512) — rain readability doesn't suffer.
8. **Cache the sun's outer flare into `s_bg_buf` proactively.** Today the
   bg cache flushes on cloud-cover change. Add a 256-tile cache for the
   sun's outer halo keyed on `(sun_x, sun_y, r, vis_disc_quant)` so we
   redraw the 210 px disc only when those change.
9. **Move `composite_lightning_on_render` behind a "lightning active"
   flag.** Currently runs every frame even with no flash queued.

---

## How to measure

`canvas_tick` already logs `tick=N Hz, work_us=W` every ~55 frames.
Capture lines from `/dev/cu.usbmodem1234561` over 8 s and grep
`'tick='`. Three reference scenarios:

- `clear-day` — best case, no particles, lightest cloud composite.
- `cloudy` — what we measured above (3 Hz baseline).
- `rain` — worst case, max particles + max cloud composite.

Goal table (post-P0, then post-P1):

| kind     | tick (Hz) baseline | post-P0 | post-P1 |
|----------|-------------------:|--------:|--------:|
| clear-day| 3                  | ≥40     | ≥60     |
| cloudy   | 3                  | ≥30     | ≥50     |
| rain     | ~2                 | ≥15     | ≥30     |

Take screenshots in each kind via `tools/eva-screenshot.py` after each
optimisation pass and diff the resulting JPEGs for visual regressions
(rim-light and sun disc positions should not move).

---

## What we won't optimise

- Cloud strip bake (`bake_strip_*`): runs once per variant rebake (~30 s
  intervals), not on the hot path.
- JPEG screenshot encode: lazy, user-triggered, 30 ms is fine.
- Wi-Fi/NTP: separate FreeRTOS tasks, do not contend.
- Sun position math: 4 floats × 1 tick × float ops. Negligible.

---

## Open questions

- Does LVGL v9 support per-area refresh skipping for hidden labels? If
  yes, we may need only flag, not delete. If no, P1.4 becomes the real
  fix.
- Can PPA do glyph blending? If so we could keep three labels but route
  them through hardware. Worth a 1-h investigation.
- What's the actual cost of `restore_scene_depth_order` calling
  `lv_obj_move_foreground` on every clock tick? Each `move_foreground`
  is a list re-link + invalidate. May need caching.
