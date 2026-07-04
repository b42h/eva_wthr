# Completed work

Condensed record of shipped features. Each landed with a spec + plan and
was verified on hardware. Detailed design docs that are still useful for
future work remain under `docs/` and `docs/superpowers/`; the process
snapshots (STATUS/AUDIT/PHASE reports) they replaced were removed.

## Rendering pipeline
- Native 800×480 landscape render → PPA rotate 270° → 480×800 DPI panel.
  Single canvas + render task (`eva_weather_canvas.c`).
- Time-of-day sky palette (day/sunset/night/sunrise) in
  `eva_sky_palette.h`, shared with the host `tools/skypreview.c`.
- Bayer 8×8 sky dithering (`eva_dither.h`), applied only on background
  rebake.
- Independent sun and moon, god rays, night backlight dim (01:00–06:00).

## Clouds (pre-baked)
- Offline generator `tools/cloudgen/` (FBM + domain warp) → LZ4 `.clm`
  masks, packed into `assets/clouds.bin`, mmap'd from a raw `0x40`
  storage partition. On-device procedural bake kept only as fallback.
- Three parallax layers: slow morph, wind drift, depth "breathing"
  (load-time scale), seamless X tiling with seam crossfade in
  `eva_cloud_scale.h`.
- Kind-aware storm cloud pool (separate masks for thunderstorm/heavy-rain).
- Device renders the light plane only; `decompose()` puts the bulk of
  density there with local-depth shading (X-smoothed to avoid striping).

## Weather effects
- Rain / heavy-rain / snow / sleet / hail particle systems.
- Lightning composited over the cloud deck (thunderstorm/hail); CDC
  `lightning` test trigger.

## Performance & stability
- Span-based text blit (`eva_text_spans.h`): per-frame text cost ~17 ms → ~5 ms.
- mmap asset loading removed SPIFFS render stalls; blend bands clipped to
  populated rows; cloud morphs serialized.
- Screenshot capture copies under the render lock (no half-painted frames).

## Tooling & tests
- Host tests: `tools/test_text_spans.c`, `test_clp_toc.c`, `test_clm_scale.c`
  (incl. seam continuity), `test_dither.c`, `tools/cloudgen/test_cloudgen.py`
  (visibility + striping + coverage gates).
- Preview rig: `preview.py` (contact sheet + golden diff), `animate.py`,
  `skypreview.c`, `sim_cloud_blend.c` (device-exact scale/wrap simulator).

## Still planned (not started)
- `docs/superpowers/{specs,plans}/2026-07-03-prebaked-sprites*` — offline
  sprite atlases for sun/moon/precip.
- `docs/superpowers/{specs,plans}/2026-07-04-offline-bake-30fps-storm*` —
  cut the per-frame pixel-moving floor to reach a stable ≥30 FPS night
  storm (cached text buffer, offline-merged storm layers, native portrait
  render to drop the PPA rotate stage).
