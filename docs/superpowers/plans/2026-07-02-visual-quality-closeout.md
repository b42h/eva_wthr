# Visual Quality Package — Closeout Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Verify and close out the already-implemented visual quality package (spec: `docs/superpowers/specs/2026-07-02-visual-quality-package-design.md`).

**Architecture:** n/a — implementation is complete; this plan covers only the remaining verification, docs and snapshot steps.

**Tech Stack:** esptool, CDC console (`cloudinfo`, `weatherdebug`, `screenshot`), rsync snapshot.

**⚠ No git repo** — no commits; snapshot at the end.

---

## Implementation status (verified 2026-07-02)

Everything in the spec is implemented and passes local verification:

| Spec § | Item | Status |
|---|---|---|
| §3 | ridged LOW / anisotropic HIGH / `PROFILES_STORM` / `cloud_L<l>s_v*.clm` | ✅ 21 files, 5.40 MB (budget 6 MB) |
| §4 | `eva_dither.h` + canvas integration + `test_dither` | ✅ test OK |
| §5 | `eva_sky_palette.h` + `skypreview.c` (`--dump-json`, grid 12 kinds × 4 dayparts) | ✅ builds, renders |
| §6 | `animate.py`, golden-diff in `preview.py`, pool gates in `test_cloudgen`/`genpool` | ✅ tests OK |
| §7 | pool-aware `eva_cloud_assets_*`, kind-aware selection, `cloudinfo` pool tag | ✅ in code, builds |
| — | host tests: `test_dither`, `test_clm_scale`, `test_cloud_bake_fsm`, `test_cloudgen.py` | ✅ all OK |
| — | build: `eva_weather.bin` + `storage.bin` fresh (Jul 2 16:20) | ✅ |

Note: the spec's §5 ordering constraint (verify palette extraction on device
BEFORE enabling dither) was not followed — both landed together, so the
screenshot-identity check is no longer possible in isolation. Accepted
residual risk: palette extraction correctness is instead covered by the
overall on-device look check (Task 2 Step 3) — any palette regression would
show as wrong sky colours there.

### Task 1: Golden reference snapshot

**Files:**
- Create: `tools/cloudgen/golden/` (generated)

- [ ] **Step 1: Approve the current contact sheet**

Open `tools/cloudgen/preview_contact_sheet.png`. If the current look is approved, continue; if not, iterate `PROFILES` first — golden should capture an approved state.

- [ ] **Step 2: Snapshot it as golden**

Run: `tools/cloudgen/.venv/bin/python tools/cloudgen/preview.py --golden`
Expected: `golden/` created; subsequent `preview.py` runs render was/now/diff.

### Task 2: Hardware verification (spec §8)

**Files:** none

- [ ] **Step 1: Flash**

```sh
cd phase7_eva_weather/build
python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* -b 460800 \
    --before usb_reset --after hard_reset write_flash @flash_args
```
(Port held by orphan python → `lsof /dev/cu.usbmodem*`, kill it.)

- [ ] **Step 2: Boot log check**

Expected: `cloud_assets` reports both pools (normal 5, storm 3 for MID/LOW; storm 0 for HIGH), no `procedural fallback` warning.

- [ ] **Step 3: Sky look check (covers palette extraction)**

Via CDC: `weatherdebug clear_day`, `weatherdebug cloudy`, `weatherdebug clear_night` + `screenshot` each. Sky colours/day-night behaviour must match the pre-package screenshots and `sky_grid.ppm` from skypreview.

- [ ] **Step 4: Storm shapes**

Via CDC: `weatherdebug thunderstorm` → `cloudinfo` shows `storm` pool on MID/LOW; `screenshot` shows near-solid heavy base, visibly different shapes from `cloudy`, not just darker tints.

- [ ] **Step 5: Banding on the physical panel**

Set a night/dusk scene (`weatherdebug clear_night`), photograph the panel. Expected: no visible banding steps in the gradient (RGB565 screenshots can't show this — must be a photo).

- [ ] **Step 6: FPS unchanged**

FPS chip on a cloudy scene ≥ ~20 Hz across 2+ morph cycles; `cloudinfo` last load still ~30–60 ms.

### Task 3: Docs + snapshot

**Files:**
- Modify: `../CLAUDE.md`
- Create: snapshot copy

- [ ] **Step 1: Update CLAUDE.md**

- §6 recipes: extend the cloud-assets bullet — storm pool (`cloud_L<l>s_v*.clm`, kinds THUNDERSTORM/HEAVY_RAIN, HIGH stays normal), dither lives in `eva_dither.h` (Bayer 8×8, bg-rebake only), palettes in `eva_sky_palette.h` shared with `tools/skypreview.c`.
- §8 ladder: add `phase7_eva_weather-snapshot-2026-07-02-visual-quality` line, mark as current shipped state.

- [ ] **Step 2: Snapshot**

```sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW
rsync -a --exclude build --exclude tools/cloudgen/.venv --exclude tools/cloudgen/__pycache__ \
    phase7_eva_weather/ phase7_eva_weather-snapshot-2026-07-02-visual-quality/
```

- [ ] **Step 3: Flag the eva_wthr port**

Remind the user: `eva_wthr` (GitHub copy) needs both 2026-07-02 packages ported after hardware sign-off (feature-parity rule).
