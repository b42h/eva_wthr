# Night Mode for CLOUDY / RAIN / FOG / SNOW — Implementation Plan

> **For agentic workers:** This is firmware for ESP32-P4 (`phase7_eva_weather`).
> There is **no on-host unit-test harness** — the LVGL/canvas pipeline only
> produces pixels on the physical panel. Verification is therefore **build →
> flash → CDC `weatherdebug <kind>` + `screenshot` → inspect the JPEG**, per
> `CLAUDE.md` §4. The TDD "write a failing test" step is replaced throughout by
> a concrete screenshot-based verification step.
>
> This directory is **not a git repository**, so there are no per-task commit
> steps. Snapshots are taken manually by copying the `phase7_eva_weather` tree
> (see `README.md` §"Snapshot ladder"); not required for this change.

**Goal:** Make the full-screen sky gradient for CLOUDY / RAIN / HEAVY_RAIN /
THUNDERSTORM / SNOW / SLEET / HAIL / FOG darken smoothly to charcoal-grey at
night, instead of staying a fixed daytime grey at all hours.

**Architecture:** Extract the existing night-ness ramp (0=day … 1=deep night)
from `clear_sky_palette()` into a reusable pure function `sky_nightness()`, then
in `sky_for_kind()` interpolate each neutral kind between its current daytime
palette and a new charcoal-grey night palette using that factor. No warm orange
glow for these kinds (overcast skies don't show a sunset). All changes are in
one file: `main/eva_weather_canvas.c`.

**Tech Stack:** C, ESP-IDF v5.5.4, LVGL 9.2.2, custom RGB565 canvas renderer.

---

## ✅ EXECUTION STATUS (2026-05-29): DONE — verified on hardware

All three tasks below were implemented and verified via CDC screenshots
(`weatherdebug` + `screenshot`) at both day (clockoffset +9h → 08:xx) and
night (clockoffset 0 → 23:xx) clocks:

- **Task 1** (`sky_nightness` + `clear_sky_palette` refactor) — done; CLEAR /
  PARTLY day & night screenshots unchanged (refactor behaviour-preserving).
- **Task 2** (day→night lerp for the 8 neutral kinds) — done; daytime
  unchanged, night skies charcoal-grey.
- **Task 3** (particle legibility) — verified; no particle colour change
  needed.
- **Addendum (Task 4, below)** — `draw_fog_bands()` removed entirely at the
  user's request after the "4 sinusoids over cloudy" was spotted during
  verification. **This was not in the original spec.**

---

## File Structure

Single file touched:

- **Modify:** `main/eva_weather_canvas.c`
  - Add `sky_nightness()` (new static pure function, ~18 lines) just above
    `clear_sky_palette()` (currently at `:1049`).
  - Refactor `clear_sky_palette()` (`:1049-1103`) night branch to call
    `sky_nightness()` — behaviour-preserving.
  - Rewrite the neutral-kind branches of `sky_for_kind()` (`:1118-1135`) to
    lerp day→night by `sky_nightness()`.

No header changes (`sky_nightness` is file-local static). No data/API changes.

---

## Reference: exact current code being transformed

`clear_sky_palette()` night branch (`eva_weather_canvas.c:1089-1101`):

```c
    } else {
        /* Nighttime: distance (minutes) past the nearer terminator. */
        float past = (m < sr) ? (float)(sr - m) : (float)(m - ss);
        if (past >= DUSK) {
            top = night_top;
            bot = night_bot;
        } else {
            /* Just after sunset / just before sunrise: twilight → night. */
            float t = past / DUSK;                 /* 0 at terminator, 1 deep night */
            top = lerp_rgb(twi_top, night_top, t);
            bot = lerp_rgb(twi_bot, night_bot, t);
        }
    }
```

`sky_for_kind()` neutral branches (`eva_weather_canvas.c:1118-1135`):

```c
    if (kind == WEATHER_RAIN || kind == WEATHER_SLEET) {
        return (sky_t){ "rain", {34, 44, 60}, {82, 92, 104} };
    }
    if (kind == WEATHER_HEAVY_RAIN) {
        return (sky_t){ "heavy-rain", {18, 26, 40}, {52, 60, 74} };
    }
    if (kind == WEATHER_THUNDERSTORM) {
        return (sky_t){ "thunderstorm", {12, 18, 30}, {46, 48, 58} };
    }
    if (kind == WEATHER_SNOW || kind == WEATHER_HAIL) {
        return (sky_t){ "snow", {116, 132, 150}, {205, 214, 220} };
    }
    if (kind == WEATHER_FOG) {
        return (sky_t){ "fog", {130, 138, 145}, {215, 216, 210} };
    }
    if (kind == WEATHER_CLOUDY) {
        return (sky_t){ "cloudy", {78, 92, 108}, {150, 160, 170} };
    }
```

**Key insight for the extraction:** night-ness must be **0 across all of
daytime** (`m >= sr && m <= ss`). During daytime `clear_sky_palette` blends on
`elev` (twilight→noon), a *different* axis. So `sky_nightness()` returns 0 in
the daytime branch and only ramps `past/DUSK` (clamped to 1) in the night
branch. This keeps `clear_sky_palette` pixel-identical after refactor.

---

## Task 1: Add `sky_nightness()` and refactor `clear_sky_palette` to use it

**Files:**
- Modify: `main/eva_weather_canvas.c` (insert before `:1049`; edit `:1089-1101`)

- [x] **Step 1: Insert the new pure function above `clear_sky_palette`**

Insert immediately before the line `static sky_t clear_sky_palette(int m, int sr, int ss)` (currently `:1049`, after the `sun_curve` fwd decl at `:1048`):

```c
/* Night-ness factor 0..1 for the current minute, shared by every sky that
 * has a day↔night cycle. 0 = sun above horizon (full day), 1 = past civil
 * twilight (deep night); the dusk band ramps 0→1 over civil_twilight_minutes()
 * on each side of the terminator. This is exactly the ramp that already drives
 * the night branch of clear_sky_palette() — factored out so the neutral kinds
 * (RAIN/CLOUDY/FOG/SNOW/...) can reuse the same timing. Pure function. */
static float sky_nightness(int m, int sr, int ss)
{
    /* Daytime: sun is up, no night-ness regardless of elevation. */
    if (m >= sr && m <= ss) {
        return 0.0f;
    }
    const float DUSK = civil_twilight_minutes();
    float past = (m < sr) ? (float)(sr - m) : (float)(m - ss);
    if (DUSK <= 1.0f) return 1.0f;          /* degenerate guard: snap to night */
    float n = past / DUSK;                  /* 0 at terminator, 1 deep night */
    if (n > 1.0f) n = 1.0f;
    return n;
}
```

- [x] **Step 2: Refactor `clear_sky_palette` night branch to use `sky_nightness`**

Replace the `else { ... }` night block (`:1089-1101`) shown in the reference above with:

```c
    } else {
        /* Nighttime: twilight → night by the shared night-ness ramp. */
        float t = sky_nightness(m, sr, ss);    /* 0 at terminator, 1 deep night */
        top = lerp_rgb(twi_top, night_top, t);
        bot = lerp_rgb(twi_bot, night_bot, t);
    }
```

Note: the old code special-cased `past >= DUSK` to assign `night_top/bot`
directly; with `sky_nightness` clamped to 1.0 the `lerp_rgb(..., 1.0f)` yields
the same `night_*` colours, so behaviour is identical. The local `DUSK` const
at `:1075` is now only used by the daytime branch's comments — leave it; it is
still referenced? Check: after this edit `DUSK` is unused in
`clear_sky_palette`. **Remove the now-unused `const float DUSK = ...;` line at
`:1075`** to avoid a `-Werror=unused-variable` build break.

- [x] **Step 3: Build**

Run:
```sh
. ~/.espressif/v5.5.4/esp-idf/export.sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
python "$IDF_PATH/tools/idf.py" build
```
Expected: build succeeds, no `unused-variable` / `implicit-declaration`
warnings for `sky_nightness` or `DUSK`.

- [x] **Step 4: Flash + regression-screenshot CLEAR (refactor must not change it)**

Flash:
```sh
./flash.sh   # or the esptool fallback in README if CDC port is held
```
Then via the CDC port (`/dev/cu.usbmodem1234561`), for each of
`clear-day`, `clear-night`, `partly-cloudy-day`, `partly-cloudy-night`:
send `weatherdebug <kind>\r\n`, then `screenshot\r\n`, save the JPEG.

Expected: CLEAR/PARTLY skies look **identical** to pre-change (the refactor is
behaviour-preserving). Twilight transition still smooth. If any CLEAR sky
shifted, the extraction is wrong — stop and reconcile before Task 2.

---

## Task 2: Lerp neutral kinds day→night in `sky_for_kind`

**Files:**
- Modify: `main/eva_weather_canvas.c` (`:1118-1135`, the neutral-kind branches)

- [x] **Step 1: Replace the six neutral-kind return blocks**

Replace the block shown in the reference (`:1118-1135`, from
`if (kind == WEATHER_RAIN ...` through the `WEATHER_CLOUDY` block) with the
following. `m`, `sr`, `ss` are already in scope at the top of `sky_for_kind`
(computed at `:1107-1109`).

```c
    /* Neutral kinds (no explicit day/night variant) share one day↔night
     * ramp: each keeps its existing daytime palette and fades to a charcoal-
     * grey night palette as sky_nightness() goes 0→1. No warm orange glow —
     * an overcast / rainy / snowy sky does not show a sunset, it just darkens.
     * Charcoal (not black) so clouds and rain/snow particles still read.
     * FOG and SNOW stay lighter at night (fog scatters city/moon light; snow
     * reflects it). */
    {
        float n = sky_nightness(m, sr, ss);
        rgb_t day_top, day_bot, night_top, night_bot;
        const char *tag;
        bool matched = true;
        switch (kind) {
        case WEATHER_RAIN:
        case WEATHER_SLEET:
            tag = "rain";
            day_top   = (rgb_t){ 34,  44,  60}; day_bot   = (rgb_t){ 82,  92, 104};
            night_top = (rgb_t){ 18,  22,  30}; night_bot = (rgb_t){ 40,  46,  56};
            break;
        case WEATHER_HEAVY_RAIN:
            tag = "heavy-rain";
            day_top   = (rgb_t){ 18,  26,  40}; day_bot   = (rgb_t){ 52,  60,  74};
            night_top = (rgb_t){ 12,  16,  24}; night_bot = (rgb_t){ 30,  36,  46};
            break;
        case WEATHER_THUNDERSTORM:
            tag = "thunderstorm";
            day_top   = (rgb_t){ 12,  18,  30}; day_bot   = (rgb_t){ 46,  48,  58};
            night_top = (rgb_t){ 10,  14,  22}; night_bot = (rgb_t){ 28,  32,  42};
            break;
        case WEATHER_SNOW:
        case WEATHER_HAIL:
            tag = "snow";
            day_top   = (rgb_t){116, 132, 150}; day_bot   = (rgb_t){205, 214, 220};
            night_top = (rgb_t){ 30,  36,  46}; night_bot = (rgb_t){ 58,  66,  80};
            break;
        case WEATHER_FOG:
            tag = "fog";
            day_top   = (rgb_t){130, 138, 145}; day_bot   = (rgb_t){215, 216, 210};
            night_top = (rgb_t){ 40,  44,  52}; night_bot = (rgb_t){ 70,  74,  82};
            break;
        case WEATHER_CLOUDY:
            tag = "cloudy";
            day_top   = (rgb_t){ 78,  92, 108}; day_bot   = (rgb_t){150, 160, 170};
            night_top = (rgb_t){ 22,  26,  34}; night_bot = (rgb_t){ 42,  48,  58};
            break;
        default:
            matched = false;
            tag = ""; day_top = day_bot = night_top = night_bot = (rgb_t){0,0,0};
            break;
        }
        if (matched) {
            return (sky_t){ tag,
                            lerp_rgb(day_top, night_top, n),
                            lerp_rgb(day_bot, night_bot, n) };
        }
    }
```

The trailing `return clear_sky_palette(m, sr, ss);` at `:1150` stays as the
fall-through for CLEAR/PARTLY (and anything unmatched). The explicit-night
guard for `WEATHER_CLEAR_NIGHT`/`WEATHER_PARTLY_CLOUDY_NIGHT` at `:1114-1116`
stays above this block, untouched.

- [x] **Step 2: Build**

Run:
```sh
python "$IDF_PATH/tools/idf.py" build
```
Expected: success. Watch for `rgb_t` compound-literal warnings (none expected;
`rgb_t` is already used as `{r,g,b}` elsewhere, e.g. `clear_sky_palette`).

- [x] **Step 3: Flash + screenshot DAY regression for neutral kinds**

Flash, then for each of `cloudy`, `rain`, `fog`, `snow` send `weatherdebug
<kind>` + `screenshot` **at a daytime clock** (default synced time, or use the
`eva_clock` hour-offset / test mode to force midday if it's currently night).

Expected: each looks like the **pre-change daytime** sky (lerp with `n≈0`
reproduces the old fixed colour). If a daytime sky changed, the day_* values
were mistyped — fix against the reference table.

- [x] **Step 4: Flash + screenshot NIGHT for neutral kinds (the actual feature)**

Force a night clock (hour-offset in test mode, or test at real night) and
repeat `weatherdebug <kind>` + `screenshot` for `cloudy`, `rain`, `fog`,
`snow`.

Expected:
- CLOUDY / RAIN: charcoal-grey, clearly night, clouds still visible.
- SNOW: dark but a touch lighter (snow reflectance), snowflakes read.
- FOG: lightest of the four, still obviously dim/night, fog band reads.
- The moon (already drawn via `is_night_kind`) sits over a dark sky now,
  not a bright grey one.

- [x] **Step 5: Twilight smoothness check**

Set the clock to ~15–25 min after local sunset (hour/min offset in test mode),
`weatherdebug cloudy` + `screenshot`. Expected: an intermediate grey between
day and full-night — no hard jump. Repeat at a couple of offsets if unsure.

---

## Task 3: Particle / fog legibility verification (no code unless needed)

**Files:** none unless a contrast problem is found.

- [x] **Step 1: Inspect the Task 2 Step 4 night screenshots for legibility**

Confirm against the night JPEGs already captured:
- RAIN streaks (`P_RAIN`) visible against the charcoal sky.
- SNOW flakes (`P_SNOW`) visible.
- FOG band (`P_FOG`) reads as fog, not a flat fill.

Expected: legible — rain/snow particles are light-grey/white and the night sky
is charcoal, so contrast is higher than before, not lower. No change needed.

- [x] **Step 2: ONLY if a particle is now too dim** — bump its colour/alpha

If (and only if) inspection shows a particle washing out, locate its draw
branch in the particle-render switch: `case P_RAIN:` at
`eva_weather_canvas.c:2868`, `case P_SNOW:` at `:2892`, `case P_FOG:` at
`:2941` (`case P_STAR:` at `:2923`). Nudge the colour lighter or alpha higher by a small
Fibonacci step. Re-build, re-flash, re-screenshot the affected kind at night to
confirm. Do **not** restructure the particle system. If Step 1 passed, skip
this step entirely.

---

## Task 4 (ADDENDUM, not in original spec): Remove `draw_fog_bands()` entirely

**Why:** During Task 2/3 verification, the live "Хмарно наживо" scene showed
**4 translucent sinusoids** drifting over the clouds once per bg-rebake. Root
cause: `draw_fog_bands()` was gated only on `s_fog_pct >= 30`, and clearoutside
reports a fog % even on plain overcast (observed: kind=CLOUDY, fog=96 %), so the
bands drew on non-fog kinds. The user rejected the effect outright ("цих 4
синусоїд бути не повинно взагалі"). Decision: delete the effect for **all**
kinds (a kind-gated version was tried first and rejected).

**Files:**
- Modify: `main/eva_weather_canvas.c` — remove the call in the bg-rebake block
  of the render loop, and remove the `draw_fog_bands()` definition.

- [x] **Step 1: Remove the call site** (in the `if (!s_bg_buf || s_bg_ttl == 0)`
  bg-rebake block, right after `draw_sun_god_rays(t);`):

```c
        /* removed: was
         * if (s_fog_pct >= 30) { draw_fog_bands(t); }
         */
```

- [x] **Step 2: Remove the function definition** (`draw_fog_bands(float t)` —
  the 4-band `sinf()` wisp loop). Deleting both the call and the definition
  together avoids a `-Werror=unused-function` break.

- [x] **Step 3: Build** — clean, no `unused-function` warning.

- [x] **Step 4: Flash + verify** — forced `weatherdebug fog 0` and
  `weatherdebug cloudy 0` at night: **no sinusoids on either** (fog was the
  densest-band case). Live cloudy (fog=96 % in feed) also clean.

**Not addressed here (separate, pre-existing):** dark wavy bands on a *clear*
night sky = cloud strips + RGB565 banding on a near-flat dark gradient,
documented as expected in `TESTING_CLOUD_SPRITES.md`. Distinct from the deleted
fog sinusoids; out of scope for this change.

---

## Self-Review notes

- **Spec coverage:** sky-gradient night mode → Task 2; twilight smoothness →
  Task 1 (shared ramp) + Task 2 Step 5; particle/fog verification → Task 3;
  CLEAR regression from the refactor → Task 1 Step 4; neutral-kind family
  (incl. storm kinds) → Task 2 switch covers all 8.
- **Placeholders:** none — all RGB triples and the full switch are inline.
- **Type consistency:** `sky_nightness(int,int,int)→float` defined in Task 1
  Step 1, called in Task 1 Step 2 and Task 2 Step 1 with the same signature.
  `rgb_t` compound literals match existing usage in `clear_sky_palette`.
  `lerp_rgb`, `civil_twilight_minutes`, `sky_t` all pre-exist.
- **Edge case (`DUSK` unused after refactor):** explicitly handled in Task 1
  Step 2 (remove the line) to avoid `-Werror=unused-variable`.
