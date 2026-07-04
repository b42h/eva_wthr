# Sync 42_EVE_FW ↔ eva_wthr Implementation Plan

> **For agentic workers:** Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring both project copies to the same feature set — port this session's sky/sun/moon + backlight + Wi-Fi work into `42_EVE_FW/phase7_eva_weather` (the main project), and port `42_EVE_FW`'s cloud/lightning work into `eva_wthr` (the GitHub copy).

**Architecture:** The two copies share an identical base for `eva_clock.c`, `eva_wifi.c`, `main.c` (0-line base diff) and for the *sky* functions of `eva_weather_canvas.c`. They diverge only in the *cloud/lightning* parts of canvas (664-line base diff). So this session's sky patch applies cleanly to 3 files + 12/16 canvas hunks; only 4 canvas hunks need manual placement.

**Tech Stack:** ESP-IDF v5.5.4, C, ESP32-P4. Build via `idf.py build`. No unit-test framework on target — verify via host arithmetic tests (already written) + `idf.py build` + flash smoke test.

**Safety:** `42_EVE_FW` is NOT a git repo — no undo. Backup taken at `/tmp/42_eve_backup-20260621-230818`. Re-backup before edits in Task 0.

---

## File Structure

| File | 42_EVE_FW action | eva_wthr action |
| --- | --- | --- |
| `main/eva_clock.c` | apply session patch (clean) | already done |
| `main/eva_clock.h` | apply session patch (clean) | already done |
| `main/eva_wifi.c` | apply session patch (clean) | already done |
| `main/main.c` | apply session patch (clean) | already done |
| `main/eva_weather_canvas.c` | 12 hunks via patch + 4 manual | port cloud/lightning fns back (Phase B) |

Patch file: `/tmp/session_changes.patch` (from `git format-patch -1 f3cfcfb -- main/`).

---

## Phase A — Port session work INTO 42_EVE_FW

### Task 0: Backup

**Files:** none modified.

- [ ] **Step 1: Fresh backup**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
TS=$(date +%Y%m%d-%H%M%S); mkdir -p /tmp/42_eve_backup-$TS
cp main/eva_weather_canvas.c main/eva_clock.c main/eva_clock.h main/eva_wifi.c main/main.c /tmp/42_eve_backup-$TS/
echo "backup: /tmp/42_eve_backup-$TS"
```

Expected: backup path printed, 5 files copied.

### Task 1: Apply clean files (clock, wifi, main)

**Files:**
- Modify: `main/eva_clock.c`, `main/eva_clock.h`, `main/eva_wifi.c`, `main/main.c`

- [ ] **Step 1: Apply the 4 clean files from the patch**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
for f in eva_clock.c eva_clock.h eva_wifi.c main.c; do
  patch -p1 "main/$f" < <(awk "/^diff --git a\/main\/$f /{p=1} p&&/^diff --git/&&!/\/$f /{p=0} p" /tmp/session_changes.patch)
done
```

Expected: each prints `patching file main/<f>` with no "hunk failed".

- [ ] **Step 2: Confirm no rejects**

```bash
ls main/*.rej 2>/dev/null && echo "REJECTS PRESENT" || echo "clean"
```

Expected: `clean`.

### Task 2: Apply the 12 clean canvas hunks

**Files:**
- Modify: `main/eva_weather_canvas.c`

