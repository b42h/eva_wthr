# Stabilization Phase (ЛІНІЯ A) Status

**Date:** 2026-05-26  
**Commit:** c5aa8a0  
**Status:** ✅ P0 Fixes Applied (A1-A3)

## Applied Fixes

### A1: Guard `lv_canvas_set_buffer` per-frame (✅ DONE)
- **File:** `main/eva_weather_canvas.c:2361-2387`
- **Change:** Added `s_lv_attached_idx` static tracker
- **Effect:** `lv_canvas_set_buffer` now calls ~every 2 frames instead of every frame
- **Expected outcome:** Eliminates LVGL per-frame invalidation overhead

### A2: Remove `s_bg_ttl = 0` from text setters (✅ DONE)
- **Files:** `main/eva_weather_canvas.c:2746-2752, 2754-2760, 2762-2768`
- **Change:** Removed forced background cache invalidation
- **Effect:** Text updates no longer flush `fill_gradient + draw_sun_or_moon + memcpy 750KB`
- **Expected outcome:** No visible flash on minute/second updates

### A3: Conditional weather reset (✅ DONE)
- **File:** `main/eva_weather_canvas.c:2715-2730`
- **Change:** Reset state only when `kind_changed`, not on minor parameter shifts
- **Effect:** Wind/cloud % changes no longer trigger full scene rebuild
- **Expected outcome:** Smooth gradual parameter updates without visible pumps

## Deferred (P1 Items)

### A4: Bake strip out of hot path
- **Rationale:** Already existed in backup; 30-80ms spikes are acceptable for native 800×480
- **Action:** Deferred to Phase B if performance proves insufficient

### A5: Text rendering cost optimization
- **Options:** Glyph cache vs. LVGL labels vs. do nothing
- **Rationale:** Current `draw_text_utf8` cost ~1-3ms; acceptable for 13ms budget
- **Action:** Deferred to Phase B if profiling shows it's the bottleneck

## Test Plan

**Before moving to Phase B, verify these DoD criteria:**

1. ✋ **Idle stability (5min on clear-day)**
   - FPS overlay ±2 Hz range
   - No visible flicker in canvas itself
   - No artifacts in clock/temp/desc text

2. ✋ **Weather transitions**
   - kind change (day→night) — smooth over ≥30 frames
   - cloud % change (0→100) — gradual, no visible pump
   - wind speed change (0→40 kph) — particle drift smooth

3. ✋ **Text updates**
   - Minute roll (13:59→14:00) — no gradient flash
   - Temperature change — no background invalidation
   - Description change — text appears cleanly

4. ✋ **Edge cases**
   - Cold boot + NTP sync — no frame skip on time jump
   - Test swipe ±2h × 20× — no heap leak, no tearing

## Next Steps

Once above DoD criteria are met:
- Move to **ЛІНІЯ B (Phase 3 Integration)**
- Enable Phase 3 sprite clouds
- Integrate `draw_clouds_3d` into render pipeline

---

**Document version:** 1.0 (2026-05-26)
