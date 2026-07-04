# Phase 3 Cloud Architecture Diagrams

## System Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                    EVA Weather Canvas (Phase 3)                 │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                 render_weather() per frame               │  │
│  │                                                          │  │
│  │  1. fill_gradient() → sky background                    │  │
│  │  2. draw_sun_or_moon() → sun position                   │  │
│  │  3. draw_clouds_3d() → cloud rendering ← PHASE 3        │  │
│  │     ├─ Physics update: position & scale each cloud     │  │
│  │     ├─ Depth sort: painter's algorithm               │  │
│  │     └─ Render: blend each sprite into framebuffer     │  │
│  │  4. update_and_draw_particles() → rain/snow/etc       │  │
│  │  5. composite_lightning_on_render() → lightning       │  │
│  │                                                          │  │
│  └──────────────────────────────────────────────────────────┘  │
│                           ↓                                      │
│                   s_buf (RGB565 framebuffer)                     │
│                   800×480 × 2 bytes = 768 KB                     │
│                           ↓                                      │
│              LVGL overlay flush (text, fps counter)              │
│                           ↓                                      │
│            Panel framebuffer → MIPI display out                  │
└─────────────────────────────────────────────────────────────────┘
```

## Cloud Sprite Atlas Initialization

```
Boot Sequence:
│
├─ eva_weather_canvas_init()
│  │
│  └─ render_task starts (calls render_weather per frame)
│     │
│     └─ cloud3d_init_if_needed() [first call]
│        │
│        └─ cloud_sprite_atlas_init_if_needed() [first call]
│           │
│           ├─ Check: already initialized? → return if yes
│           │
│           ├─ for i = 0..7:
│           │  │
│           │  ├─ esp_malloc(20000 bytes) → allocate A8 buffer
│           │  │
│           │  └─ generate_cloud_sprite(i) 
│           │     │
│           │     ├─ Init seeded RNG: seed = 0x12345678 + i*0x6c5ce7
│           │     │
│           │     ├─ for bump = 0..3:
│           │     │  │
│           │     │  ├─ Random position: (cx_norm, cy_norm)
│           │     │  │
│           │     │  ├─ Random radius: r_norm
│           │     │  │
│           │     │  └─ Draw anti-aliased circle:
│           │     │     └─ for all pixels near circle:
│           │     │        ├─ dist = sqrt(dx² + dy²)
│           │     │        ├─ rel_dist = dist / r
│           │     │        ├─ if rel_dist < 0.95: alpha = 255
│           │     │        ├─ if 0.95 ≤ rel_dist < 1.05: 
│           │     │        │   alpha = 255 * (1.05 - rel_dist) * 10
│           │     │        └─ a8[py*200+px] = max(a8[...], alpha)
│           │     │
│           │     └─ Set base tint color (RGB values)
│           │
│           ├─ s_cloud_atlas_inited = true
│           │
│           └─ Log: "Cloud sprite atlas ready (160 KB allocated)"
│
├─ For each frame:
│  │
│  ├─ draw_clouds_3d(dt)
│  │  │
│  │  ├─ [skip atlas init — already done]
│  │  │
│  │  └─ [continue with physics & rendering]
│  │
│  └─ render cost: 3–6 ms
│
└─ Device runs (until power off)
   └─ Sprite atlas never freed (static lifetime)
```

## Per-Frame Cloud Rendering

```
draw_clouds_3d(float dt):

┌─ Physics Update Loop ─────────────────────────────────┐
│                                                       │
│ for i = 0..23 (CLOUD_3D_MAX):                        │
│   c = &s_clouds3d[i]                                 │
│                                                       │
│   Approach motion:                                    │
│   c->y += dt * (0.15 + c->scale * 0.10)             │
│                                                       │
│   Scale from depth:                                  │
│   c->scale = 0.3 + c->y * 1.7                       │
│                                                       │
│   Wind drift:                                         │
│   c->x += dt * (c->vx + wind_bias * 0.0007)         │
│                                                       │
│   Wrap around:                                        │
│   if c->x < -0.25: c->x += 1.5                      │
│   if c->x > 1.25: c->x -= 1.5                       │
│                                                       │
│   Respawn at horizon:                               │
│   if c->y > 1.15:                                    │
│     cloud3d_respawn(c, true)                         │
│                                                       │
└───────────────────────────────────────────────────────┘
                        ↓
