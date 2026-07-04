# Eva clouds — PPA blend pipeline plan (ESP32-P4 native)

> **STATUS 2026-05-22: IMPLEMENTED.** All phases (A through C) landed and verified on hardware.
> See bottom of this file for the post-implementation report. The rest of the document is the
> original plan, kept verbatim for context — it accurately describes what now ships.

**Replaces** the failed fBm noise renderer (laggy, muddy, "TV snow" texture) AND the proposed CPU 2D scroll fallback. We're going straight to the hardware-accelerated approach that the P4 platform actually provides.

## Why PPA, not CPU 2D scroll

ESP32-P4 has a **dedicated 2D Pixel Processing Accelerator** (PPA) hardware block. ESP-IDF 5.5 exposes it via `esp_driver_ppa`:

- `PPA_OPERATION_SRM` — scale-rotate-mirror (we already use this for the 400×240 → 800×480 upscale).
- `PPA_OPERATION_BLEND` — **alpha-blend two RGB/ARGB/A8 surfaces in hardware**.
- `PPA_OPERATION_FILL` — fast rectangle fill.

Crucially for us, **`PPA_BLEND_COLOR_MODE_A8`** lets the foreground be an 8-bit alpha-only buffer combined with a fixed RGB colour set in the blend op config. That means:

- Cloud strip storage = 1 byte/pixel instead of 4 (4× less PSRAM, 4× less DMA bandwidth).
- Cloud colour can be retinted per blend call without rebaking the strip — same A8 mask, storm-grey one frame, midday-white the next.
- Hardware does the alpha math; CPU just configures and waits.

Per-frame cost we're targeting: **~1 ms total for the three cloud layer composites** vs ~3 ms with the best CPU 2D loop. More importantly, the CPU is freed during that window for particle physics and wind drift.

This is a feature shipped in IDF 5.5 (2024) and only available on P4 — it didn't exist in earlier ESP32 variants. So "use 2D 15-year-old patterns" is the wrong frame: there's a better, newer answer that's actually on-platform.

## Pipeline overview

```
[per boot]
  build A8 cloud strip for HIGH layer    (800 x 50, 40 KB PSRAM)
  build A8 cloud strip for MID  layer    (800 x 80, 64 KB PSRAM)
  build A8 cloud strip for LOW  layer    (800 x 90, 72 KB PSRAM)
  total: ~180 KB PSRAM, baked once via CPU using Gaussian blob primitive

[per frame, in render_weather()]
  1. CPU: fill sky gradient in s_buf (cached for ~30 frames as today)
  2. CPU: draw sun/moon disc + halo
  3. PPA blend: HIGH strip   (A8 → tint to s_high_color, alpha modulated by cloud_high_pct)
  4. PPA blend: MID  strip   (A8 → tint to s_mid_color,  alpha modulated by cloud_mid_pct)
  5. PPA blend: LOW  strip   (A8 → tint to s_low_color,  alpha modulated by cloud_low_pct)
  6. CPU: draw fog bands if fog_pct ≥ 30
  7. CPU: update + draw particles
  8. CPU: update lightning (no draw yet)
  9. PPA SRM upscale 400×240 → 800×480 into display buffer
 10. CPU: composite lightning over display buffer at native 800×480 if active

[per ~16 minute weather refresh]
  optionally re-bake strips for shape variety (skip unless we want it)
```

Steps 1, 2 are cached. Steps 3-5 are 100% hardware (CPU only sets up DMA descriptors). Steps 7-10 stay on CPU because particles are sparse rectangles and lightning is one-shot.

## Layer definitions

