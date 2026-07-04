# Cloud Sprites Phase 3 — Quick Testing Guide

## What Changed

**Before:** Clouds rendered as 3 placeholder circles per cloud, no 3D effect.

**After:** Clouds rendered as pre-baked A8 sprite masks with full 3D perspective:
- Grow from small at horizon to large at bottom of screen
- 8 unique cloud shapes for visual variety
- Proper depth sorting (back-to-front rendering)
- Alpha fades out near horizon
- Wind affects horizontal drift

## How to Test

### 1. Verify Compilation
```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
idf.py build
```

Look for logs:
```
[eva_canvas] Allocating cloud sprite atlas (8 sprites × 20000 bytes)...
[eva_canvas] Cloud sprite atlas ready (160 KB allocated)
```

### 2. Flash to Device
```bash
idf.py flash monitor
```

Watch the startup sequence. You should see:
1. Boot messages
2. "Allocating cloud sprite atlas..." message
3. "Cloud sprite atlas ready..." message  
4. Display shows weather scene with new clouds

### 3. Visual Checks (Sanity Test)

On any weather scene, verify:

| Check | Expected | How to Verify |
|-------|----------|---------------|
| **Clouds visible** | Yes, multiple clouds on screen | Look at display, count at least 5 clouds |
| **Clouds vary in size** | Smaller at top, larger at bottom | Top clouds ≈ 60px, bottom clouds ≈ 300px |
| **Clouds move toward viewer** | Yes, smoothly rising | Watch for ~30-60 seconds, clouds rise from bottom to top |
| **Different shapes** | 8 unique outlines, not all identical | Watch shapes as they cycle through and respawn |
| **Depth sorting** | Near clouds cover distant clouds | Ensure no transparent near-clouds revealing clouds behind |
| **Fade at horizon** | Distant clouds fainter | Clouds at top are dimmer than clouds at bottom |
| **Horizontal drift** | Clouds drift left-right with wind | In windy weather, notice horizontal movement |

### 4. Weather Mode Tests

Test each weather type to verify tinting:

**clear-day:**
```
- Few clouds (5-8)
- Light color, near-white
- Blue sky visible behind
```

**cloudy:**
```
- Medium clouds (12-16)
- Light grey tint
- Sky partially clouded
```

**thunderstorm:**
```
- Many clouds (20-24, dense)
- Dark grey tint
- Nearly opaque cloud cover
```

**clear-night:**
```
- Few clouds (3-6)
- Cool blue tint (dim)
- Moonlight visible between clouds
```

### 5. Performance Check

Watch the FPS overlay (if enabled):

**Expected FPS by weather:**
- `clear-day`: 40-60 Hz (few clouds)
- `cloudy`: 25-35 Hz (medium clouds)
- `thunderstorm`: 20-28 Hz (many clouds)

If FPS is unexpectedly low:
```
[Eva_Canvas] tick=N Hz, work_us=W (bg=… cl=… pa=… li=… up=…)
                              ↑
Check "cl=" (cloud render cost) — should be 3-6 ms
```

### 6. Wind Bias Test (Advanced)

If weather provides wind data:

```
Wind = 0 kph:    Clouds drift slowly (Fibonacci vx values)
Wind = +30 kph:  Clouds drift to the right
Wind = -30 kph:  Clouds drift to the left
Wind = +60 kph:  Fast rightward drift
```

Clouds should remain left-to-right, wrap smoothly at edges.

### 7. Scale Verification Test

Examine a single cloud from horizon to viewer:

```
Y position  | Scale expected | Sprite size | Visual |
────────────┼────────────────┼─────────────┼─────────
y≈0.0       | 0.30           | 60×30 px    | Tiny
y≈0.3       | 0.81           | 162×81 px   | Small
y≈0.5       | 1.15           | 230×115 px  | Medium
y≈0.7       | 1.49           | 298×149 px  | Large
y≈0.9       | 1.83           | 366×183 px  | Very large
y≈1.0       | 2.00           | 400×200 px  | Max (clamped)
```

Pick one cloud shape and watch it grow as it rises. Measure at screen edges to verify scale consistency.

### 8. Sprite Variety Check

Count distinct cloud outline shapes over 2 minutes:

