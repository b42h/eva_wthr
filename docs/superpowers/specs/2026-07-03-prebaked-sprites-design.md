# Pre-baked Scene Sprites (Lightning / Sun / Moon / Glass Rain) — Design

**Date:** 2026-07-03
**Status:** DONE — all tasks (§3–8) implemented, built, flashed; optional
panel-level art QA and `eva_wthr` port remain. See Execution status in the
plan `docs/superpowers/plans/2026-07-03-prebaked-sprites.md`.
**Builds on:** prebaked clouds (2026-07-02), visual quality package (2026-07-02),
FPS stabilization + fixes (2026-07-02/03)

## 0. Progress snapshot (2026-07-04)

- **Done:** CLP2 typed pack, Python generators + host gates, 6.34 MB pack,
  device sprite loader (resident PSRAM), lightning/sun/moon/glass-rain sprite
  blits with procedural fallbacks, CLAUDE.md + tree snapshot.
- **Verified on hardware:** firmware + `assets/clouds.bin` flashed; CDC
  `cloudinfo`/`lightning`/`perf` smoke pass.
- **Optional follow-up:** panel-level sprite art tuning (`sprites.py` +
  re-flash pack only); `eva_wthr` port of all 2026-07-02/03 packages.
- **Out of scope (unchanged):** fog sprites, FPS "D" pipeline refactor.

## 1. Motivation and expectation setting

Extend the offline-generation pipeline (CLP pack → mmap → PSRAM sprites →
preview rig → gates) to the remaining hand-drawn scene elements. **This is a
visual-quality package, explicitly NOT an FPS package** (agreed with user
2026-07-03): every element it touches is already cheap at runtime — the win
is realism (branched lightning, cratered moon with real phases, lens-like
glass droplets with wet trails), at ~0.5 MB of flash. The remaining FPS
levers (pipeline overlap "D" → ~22–27 Hz; fog sprites 32→~7 ms) are separate
future work. Fog is explicitly OUT of this package (user: works well enough).

## 2. Decisions made (with user)

| Decision | Choice |
|---|---|
| Scope | Lightning bolts, sun god rays, moon phases, glass rain drops+trails |
| Fog | Excluded (existing CPU blobs stay) |
| Priority framing | "щоб виглядало круто" — visuals first, FPS neutral is acceptable |
| Architecture | One typed CLP2 pack, shared generator toolkit, `.clm` 3-plane container reused with per-type plane semantics |
| Phasing | Lightning → sun/moon → glass, each verified on hardware before the next |

## 3. Pack format CLP2 + loader

- Magic bumps `CLP1` → `CLP2`. The TOC entry's reserved byte becomes
  `type`: 0=cloud, 1=bolt, 2=ray, 3=moon, 4=drop, 5=trail. For clouds the
  layer/pool/variant semantics are unchanged; for sprite types `layer`
  carries the subtype (drop size class, ray phase, moon phase index) and
  `pool` is unused (0).