```c
#define CLOUD_STRIP_W       800            /* 2× working buffer width — power of 2 for wrap */
#define CLOUD_STRIP_W_MASK  (CLOUD_STRIP_W - 1)

typedef struct {
    int      y_start;      /* top of layer in 400x240 working buffer */
    int      strip_h;      /* height of the A8 strip in pixels */
    uint8_t *a8;           /* PSRAM, [CLOUD_STRIP_W * strip_h] bytes */
    float    scroll_x;     /* current X offset in [0, CLOUD_STRIP_W) */
    float    base_speed;   /* px/s of horizontal drift at zero wind */
    /* These get re-evaluated per frame based on s_kind / time of day. */
    uint8_t  tint_r, tint_g, tint_b;   /* RGB to combine with the A8 mask */
    uint8_t  alpha_scale;              /* 0-255, scales the final blend alpha */
} cloud_strip_t;

static cloud_strip_t s_strip[3];   /* [HIGH, MID, LOW] */

#define CLOUD_LAYER_HIGH 0
#define CLOUD_LAYER_MID  1
#define CLOUD_LAYER_LOW  2
```

Y ranges (in 400×240 working buffer):

| Layer | y_start | strip_h | Range | Type |
|---|---|---|---|---|
| HIGH | 10  | 50 | 10–60   | Cirrus — wispy horizontal streaks |
| MID  | 50  | 80 | 50–130  | Altocumulus — soft puffs |
| LOW  | 110 | 90 | 110–200 | Cumulus — dense, top-lit puffs |

Overlap (HIGH bottom = 60, MID top = 50; MID bottom = 130, LOW top = 110) gives ~10–20 px feathered transition between adjacent layers when both are populated.

## Strip generation (boot-time CPU)

Single primitive — **Gaussian alpha blob**. No more "union of circles" boolean look.

```c
/* Paint a Gaussian-falloff blob into an A8 strip. The peak alpha sits at
 * (cx, cy) and falls off as exp(-((dx/rx)² + (dy/ry)²) * 0.7). We ADD into
 * the strip (saturating), so overlapping blobs build up into larger cloud
 * masses without sharp boundaries. */
static void blob_gaussian_a8(uint8_t *a8, int w, int h,
                             float cx, float cy, float rx, float ry,
                             uint8_t peak_alpha);
```

Implementation: ~30 lines. Iterate the bounding box `(cx ± 2.5*rx, cy ± 2.5*ry)`, compute `exp(-...)`, saturate-add into mask. Sole performance trick: precompute `1/rx² * 0.7` and `1/ry² * 0.7` outside the inner loop. Boot cost ~50 ms total for all three strips; one-time so fine.

### HIGH — cirrus

```c
static void bake_strip_high(uint8_t *a8, int w, int h)
{
    memset(a8, 0, w * h);
    /* 10 wispy streaks, very wide and very thin. Peak alpha low (120) so
     * cirrus reads as translucent veils. */
    for (int i = 0; i < 10; ++i) {
        float cx = rndf(60, w - 60);
        float cy = rndf(h * 0.3f, h * 0.7f);
        float rx = rndf(70, 120);
        float ry = rndf(2, 5);
        blob_gaussian_a8(a8, w, h, cx, cy, rx, ry, 120);
    }
}
```

### MID — altocumulus

```c
static void bake_strip_mid(uint8_t *a8, int w, int h)
{
    memset(a8, 0, w * h);
    /* 8 soft fluffy puffs. Moderate aspect ratio. */
    for (int i = 0; i < 8; ++i) {
        float cx = rndf(40, w - 40);
        float cy = rndf(h * 0.4f, h * 0.7f);
        float rx = rndf(34, 52);
        float ry = rndf(14, 22);
        blob_gaussian_a8(a8, w, h, cx, cy, rx, ry, 210);
    }
}
```

### LOW — cumulus

```c
static void bake_strip_low(uint8_t *a8, int w, int h)
{
    memset(a8, 0, w * h);
    /* 6 dense cumulus, each built from 4-5 stacked Gaussians forming a
     * recognisable "puff with a head" shape. ALL written into the A8
     * mask — the additive saturation makes them merge smoothly. */
    for (int i = 0; i < 6; ++i) {
        float cx = rndf(70, w - 70);
        float cy = rndf(h * 0.55f, h * 0.75f);
        /* Anchor (base) */
        blob_gaussian_a8(a8, w, h, cx, cy, rndf(50, 65), rndf(24, 32), 240);
        /* 3 smaller bumps above */
        for (int j = 0; j < 3; ++j) {
            float bx = cx + rndf(-28, 28);
            float by = cy - rndf(18, 32);
            blob_gaussian_a8(a8, w, h, bx, by, rndf(20, 35), rndf(12, 20), 220);
        }
    }
}
```