```
Expected: 8 unique shapes
Method:  Watch clouds as they spawn at horizon and grow
         Try to identify distinct "bump patterns"
         
Shape characteristics:
- Some have 2-3 bumps (more elongated)
- Some have 3-4 bumps (rounder)
- Bump sizes and positions vary per shape
```

If you see the same exact shape repeated immediately, something is wrong with the shape_id assignment.

### 9. Alpha Fade Test

Examine cloud at horizon:

```
At y≈0.05 (near horizon):
  Expected fade = alpha * (0.25 + 0.75 * 0.05)
               = alpha * 0.287
  Visual: Cloud at horizon is ~25-30% opacity

At y≈0.5 (middle):
  Expected fade = alpha * 0.625
  Visual: Cloud is medium-opaque

At y≈0.95 (near viewer):
  Expected fade = alpha * 0.96
  Visual: Cloud is nearly fully opaque
```

Check that clouds become progressively more opaque as they rise.

### 10. Tearing & Artifacts Check

Look for visual problems:

```
Tearing:         Horizontal lines where frame updates (SYNC issue)
Color bands:     Banding in cloud gradients (8-bit alpha limits expected)
Clipping:        Clouds cut off at screen edges (bounds check)
Z-fighting:      Flickering overlap of same-depth clouds (shouldn't happen)
```

None of these should be visible. Clouds should blend smoothly.

## Debugging Commands (Serial Console)

If available, monitor logs:

```bash
# Watch cloud allocation
idf.py monitor | grep "cloud sprite"

# Watch each frame's render cost
idf.py monitor | grep "cl="

# Search for errors
idf.py monitor | grep "loge\|ERROR\|FAIL"
```

## Regression Checks

**Do NOT regress:**
- Text rendering (clock, temperature, description)
- Particle effects (rain, snow, stars, fog)
- Lightning rendering
- Sun/moon position
- Sky gradient background
- FPS counter (if visible)

These should be unaffected by cloud sprite changes.

## Expected Memory Layout

After init, PSRAM should show:

```
PSRAM allocation (8 sprites):
  sprite[0]: 20 KB @ 0x_________
  sprite[1]: 20 KB @ 0x_________
  sprite[2]: 20 KB @ 0x_________
  ...
  sprite[7]: 20 KB @ 0x_________
  ────────────────
  total: 160 KB

  (Device has 16 MB PSRAM, so ~1% used)
```

If allocation fails:
```
[eva_canvas] Failed to allocate cloud sprite 0
[eva_canvas] ESP_MALLOC returned NULL
```

This would cause clouds to render as black (graceful fallback), but should not crash.

## Success Criteria

Phase 3 is **successful** if:

- ✅ Firmware compiles and boots
- ✅ Sprite atlas allocates (logs visible)
- ✅ Clouds visible on all weather types
- ✅ Clouds grow from small at horizon to large at bottom
- ✅ Multiple (8) distinct shapes visible
- ✅ Clouds fade out near horizon
- ✅ FPS within expected range (20-60 Hz depending on weather)
- ✅ No tearing, z-fighting, or artifacts
- ✅ Wind affects cloud drift
- ✅ No regressions in other features

## If Something Goes Wrong

### Clouds Not Visible
Check:
1. `s_buf` is initialized (should be after 1st frame)
2. `s_clouds3d_active > 0` (check weather spawns clouds)
3. No allocation errors in log

### Clouds Wrong Color
Check:
1. `base_col` calculation (line 1609)
2. Weather type correctly set (stormy/night/day)
3. `blend565()` function working (test with sky rendering)

### Performance Too Low
Check:
1. Number of active clouds (line 1570)
2. Cloud scale distribution (many at y≈0.8-0.95 are expensive)
3. CPU used by sprite blending (use profiler)

### Crash or Hang
Check:
1. Sprite allocation succeeded (logs)
2. `blend_cloud_sprite_cpu()` bounds checks
3. No NULL dereference in `sprite->a8_data`

## Next Steps (Phase 4 Optimization)

When ready to optimize:

1. **PPA Hardware Scaling** — reduce CPU sprite blend cost
2. **Cloud Color Tinting** — vary by sun position (sunrise/sunset)
3. **Adaptive Cloud Count** — reduce if FPS drops below 30 Hz
4. **Sun Halo Sprite** — replace CPU circles with pre-baked A8

---

**Testing Guide Complete** ✓

Proceed with flashing and visual verification on hardware.