- [ ] **Step 1: Apply canvas hunks with fuzz; 4 will reject (expected)**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
patch -p1 --fuzz=3 main/eva_weather_canvas.c < <(awk '/^diff --git a\/main\/eva_weather_canvas.c /{p=1} p&&/^diff --git/&&!/eva_weather_canvas.c /{p=0} p' /tmp/session_changes.patch)
```

Expected: `4 out of 16 hunks failed--saving rejects to main/eva_weather_canvas.c.rej`.

### Task 3: Manually place the 4 rejected hunks

The 4 rejects all sit in cloud/lightning-shifted regions. Each is a self-contained edit. The reject file is `main/eva_weather_canvas.c.rej`.

**Reject 1 — `@@ -294` (luminary position cache + constants):** adds `luminary_pos_t`, `s_luminary_pos`, `SUN_HORIZON_Y`, `SUN_APEX_Y_WINTER/SUMMER`, `SET_GLIDE_MIN`. Target: just after the sun-position cache block (`s_sun_x/y` declarations, ~line 271-300 in 42). These are new declarations — paste the `+` lines from the reject hunk after the existing `s_sun_strength`/`s_sun_elevation` block.

**Reject 2 — `@@ -3416` (new sky/luminary functions):** the big hunk adding `solar_declination_deg`, `set_glide_minutes`, `sun_apex_y_seasonal`, `arc_progress_glide`, `luminary_pos_from_progress`, `compute_luminary_pos`, and the rewritten `draw_sun_or_moon`. Target: the existing `draw_sun_or_moon` in 42 (line ~2316) and surrounding helpers. Replace 42's `draw_sun_or_moon` body with the session version and insert the new helper functions above it. The sky functions (`fill_sky`, `clear_sky_palette`, `sky_for_kind`, `civil_twilight_minutes`) are byte-identical in both copies' base, so their edits already applied in Task 2.

**Reject 3 — `@@ -3460` (renderpath call site):** changes `fill_gradient(sky.top, sky.bottom)` → `compute_luminary_pos(m, &s_luminary_pos); fill_sky(...)`. Target in 42: line ~4226 (`fill_gradient(sky.top, sky.bottom);` inside the bg-cache pass). Replace per the session diff.

**Reject 4 — `@@ -3882` (background_hold_frames / render tail):** minor context-shifted edit near `background_hold_frames` (42 line ~4108). Apply the `+`/`-` lines from the reject by hand.

- [ ] **Step 1: Add `fill_sky` definition if Task 2 didn't place it**

`fill_sky` is new. Confirm presence:

```bash
grep -c "static void fill_sky" main/eva_weather_canvas.c
```

If `0`, copy the full `fill_sky` function from
`/Users/b42h/Desktop/JC4880P443C_I_W/eva_wthr/main/eva_weather_canvas.c`
(search `static void fill_sky`) and paste it immediately before `fill_gradient` in 42. Keep 42's `fill_gradient` — it is still referenced elsewhere; do not delete it.

- [ ] **Step 2: Place Reject 1 (declarations)**

Open `main/eva_weather_canvas.c.rej`, take the hunk headed `@@ -294`, and insert its `+` lines (the `luminary_pos_t` struct, `s_luminary_pos`, and the `SUN_*`/`SET_GLIDE_MIN` `#define`s) after the `s_sun_elevation` declaration block. Reference: same block in eva_wthr canvas around the `luminary_pos_t` typedef.

- [ ] **Step 3: Place Reject 2 (new functions + draw_sun_or_moon)**

From the eva_wthr canvas, copy these complete functions and insert them just above 42's `draw_sun_or_moon`:
`solar_declination_deg`, `set_glide_minutes`, `sun_apex_y_seasonal`, `arc_progress_glide`, `luminary_pos_from_progress`, `compute_luminary_pos`.
Then replace 42's `draw_sun_or_moon` body with eva_wthr's version (it reads `s_luminary_pos` instead of computing position inline).

- [ ] **Step 4: Place Reject 3 (renderpath)**

In 42's bg-cache pass (the `if (!s_bg_buf || s_bg_ttl == 0)` block, ~line 4224), replace:

```c
        fill_gradient(sky.top, sky.bottom);
```

with:

```c
        int m = minutes_now();
        compute_luminary_pos(m, &s_luminary_pos);
        fill_sky(sky.top, sky.bottom,
                 s_luminary_pos.x_n, s_luminary_pos.y_n, s_luminary_pos.warmth);
```

(Match exact surrounding lines from eva_wthr.)

- [ ] **Step 5: Place Reject 4 (render tail)**

Apply the small `@@ -3882` hunk's `+`/`-` lines by hand at 42's `background_hold_frames` neighbourhood. If it is only a context-shift with no real `+`/`-` content change, skip.

- [ ] **Step 6: Remove the reject file**

```bash
rm -f main/eva_weather_canvas.c.rej main/eva_weather_canvas.c.orig
```

### Task 4: Build 42_EVE_FW

**Files:** none.

- [ ] **Step 1: Build**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf" && source "$IDF_PATH/export.sh" >/dev/null 2>&1
idf.py build 2>&1 | tail -15
```

Expected: `Project build complete`, `eva_weather.bin` generated. Fix any undeclared-symbol errors by checking the corresponding function was pasted (Task 3).

- [ ] **Step 2: Re-run host curve tests (logic unchanged, sanity)**

```bash
/tmp/bl_test && /tmp/sun_test && /tmp/night_test
```

Expected: all pass / SMOOTH verdicts (already verified in eva_wthr).

### Task 5: Flash + smoke test 42_EVE_FW

**Files:** none.

- [ ] **Step 1: Flash and capture health log**

42_EVE_FW uses `flash-weather.sh` (not `flash.sh`). Confirm:

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW/phase7_eva_weather
ls flash*.sh 2>/dev/null || ls ../flash*.sh
```