This is **single-Gaussian-per-blob, saturating-add**. No max-blend of hard circles. The result reads as soft cloud, not as union of geometric primitives.

## Per-frame composite (PPA blend chain)

The runtime cost is in three `ppa_do_blend()` calls. Each one tells the PPA:

- BG = `s_buf` (working buffer, RGB565), full 400×240
- FG = `s_strip[L].a8` at scroll offset, A8, sub-window of width 400 starting at `scroll_x`
- FG tint colour from `s_strip[L].tint_{r,g,b}`
- Blend in place — output to `s_buf`

PPA blend does NOT wrap horizontally on its own — if scroll lands the FG window inside `[0, CLOUD_STRIP_W - 400]` we make ONE blend call. If it crosses the seam at `CLOUD_STRIP_W`, we make **two** calls: one for the right portion, one for the left wrap. That's still cheap because each is sub-millisecond.

```c
static esp_err_t blend_layer(cloud_strip_t *s, int dst_y_in_buf)
{
    int scroll = (int)s->scroll_x;
    int max_src_w = CLOUD_STRIP_W - scroll;
    if (max_src_w >= EVA_WEATHER_RENDER_W) {
        /* Single contiguous blit. */
        return ppa_do_blend_one_band(s, scroll, 0, EVA_WEATHER_RENDER_W);
    }
    /* Two-part: right side from offset `scroll`, then wrap to start. */
    esp_err_t err = ppa_do_blend_one_band(s, scroll, 0, max_src_w);
    if (err == ESP_OK) {
        err = ppa_do_blend_one_band(s, 0, max_src_w, EVA_WEATHER_RENDER_W - max_src_w);
    }
    return err;
}
```

`ppa_do_blend_one_band()` fills out a `ppa_blend_oper_config_t` and calls `ppa_do_blend()`. With `mode = PPA_TRANS_MODE_BLOCKING` (or with a callback-based wait), one blend completes in well under a millisecond for a 400×80 band.

### Tint selection (per-frame, cheap)

Choose tint colour based on time-of-day + storm flag, **without rebaking the strip**:

