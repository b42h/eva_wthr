# Lightning — cached directional bolts (2026-07-19)

Fractal midpoint-displacement generator in `tools/cloudgen/sprites.py`
(`gen_bolt(seed, direction)`). 1px thin core + soft blurred glow. Not the
old random walker.

## Pack keying

- `EVA_BOLT_DIRECTION_COUNT = 7`, `EVA_BOLT_VARIANT_COUNT = 4` → 28 sprites
- Keyed `(type=BOLT, subtype=direction, variant=v)`
- Directions (`BOLT_DIR_NAMES`): 0 down, 1 down-left, 2 down-right, 3 up,
  4 up-left, 5 up-right, 6 intracloud
- Keep `eva_clp_toc.h` and `sprites.py` in sync

## Device rules

- `generate_lightning_bolt()` only picks (direction, variant) + anchor
- **Do NOT horizontal-mirror** bolt sprites (breaks baked slant)
- Core tint near-pure white (`rgb565(255,255,255)`) — thin channels need
  brightness, not width
- Polyline is a single top-down fallback for a missing pack only

## Regen

```sh
CLOUDGEN_PORTRAIT=0 .venv/bin/python genpool.py
# verify bolts==28, dirs 0..6 before flashing
```

Inspector: `tools/cloudgen/clm_read.py <pack> 1 <dir> <var> out.png`