Flash with the weather flasher, then capture 30 s: expect `GOT IP`, `HTTP time sync`, steady `tick=`, no `panic`/`abort`.

---

## Phase B — Port cloud/lightning work INTO eva_wthr

These functions exist only in 42_EVE_FW: `bake_cloud_scatter`, `lightning_stroke_envelope`, `nudge_lightning_channel`, `clamp_lightning_xy`, `strip_eff_y`, plus the `CLOUD_STRIP_OVERFLOW_Y` cloud-strip rework.

### Task 6: Diff the cloud/lightning regions

- [ ] **Step 1: Generate a focused diff (42 → eva_wthr) for cloud/lightning only**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W
diff eva_wthr/main/eva_weather_canvas.c \
     42_EVE_FW/phase7_eva_weather/main/eva_weather_canvas.c > /tmp/canvas_full.diff
wc -l /tmp/canvas_full.diff
```

Review which hunks are cloud/lightning (the `CLOUD_STRIP_OVERFLOW_Y`, `bake_cloud_scatter`, `*lightning*` regions) vs sky (already synced). Only port the cloud/lightning hunks.

### Task 7: Port cloud-strip rework + new functions into eva_wthr

**Files:**
- Modify: `eva_wthr/main/eva_weather_canvas.c`

- [ ] **Step 1: Add `CLOUD_STRIP_OVERFLOW_Y` + rewrite the three cloud-strip initializers**

Copy the `#define CLOUD_STRIP_OVERFLOW_Y FIB_144` and the `s_strip[]` initializer block (the `.y_start = -CLOUD_STRIP_OVERFLOW_Y, .strip_h = EVA_WEATHER_RENDER_H + 2*CLOUD_STRIP_OVERFLOW_Y` form) from 42 into eva_wthr, replacing the old `.y_start=-40,.strip_h=560` form.

- [ ] **Step 2: Add the new functions**

Copy complete bodies of `strip_eff_y`, `bake_cloud_scatter`, `lightning_stroke_envelope`, `nudge_lightning_channel`, `clamp_lightning_xy` from 42 into eva_wthr at the matching locations (near the cloud bake / lightning draw code). Update their call sites per 42.

- [ ] **Step 3: Build eva_wthr**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/eva_wthr
export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf" && source "$IDF_PATH/export.sh" >/dev/null 2>&1
idf.py build 2>&1 | tail -12
```

Expected: `Project build complete`.

### Task 8: Verify eva_wthr ≈ 42 canvas (sky+clouds synced)

- [ ] **Step 1: Re-diff; only intentional GitHub-cleanliness differences should remain**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W
diff <(grep -vE '^\s*$' eva_wthr/main/eva_weather_canvas.c) \
     <(grep -vE '^\s*$' 42_EVE_FW/phase7_eva_weather/main/eva_weather_canvas.c) | grep -cE '^[<>]'
```

Expected: small number (ideally 0 for canvas). Remaining diffs should only be intentional (e.g. comments), not behaviour.

---

## Phase C — Commit eva_wthr (GitHub copy stays credential-free)

### Task 9: Ensure eva_wthr has NO real credentials, then commit

**Files:**
- Modify: `eva_wthr/main/eva_wifi.c` (only if real creds present)

- [ ] **Step 1: Check for real credentials**

```bash
grep -nE 'getRicher|sure0420' eva_wthr/main/eva_wifi.c
```

If present, replace with the Kconfig-driven defaults or placeholders
(`CONFIG_EVA_WIFI_SSID` / `"YOUR_WIFI_SSID"` / `"YOUR_WIFI_PASSWORD"`) so the
GitHub copy ships credential-free, per the README "Notes For GitHub Users".

- [ ] **Step 2: Commit eva_wthr cloud/lightning sync**

```bash
cd /Users/b42h/Desktop/JC4880P443C_I_W/eva_wthr
git add -A && git commit -m "Sync cloud/lightning rework from main project; scrub Wi-Fi creds"
```

- [ ] **Step 3: (Deferred) GitHub push — only on explicit user go-ahead**

Do NOT `git push` until the user confirms credentials are scrubbed and they want it public.

---

## Self-Review notes

- **Coverage:** Phase A ports all 7 new + 4 modified sky functions and the 3 clean files; Phase B ports all 5 cloud/lightning functions + strip rework; Phase C handles the GitHub credential rule.
- **No placeholders:** every step has exact commands/paths. Manual hunks (Task 3) reference the exact source function names and target line neighbourhoods.
- **Risk:** the only non-mechanical work is Task 3 (4 canvas hunks) and Task 7 (cloud port). Both build-verified before moving on.
- **Reversibility:** Task 0 backup + eva_wthr is git-backed.