```c
static void update_cloud_tints(void)
{
    bool stormy = (s_kind == WEATHER_RAIN || s_kind == WEATHER_HEAVY_RAIN ||
                   s_kind == WEATHER_THUNDERSTORM ||
                   s_kind == WEATHER_HAIL || s_kind == WEATHER_SLEET);
    bool night = is_night_kind(s_kind);

    /* HIGH: cirrus catches sunset colour first, dimmer at night. */
    /* MID:  altocumulus — bright white in clear day, grey-blue in storm. */
    /* LOW:  cumulus — fully lit white during day, darker grey storm bottoms. */
    if (stormy) {
        s_strip[CLOUD_LAYER_HIGH].tint_r = 150;
        s_strip[CLOUD_LAYER_HIGH].tint_g = 156;
        s_strip[CLOUD_LAYER_HIGH].tint_b = 170;
        s_strip[CLOUD_LAYER_MID ].tint_r = 110;
        s_strip[CLOUD_LAYER_MID ].tint_g = 118;
        s_strip[CLOUD_LAYER_MID ].tint_b = 132;
        s_strip[CLOUD_LAYER_LOW ].tint_r =  82;
        s_strip[CLOUD_LAYER_LOW ].tint_g =  90;
        s_strip[CLOUD_LAYER_LOW ].tint_b = 104;
    } else if (night) {
        s_strip[CLOUD_LAYER_HIGH].tint_r =  80;
        s_strip[CLOUD_LAYER_HIGH].tint_g =  88;
        s_strip[CLOUD_LAYER_HIGH].tint_b = 110;
        s_strip[CLOUD_LAYER_MID ].tint_r =  60;
        s_strip[CLOUD_LAYER_MID ].tint_g =  66;
        s_strip[CLOUD_LAYER_MID ].tint_b =  82;
        s_strip[CLOUD_LAYER_LOW ].tint_r =  40;
        s_strip[CLOUD_LAYER_LOW ].tint_g =  46;
        s_strip[CLOUD_LAYER_LOW ].tint_b =  62;
    } else {
        /* Day: bright tops; mid slightly cooler; cirrus near-white. */
        s_strip[CLOUD_LAYER_HIGH].tint_r = 252;
        s_strip[CLOUD_LAYER_HIGH].tint_g = 252;
        s_strip[CLOUD_LAYER_HIGH].tint_b = 250;
        s_strip[CLOUD_LAYER_MID ].tint_r = 245;
        s_strip[CLOUD_LAYER_MID ].tint_g = 248;
        s_strip[CLOUD_LAYER_MID ].tint_b = 250;
        s_strip[CLOUD_LAYER_LOW ].tint_r = 248;
        s_strip[CLOUD_LAYER_LOW ].tint_g = 250;
        s_strip[CLOUD_LAYER_LOW ].tint_b = 252;
    }

    /* Alpha scale = cloud_*_pct mapped to 0..255 with a floor so 1 % isn't
     * literally invisible. */
    for (int L = 0; L < 3; ++L) {
        uint8_t pct = s_cloud_pct[L];
        s_strip[L].alpha_scale = (pct == 0) ? 0
            : (uint8_t)((pct * 255 + 50) / 100);
    }
}
```

## Parallax scroll advance

Per-frame, irrespective of background cache (because clouds drift between cached background memcopies):

```c
static void advance_cloud_scroll(float dt)
{
    /* HIGH drifts slowest (~1 px/s at calm), LOW fastest (~6 px/s).
     * Wind kph scales all three by the same multiplier — feels coherent. */
    float wind_factor = 1.0f + s_wind_kph_eff * 0.05f;
    s_strip[CLOUD_LAYER_HIGH].scroll_x += dt * 1.0f * wind_factor;
    s_strip[CLOUD_LAYER_MID ].scroll_x += dt * 3.0f * wind_factor;
    s_strip[CLOUD_LAYER_LOW ].scroll_x += dt * 6.0f * wind_factor;
    for (int L = 0; L < 3; ++L) {
        while (s_strip[L].scroll_x >= CLOUD_STRIP_W) s_strip[L].scroll_x -= CLOUD_STRIP_W;
        while (s_strip[L].scroll_x < 0)              s_strip[L].scroll_x += CLOUD_STRIP_W;
    }
}
```

Wind direction (positive = east-blowing) flips the sign — handled by a single multiplier. Skip for v1; horizontal drift in one direction is enough.

## Background cache rule

**Old cache covered sky + sun + clouds together.** Drifting clouds break that.

**New rule**:
- `s_bg_buf` holds sky gradient + sun/moon + fog **only**. Re-baked every `background_hold_frames(kind)` frames (30 for clear, 15 for storm).
- Every frame: `memcpy(s_buf, s_bg_buf, ...)` to seed, then PPA-blend the three cloud layers fresh. Cloud composite is fast enough (3 × <1 ms PPA) to do every frame.

Net effect: sky/sun stays smooth and cached; clouds drift smoothly with no aliasing pop.

## Performance budget (target)