┌─ Depth Sort (Painter's Algorithm) ────────────────────┐
│                                                       │
│ order[24] = insertion_sort by y-coordinate           │
│   (low y = horizon = back, high y = viewer = front)   │
│                                                       │
│ Result: order[0] points to most distant cloud        │
│         order[23] points to nearest cloud            │
│                                                       │
└───────────────────────────────────────────────────────┘
                        ↓
┌─ Render Loop (sorted order) ──────────────────────────┐
│                                                       │
│ for k = 0..23:                                        │
│   c = &s_clouds3d[order[k]]  ← sorted by depth       │
│   y = c->y                                            │
│                                                       │
│   Frustum cull:                                       │
│   if y < -0.12 || y > 1.20: continue                 │
│                                                       │
│   Screen projection:                                  │
│   cx = c->x * 800                                     │
│   cy = (0.18 + y * 0.70) * 480                       │
│                                                       │
│   Alpha modulation:                                   │
│   fade = clamp_u8(c->alpha * (0.25 + 0.75 * y))     │
│   if fade < 8: continue                              │
│                                                       │
│   Get sprite:                                         │
│   sprite = &s_cloud_atlas[c->shape_id % 8]           │
│                                                       │
│   Blend to framebuffer:                              │
│   blend_cloud_sprite_cpu(s_buf, 800, 480,            │
│       sprite, cx, cy, c->scale, fade, base_col)      │
│                                                       │
│   cost: ~200 μs per cloud (scale dependent)          │
│                                                       │
└───────────────────────────────────────────────────────┘
```

## Sprite Blending Pipeline

```
blend_cloud_sprite_cpu():

Input:
  ├─ dst: RGB565 framebuffer (800×480)
  ├─ sprite: A8 mask (200×100)
  ├─ (cx, cy): screen center position
  ├─ scale: 0.3–2.0
  ├─ alpha: 0–255
  └─ tint_col: RGB565 color

Process:
  1. Compute scaled dimensions:
     sw = 200 * scale
     sh = 100 * scale

  2. Compute origin:
     sx0 = cx - sw/2
     sy0 = cy - sh/2

  3. For each pixel in scaled sprite:
     ├─ Sample source (nearest-neighbor):
     │  src_x = sx / scale
     │  src_y = sy / scale
     │  sprite_alpha = sprite->a8[src_y * 200 + src_x]
     │
     ├─ Modulate alpha:
     │  final_alpha = (sprite_alpha * alpha) >> 8
     │
     └─ Blend to destination:
        px = sx0 + sx
        py = sy0 + sy
        if bounds_check(px, py, 800, 480):
          dst[py*800 + px] = blend565(dst[...], tint_col, final_alpha)

Cost: ~5–10 cycles per pixel
      scale 0.3: 30×15 px = 450 pixels ≈ 9 μs
      scale 1.0: 200×100 px = 20,000 pixels ≈ 200 μs
      scale 2.0: 400×200 px = 80,000 pixels ≈ 800 μs
```

## Memory Layout

```
ESP32-P4 PSRAM (16 MB total):

0x00000000 ┌────────────────────────────────┐
           │ Other allocations              │ ← ~5-10 MB (display buffers, etc.)
           │                                │
           ├────────────────────────────────┤
s_buf[0]   │ Render buffer 0 (RGB565)       │ 800×480×2 = 768 KB
           │ 800×480 pixels                 │
           ├────────────────────────────────┤
s_buf[1]   │ Render buffer 1 (RGB565)       │ 768 KB (double-buffer)
           │ 800×480 pixels                 │
           ├────────────────────────────────┤
s_bg_buf   │ Background buffer (RGB565)     │ 768 KB
           │ 800×480 pixels                 │
           ├────────────────────────────────┤
s_glyph    │ Glyph buffer (A8)              │ 25.6 KB
           │ 160×160 pixels                 │
           ├────────────────────────────────┤
           │ Cloud strip buffers (A8)       │ ~1 MB (old code, still present)
           │ 3 layers × 2 variants × ...    │
           ├────────────────────────────────┤
           │ ★ CLOUD SPRITE ATLAS (NEW) ★  │
atlas[0]   │ Sprite 0 (A8)                  │ 20 KB
atlas[1]   │ Sprite 1 (A8)                  │ 20 KB
atlas[2]   │ Sprite 2 (A8)                  │ 20 KB
atlas[3]   │ Sprite 3 (A8)                  │ 20 KB
atlas[4]   │ Sprite 4 (A8)                  │ 20 KB
atlas[5]   │ Sprite 5 (A8)                  │ 20 KB
atlas[6]   │ Sprite 6 (A8)                  │ 20 KB
atlas[7]   │ Sprite 7 (A8)                  │ 20 KB
           │ ───────────────────────────── │ 160 KB total
           │                                │
           │ (free space)                   │ ~8-10 MB
           │                                │
0xFFFFFFFF └────────────────────────────────┘

Key: ★ = NEW in Phase 3
     Each sprite = 200×100 × 1 byte (A8 format)
     Total = 8 × 20000 = 160000 bytes = 160 KB
```

## Data Structure Layout

```c
/* Per-cloud data (repeated 24 times) */
typedef struct {
    float x;            /* 4 bytes: 0..1 left-right */
    float y;            /* 4 bytes: 0..1 horizon-viewer */
    float scale;        /* 4 bytes: 0.3..2.0 */
    float vx;           /* 4 bytes: wind drift -0.25..0.25 */
    uint8_t alpha;      /* 1 byte: base opacity */
    uint8_t shape_id;   /* 1 byte: 0..7 sprite index ← NEW */
    uint8_t seed;       /* 1 byte: wobble seed */
    /* padding:   1 byte */
} cloud3d_t;           /* 24 bytes total per cloud */

/* Sprite atlas (allocated once) */
typedef struct {
    uint8_t *a8_data;   /* pointer: 8 bytes → malloc'd 20000 bytes */
    uint8_t base_r;     /* 1 byte */
    uint8_t base_g;     /* 1 byte */
    uint8_t base_b;     /* 1 byte */
    /* padding: 5 bytes */
} cloud_sprite_t;       /* 16 bytes per sprite struct */

/* Array: 8 sprites × 16 bytes = 128 bytes stack
   + 8 × 20000 bytes PSRAM = 160000 bytes */
```

## Phase 3 vs. Previous Architecture

```
BEFORE (Placeholder Circles):
┌──────────────────────┐
│ Cloud particle (x,y) │
│ (24 max)             │
└──────┬───────────────┘
       │
       └─→ for each cloud:
           ├─ cx = x * 800
           ├─ cy = y * 480
           ├─ r = 12 + scale * 32
           │
           └─ Draw 3 circles directly:
              ├─ draw_filled_circle(cx-r/2, cy, r, col, fade)
              ├─ draw_filled_circle(cx+r/3, cy-r/5, r*0.82, col, fade)
              └─ draw_filled_circle(cx+r/8, cy+r/3, r*0.76, col, fade*0.65)

Issues:
- Always 3 circles per cloud (repetitive)
- No visual variety (all clouds identical)
- No texture/realism
- Geometric look (circles ≠ clouds)


AFTER (Sprite Rendering):
┌────────────────────────────────┐
│ Cloud particle (x,y,shape_id) │
│ (24 max)                        │
└──────┬─────────────────────────┘
       │
       ├─→ Atlas init [once at boot]:
       │   └─ generate 8 sprites (procedural)
       │      └─ Store as A8 masks in PSRAM
       │
       └─→ for each cloud [per frame]:
           ├─ Physics: update (x,y,scale)
           ├─ Projection: cx,cy = screen position
           ├─ Alpha: fade = alpha * (0.25 + 0.75*y)
           │
           └─ Blend sprite to framebuffer:
              ├─ sprite = atlas[shape_id]
              ├─ scale sprite: (200×100)*scale
              ├─ sample: sprite_alpha = sprite->a8[src_y*200 + src_x]
              ├─ modulate: final_alpha = sprite_alpha * fade >> 8
              └─ blend: dst[px] = blend565(dst, tint_col, final_alpha)

Improvements:
- 8 unique shapes (visual variety)
- Procedurally generated (deterministic but diverse)
- Realistic cloud outlines
- Strong 3D perspective effect
- Proper depth sorting
- Atmospheric fading
```

---

These diagrams show the complete architecture, data flow, and memory layout of Phase 3 implementation.