- `.clm` container reused per sprite with per-type plane meaning:
  - bolt: plane0 = core alpha, plane1 = glow alpha, plane2 unused (size 0 —
    the header's three block sizes already support empty planes);
  - moon: plane0 = alpha (disc), plane1 = luminance (craters + terminator);
  - ray: plane0 = alpha; drop: plane0 = alpha, plane1 = specular;
    trail: plane0 = alpha.
- `eva_cloud_assets` grows a sprite API (module keeps its file, name becomes
  aspirational — no rename churn):
  `bool eva_cloud_assets_sprite(int type, int subtype, int variant, eva_sprite_t *out)`
  where `eva_sprite_t` = `{const uint8_t *plane[3]; uint16_t w, h;}`.
  Sprites are LZ4-decompressed **once at init** into PSRAM and kept
  resident. Decompressed budget: bolts 8×(360×560×2) ≈ 3.2 MB, rays
  4×230 K ≈ 0.9 MB, moon+drops ≈ 0.3 MB, wet-glass accumulation buffer
  384 KB → **≈ 4.8 MB PSRAM** on top of today's ~16 MB usage — fits the
  32 MB part with room; if it ever pinches, bolts drop to on-demand
  decompression at strike-schedule time (they're the 3.2 MB). Clouds keep
  their on-demand path.
- Firmware and pack always ship together; on type/magic mismatch every
  element falls back to its existing procedural drawing (same pattern as
  clouds → `bake_strip_fallback_*`).

## 4. Lightning

- **Generation (Python):** dielectric-breakdown model on a lattice — main
  channel top→bottom with decaying secondary branches; core plane = sharp
  channel, glow plane = gaussian-blurred halo. 8 variants ~360×560.
- **Runtime:** the tuned strike machinery (envelope, 2–6 strokes, flash,
  cadence 2.5–7 s, `lightning` CDC command, z-order over clouds) is kept
  as-is. Only `draw_lightning_path()` is replaced by sprite blits:
  glow plane at channel alpha for the whole strike, core plane at stroke
  peaks. Variant + mirror chosen per strike; a re-stroke may switch variant
  (reads as a repeat discharge). Anchor/placement reuses the existing style
  randomizer (top-drop / diagonal / upward).
- Tints stay runtime (cold blue-white core, warm-ish glow).
- Cost: blits only on strike frames (~2–4 ms) — bounded, rare.

## 5. Sun god rays + moon phases

- **Rays:** 4 animation phases (slight rotation + intensity breathing) as
  ~480×480 A8 radial fans, generated with soft falloff and angular noise.
  Runtime: `draw_sun_fib_light()`'s per-frame wedge march is replaced by a
  phase-cycled sprite blit centred on the sun position; day-time tint logic
  unchanged. Rays render only on clear/partly kinds, as today.
- **Moon:** 8 phase sprites ~120×120 (alpha + luminance planes), procedural
  craters lit by phase angle, clean terminator. Runtime: phase index from
  `moon_phase_pct`, waning = mirror; drawn in the amortized bg repaint where
  the flat disc is drawn today (zero per-frame cost). Old flat-disc code
  stays as the fallback.

## 6. Glass rain (drops + trails, "inside the window")

- **Sprites:** drops in 3 size classes × 3 shapes (dome + top-side specular
  highlight + darker lower rim → lens read), planes alpha + specular;
  2 trail variants (vertical wet streak, ragged edge).
- **Runtime:** the existing `glass_drop_t` FSM (FORMING/SLIDING/DRYING,
  ≤34 drops, states already tuned) is untouched — only drawing changes:
  per-state sprite blit (forming = alpha ramp up, sliding = drop sprite +
  trail deposited behind, drying = alpha ramp down).
- **Trail accumulation:** sliding drops write their trail into a persistent
  A8 "wet glass" buffer (screen-width band), which decays globally every N
  frames — condensation that slowly dries. Composited in the glass overlay
  pass (topmost, as today).
- Cost: ≤ ~1.5 ms/frame in rain (within the current `gl` bucket scale).

## 7. Flash budget

Free in the pack today ≈ 0.77 MB. Compressed estimates: bolts ≈ 0.30 MB,
rays ≈ 0.15 MB, moon ≈ 0.04 MB, drops+trails ≈ 0.03 MB → **≈ 0.52 MB**.
Contingency (in order): half-resolution bolts (glow is soft anyway), then
3 ray phases instead of 4. `genpool.py` keeps printing totals; hard
pack-fits-partition check unchanged.

## 8. Preview rig + gates

- `preview.py` galleries: bolts composited on the storm scene (with
  envelope phases), 8-phase moon strip, drop sheet over a rain scene,
  ray fan over clear-day. Golden-diff covers all of them.
- `test_cloudgen.py` gates: bolt main-channel connectivity (unbroken
  top→bottom path in the core plane) + coverage bounds; moon lit-area
  monotonic across phases; drop specular strictly inside the alpha
  footprint; existing roundtrip/dimension checks extended to CLP2 types.
- Host tests: CLP2 TOC parse (typed entries) in `tools/test_clp_toc.c`.

## 9. Verification on device (per phase)

Each phase lands separately: build + flash, `weatherpin on` for stable
scenes, screenshots (lightning via `lightning` + immediate capture),
FPS must not drop below the current baseline (~15 Hz storm, ~19–20 light
scenes), fallback path spot-checked once (erase storage → procedural
drawing everywhere, scene still renders).

## 10. Out of scope

- Fog sprites (user deferred; the 32 ms/frame fog cost stays).
- Pipeline overlap "D" (the actual FPS lever, separate spec when
  prioritised).
- Any change to strike scheduling/flash tuning (just landed, being
  evaluated live).
- eva_wthr port — after the whole package is signed off.