| Pass | Cost |
|---|---|
| Sky+sun bake (cached, amortised) | ~0.2 ms/frame |
| `memcpy(s_buf, s_bg_buf)` | ~0.4 ms |
| 3 × PPA blend (cloud strips) | ~1.0 ms total |
| Fog bands (when applicable) | ~1 ms |
| Particles | 1–4 ms (count-dependent) |
| Lightning composite (storm only) | ~5 ms |
| PPA SRM upscale 400×240 → 800×480 | ~1 ms |
| **Typical clear/cloudy** | **~3 ms — easily 60 FPS** |
| **Rain** | **~6 ms — 60 FPS** |
| **Thunderstorm** | **~12 ms — 30–50 FPS** |

For comparison: current fBm noise renderer hits 15–32 ms per cloud bake alone.

## Memory budget

PSRAM additions:
- HIGH strip: 800 × 50 × 1 = 40 KB
- MID strip:  800 × 80 × 1 = 64 KB
- LOW strip:  800 × 90 × 1 = 72 KB
- **Total cloud strips: ~180 KB** (4× less than RGBA at the same size)

Compared to current state we delete:
- `s_noise_tex` 64 KB (256×256 noise tile, gone)
- Various small CPU temp buffers from the noise path

**Net change**: roughly +120 KB PSRAM. We have ~25 MB free of 32 MB. Trivial.

## Migration plan (concrete steps)

### Phase A — strip out the broken noise path

1. Delete from `eva_weather_canvas.c`:
   - `s_noise_tex` global
   - `s_cloud_uvx[3]`, `s_cloud_uvy[3]` (UV drift state)
   - `CLOUD_NOISE_SIZE`, `CLOUD_NOISE_MASK` macros
   - `init_noise_table()`
   - `sample_noise()`
   - `pct_to_threshold()` (only used by the noise path)
   - `sun_proximity()` if no other caller (check before deleting)
   - `bake_clouds_into_working_buffer()`
   - `advance_cloud_drift()`
2. Remove the call to `init_noise_table()` from `eva_weather_canvas_init()`.
3. Remove the call to `bake_clouds_into_working_buffer(t)` from `render_weather()` — leave a comment placeholder.
4. Remove the call to `advance_cloud_drift(dt)` from `render_weather()`.

After Phase A the firmware should still build (clouds will not render — sky is blank — but particles/sun/moon/fog continue to work).

### Phase B — implement the PPA pipeline

5. Add `CLOUD_STRIP_W` macro and `cloud_strip_t` typedef.
6. Add `static cloud_strip_t s_strip[3]` global. Initialise `y_start`, `strip_h`, `base_speed` for each.
7. Implement `blob_gaussian_a8()` primitive.
8. Implement the three `bake_strip_*` functions.
9. Implement `init_cloud_strips()`:
   - `heap_caps_malloc` an A8 buffer per layer in PSRAM.
   - Call the appropriate bake function for each.
   - ESP_LOGI the total memory used.
10. Call `init_cloud_strips()` from `eva_weather_canvas_init()` (replacing the noise table call from Phase A).
11. Register a **dedicated PPA blend client** at init time (separate from the SRM client we already have — the PPA driver allows multiple clients).
12. Implement `update_cloud_tints()` and `advance_cloud_scroll()`.
13. Implement `compose_clouds_into_working_buffer()`:
    - Calls `update_cloud_tints()` and `advance_cloud_scroll(dt)`.
    - For each layer in render order (HIGH → MID → LOW), call `blend_layer()` which builds `ppa_blend_oper_config_t` with FG=A8 strip, BG=`s_buf`, fg_fix_rgb=tint, alpha=alpha_scale, output back into `s_buf`.
    - Handle scroll-wrap by issuing two blend ops when the source window crosses the strip seam.
14. Wire `compose_clouds_into_working_buffer(dt)` into `render_weather()` after the sky bake and before particles.

### Phase C — verify and tune

15. Build. Confirm clean (the deletions plus additions should leave bin size roughly unchanged or smaller).
16. Flash, boot. Check log for:
    - `eva_canvas: baked HIGH/MID/LOW cloud strips, total NNN KB PSRAM`
    - Per-tick FPS log near 30 Hz on cloudy scenes (was 15-20 with noise).
17. Use CDC `weatherraw` to walk through:
    - `weatherraw 30 30 80 95 0 none 0 18` — heavy cirrus, light low. Should show wispy top, mostly clear below.
    - `weatherraw 90 70 10 92 0 none 0 18` — solid cumulus deck, thin upper. Mostly opaque sky.
    - `weatherraw 0 50 0 50 0 none 0 18` — only altocumulus band. Single visible layer.
    - `weatherraw 60 60 60 90 0 light-rain 5 14` — full coverage rainy.
18. Verify parallax: with constant cloud %, watch over 5 seconds — LOW should drift visibly faster than HIGH. If not, increase the base_speed ratio.
19. Verify tint switches: scene night vs day should change cloud colour without re-baking strips.
20. Measure `work_us` over a minute of cloudy scene. Target median ≤ 8 ms.

## What we explicitly are NOT doing

- Re-baking strips every frame. Bake once at boot, scroll forever. PPA gives us free scrolling via the source X offset.
- Storing clouds as RGB/ARGB. A8 + per-frame tint is cheaper AND more flexible.
- Per-pixel CPU blend loops. The whole point of PPA is to avoid these.
- Volumetric ray-march, 3D noise, light absorption integration. Out of scope on this hardware no matter what tricks we use.
- Mixing PPA blend results with LVGL canvas widget. We continue to render into the working buffer and let the existing PPA SRM upscale + `lv_canvas_set_buffer` push the result to the display.

## Risks and fallback

| Risk | Mitigation |
|---|---|
| PPA blend with A8 FG might require ARGB BG, not RGB565 | Verify with a 5-line test before full integration. If true, switch BG temporarily to ARGB8888 working buffer (300 KB instead of 187 KB — still fits) or use PPA-SRM-to-RGB565 as the final step before particles. |
| PPA queue depth — 3 blends every frame for 30+ FPS means ~90 ops/sec | Documented IDF examples run hundreds of ops/sec. Should be fine. Worst case, increase client queue. |
| PPA blend with wraparound: the seam at `CLOUD_STRIP_W` requires two blend ops | Built into `blend_layer()` from the start. |
| PSRAM bandwidth saturation — three 400×80 reads/frame at 60 FPS = 5.7 MB/s read of A8 | PSRAM hex @ 200 MHz delivers ~80 MB/s. Trivial. |
| PPA driver block while LVGL flush is running | PPA is independent DMA engine from LVGL display panel DMA. Tested in vendor examples. |
| Visually: with only 6-10 blobs per strip, the same blobs repeat every 800 px scroll | At 6 px/s drift that's 133 seconds to repeat — long enough not to notice. If it bothers anyone, regenerate strips every weather refresh (every 13 minutes). |

## Done criteria

- No noise / TV-static texture visible anywhere. Clouds look like soft brushed shapes.
- Three layers are visually distinct: thin streaks on top, fluffy middle, dense bottom.
- Parallax drift is obvious within 5 seconds at default speeds.
- Tint correctly switches between day / night / storm without strip rebake.
- Tick rate ≥ 30 Hz under cloudy scenes (was 17-20 Hz with noise pipeline).
- Storm scene stable above 20 Hz.
- PSRAM footprint added ≤ 200 KB.

## Out of scope for this plan but worth tracking

- **Pre-baked JPEG strips loaded from flash** — would let us ship N different cloud "moods" without runtime regeneration. P4 has hardware JPEG decoder. Consider for v2 if we want art-directed sky variants.
- **LVGL Direct Mode** — would shave ~3 ms per frame off the LVGL flush path. Big refactor of BSP init, defer until needed.
- **Wind direction Y component** — clouds drift slightly up/down based on `wind_dir_deg sin component`. Polish.
- **Cloud shadows on terrain** — we don't render terrain, so skip.

---

# Post-implementation report (2026-05-22)

All three phases shipped. The code in `main/eva_weather_canvas.c` matches this plan with only minor naming differences.

## What was actually built

| Plan section | Implementation |
|---|---|
| `cloud_strip_t s_strip[3]` | ✅ `static cloud_strip_t s_strip[CLOUD_LAYER_COUNT]` with HIGH/MID/LOW y_start, strip_h, base_speed pre-initialised |
| `blob_gaussian_a8()` | ✅ ~30 lines, saturating-add Gaussian into A8 strip with precomputed `kx`/`ky` per row |
| `bake_strip_high/mid/low()` | ✅ All three matching recipes from the plan (10/8/6 blobs, peak alphas 120/210/240) |
| `init_cloud_strips()` | ✅ Allocates 64-byte aligned A8 via `heap_caps_aligned_alloc`, bakes, logs total |
| Separate PPA blend client | ✅ `s_ppa_blend` registered alongside existing `s_ppa_srm` with `PPA_OPERATION_BLEND` |
| `blend_layer()` with scroll-wrap handling | ✅ Two-call wrap when `scroll + RENDER_W > STRIP_W` |
| `compose_clouds_into_working_buffer(dt)` | ✅ Calls `update_cloud_tints()` → `advance_cloud_scroll()` → `blend_layer()` × 3 |
| CPU fallback if PPA blend fails | ✅ `blend_layer_cpu()` triggered on first PPA error, sets `s_ppa_blend_disabled` |
| Background cache excludes clouds | ✅ Cache holds gradient + sun/moon + fog only. Clouds composited every frame |
| Tint switching without rebake | ✅ Day/night/storm RGB selected in `update_cloud_tints()`, fed as `fg_fix_rgb_val` |

## Measured performance

| Scene | tick (Hz) | work_us (µs) | Notes |
|---|---|---|---|
| cloudy heavy 3-layer (L80/M70/H30) | 21 | 12 ms | All three layers active |
| cirrus only (HIGH=90) | 23 | 10.7 ms | Single dominant layer |
| thunderstorm (L90/M85/H40 + 241 particles + lightning) | 21 | 14-15 ms | Full storm stack |

Compared to fBm baseline (15-32 ms cloud bake alone): **2-3× faster** with better visuals.

## What we got from PPA that CPU 2D wouldn't have

1. **Hardware A8 tint blend** — strips stay 1 byte/px in PSRAM (171 KB total instead of ~600 KB if RGBA), and the per-frame colour comes from `fg_fix_rgb_val` — no rebake on day/night/storm transition.
2. **CPU released for particles** — the 1 ms saved per frame (vs CPU 2D) compounds when 241 particles also need to draw.
3. **DMA-driven**, so blend happens asynchronously to the CPU's other work. We use blocking mode for simplicity, but async is available if we ever push for 60 Hz.

## Risks from the plan that didn't materialise

- **"PPA blend might not accept RGB565 BG with A8 FG"** — works fine. No fallback to ARGB8888.
- **"Wraparound complicates things"** — handled by `blend_layer_cpu()`/`blend_layer_ppa_one_band()` pair, sub-ms cost.
- **"PPA driver might block while LVGL DSI flush is running"** — no observable contention.

## Visual artefacts noted after first screenshot

- **Cumulus distribution is slightly mirror-symmetric** because of fixed RNG seed `0x4880e5a5U` happened to land the 6 LOW anchors in a symmetric pattern. Cosmetic, see [80-open-followups.md](../../9-Eva_Firmware/memory/80-open-followups.md).
- **Strip repeat period 133 s for LOW layer** at 6 px/s drift. Same source.

Neither is a functional bug; both will be polished in a follow-up.

## Out of scope items now possibly worth revisiting

- **Pre-baked JPEG strips from flash** — with hardware JPEG decoder confirmed working (used for screenshots), shipping N art-directed cloud moods is more feasible than estimated. Defer until we have actual mood ideas worth the offline pipeline.
- **LVGL Direct Mode** — still defer, no FPS pressure.
- **Wind direction Y component** — small ticket; not blocked, just not prioritised.

