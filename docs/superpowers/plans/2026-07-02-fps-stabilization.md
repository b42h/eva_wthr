# FPS Stabilization (A+B+C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Steady ≥18 Hz on every scene with no frame gap >100 ms: span-based text blit (A), mmap'd raw asset partition instead of SPIFFS (B), cloud-blend cost bounding (C1/C2/C4, C3 gated).

**Architecture:** A adds a span table to the scene-text slot so the per-frame blit touches only glyph runs. B replaces the SPIFFS filesystem with one packed `clouds.bin` in a raw partition, memory-mapped — LZ4 reads flash through the cache, no SPI transactions, no render stalls. C instruments the cloud blend, clips bands to populated rows, and serializes morphs so at most one layer double-blends.

**Tech Stack:** ESP-IDF v5.5.4, esp_partition mmap, PPA blend, Python (numpy/lz4) for the packer.

**Spec:** `docs/superpowers/specs/2026-07-02-fps-stabilization-design.md`

**⚠ No git repo** — skip commit steps; verify per task; snapshot at the very end.
**Build env:** `export IDF_PATH="$HOME/.espressif/v5.5.4/esp-idf" && source $IDF_PATH/export.sh`
**Flash:** from `build/`: `python -m esptool --chip esp32p4 -p /dev/cu.usbmodem* -b 460800 --before usb_reset --after hard_reset write_flash @flash_args` (port renames CDC↔ROM per CLAUDE.md §3).
**CDC helper:** `scratchpad/cdc_cmd.py <cmd>...` (session scratchpad) sends console commands; `tools/cdc_shoot.py <outdir> 0 <kind>` grabs screenshots.
**Measure protocol (used by every verify step):** `cdc_cmd.py "weatherdebug <kind> 0"`, wait ≥60 s (covers morph windows), then `cdc_cmd.py perf perf` twice ~30 s apart; record tick/work/tx/cl and the `jitter=..` max from the log lines echoed back.

---

## Baseline (recorded 2026-07-02, before this plan)

| scene | tick | work | tx | cl | jitter max |
|---|---|---|---|---|---|
| clear-day | 10–12 Hz | 78–92 ms | 16–18 ms | 30–37 ms | 246 ms |
| partly-cloudy | 13 Hz | 72 ms | 17 ms | 26 ms | 77 ms |
| thunderstorm | 12–14 Hz | 68–79 ms | 17–18 ms | 23–29 ms | 187 ms |

Variant load (SPIFFS): ~0.7 s with render micro-stalls.

---

### Task 1: Span table — pure header + host test (Fix A, part 1)

**Files:**
- Create: `main/eva_text_spans.h`
- Create: `tools/test_text_spans.c`

- [ ] **Step 1: Write the failing host test**

`tools/test_text_spans.c`:

```c
/* Host test: A8 glyph-run span table build + walk equivalence.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_text_spans tools/test_text_spans.c && /tmp/test_text_spans
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/eva_text_spans.h"

enum { W = 64, H = 8 };

static void fill_row(uint8_t *a8, int row, int from, int to, uint8_t v)
{
    for (int x = from; x < to; ++x) a8[row * W + x] = v;
}

static void test_empty_mask(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    eva_span_t spans[64];
    uint32_t row_start[H + 1];
    int n = eva_text_build_spans(a8, W, 0, H, spans, 64, row_start);
    assert(n == 0);
    for (int r = 0; r <= H; ++r) assert(row_start[r] == 0);
}

static void test_runs_found(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    fill_row(a8, 2, 4, 10, 200);      /* one run */
    fill_row(a8, 3, 0, 2, 50);        /* two runs */
    fill_row(a8, 3, 60, 64, 50);
    eva_span_t spans[64];
    uint32_t row_start[H + 1];
    int n = eva_text_build_spans(a8, W, 0, H, spans, 64, row_start);
    assert(n == 3);
    /* row 2: spans[row_start[2]..row_start[3]) */
    assert(row_start[3] - row_start[2] == 1);
    assert(spans[row_start[2]].x == 4 && spans[row_start[2]].len == 6);
    assert(row_start[4] - row_start[3] == 2);
    assert(spans[row_start[3]].x == 0 && spans[row_start[3]].len == 2);
    assert(spans[row_start[3] + 1].x == 60 && spans[row_start[3] + 1].len == 4);
}

static void test_cap_merges_to_single_span(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    /* 20 isolated pixels on row 1 → more than EVA_TEXT_MAX_SPANS_PER_ROW runs. */
    for (int i = 0; i < 20; ++i) a8[1 * W + i * 3] = 255;
    eva_span_t spans[128];
    uint32_t row_start[H + 1];
    int n = eva_text_build_spans(a8, W, 0, H, spans, 128, row_start);
    assert(row_start[2] - row_start[1] == 1);          /* merged */
    eva_span_t s = spans[row_start[1]];
    assert(s.x == 0 && s.x + s.len == 19 * 3 + 1);     /* covers first..last */
    (void)n;
}

/* Equivalence: walking spans must visit exactly the non-zero pixels. */
static void test_walk_equivalence(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    fill_row(a8, 0, 1, 5, 10);
    fill_row(a8, 4, 30, 40, 99);
    fill_row(a8, 7, 63, 64, 1);
    eva_span_t spans[64];
    uint32_t row_start[H + 1];
    (void)eva_text_build_spans(a8, W, 0, H, spans, 64, row_start);

    static uint8_t visited[W * H];
    memset(visited, 0, sizeof visited);
    for (int r = 0; r < H; ++r) {
        for (uint32_t s = row_start[r]; s < row_start[r + 1]; ++s) {
            for (int x = spans[s].x; x < spans[s].x + spans[s].len; ++x) {
                visited[r * W + x] = 1;
            }
        }
    }
    for (int i = 0; i < W * H; ++i) {
        if (a8[i]) assert(visited[i]);          /* every glyph px covered */
    }
    /* Spans may include interior zeros only via the cap merge; none here. */
    for (int i = 0; i < W * H; ++i) {
        if (visited[i]) assert(a8[i]);
    }
}

int main(void)
{
    test_empty_mask();
    test_runs_found();
    test_cap_merges_to_single_span();
    test_walk_equivalence();
    printf("OK\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `cc -std=c11 -Wall -Wextra -o /tmp/test_text_spans tools/test_text_spans.c && /tmp/test_text_spans`
Expected: FAIL — `eva_text_spans.h: No such file or directory`

- [ ] **Step 3: Write `main/eva_text_spans.h`**

```c
/* Glyph-run span table for A8 text masks (pure, host-testable).
 * Built once per text rebake; the per-frame blit walks only these runs
 * instead of scanning the full bbox. See eva_weather_canvas.c. */
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t x;
    uint16_t len;
} eva_span_t;

#define EVA_TEXT_MAX_SPANS_PER_ROW 16

/* Scan rows [y0, y1) of `a8` (stride w). For each row append its runs of
 * non-zero pixels to `spans`; `row_start[i]` = first span index of row
 * (y0+i), with a final sentinel row_start[y1-y0] = total count. A row with
 * more than EVA_TEXT_MAX_SPANS_PER_ROW runs is merged into one span from
 * its first to last non-zero pixel. Returns the total span count, or -1 if
 * `cap` would be exceeded (caller falls back to bbox blit). */
static inline int eva_text_build_spans(const uint8_t *a8, int w,
                                       int y0, int y1,
                                       eva_span_t *spans, int cap,
                                       uint32_t *row_start)
{
    int total = 0;
    for (int row = y0; row < y1; ++row) {
        row_start[row - y0] = (uint32_t)total;
        const uint8_t *m = &a8[row * w];
        int row_first = -1, row_last = -1;
        int row_begin = total;
        int in_run = 0, run_x = 0;
        for (int x = 0; x < w; ++x) {
            if (m[x] && !in_run) {
                in_run = 1;
                run_x = x;
                if (row_first < 0) row_first = x;
            } else if (!m[x] && in_run) {
                in_run = 0;
                row_last = x - 1;
                if (total >= cap) return -1;
                spans[total++] = (eva_span_t){ (uint16_t)run_x,
                                               (uint16_t)(x - run_x) };
            }
        }
        if (in_run) {
            row_last = w - 1;
            if (total >= cap) return -1;
            spans[total++] = (eva_span_t){ (uint16_t)run_x,
                                           (uint16_t)(w - run_x) };
        }
        if (total - row_begin > EVA_TEXT_MAX_SPANS_PER_ROW) {
            /* Too fragmented — merge the whole row into one span. */
            total = row_begin;
            spans[total++] = (eva_span_t){ (uint16_t)row_first,
                                           (uint16_t)(row_last - row_first + 1) };
        }
    }
    row_start[y1 - y0] = (uint32_t)total;
    return total;
}
```

- [ ] **Step 4: Run the test — expect `OK`**

---

### Task 2: Wire spans into bake + blit, verify on hardware (Fix A, part 2)

**Files:**
- Modify: `main/eva_weather_canvas.c` (`scene_text_slot_t` ~line 430, `bake_scene_text_slot` bbox block ~line 1105, `blit_text_mask_at` ~line 1127, `blit_text_slot` ~line 1150)

- [ ] **Step 1: Extend the slot struct**

In `scene_text_slot_t` add after `bbox_valid`:

```c
    /* Glyph-run spans over rows [bbox_y0, bbox_y1) — built at bake time,
     * walked by the per-frame blit. spans_valid false → bbox fallback. */
    eva_span_t *spans;
    uint32_t *row_start;
    bool spans_valid;
```

Add `#include "eva_text_spans.h"` next to the other local includes.

- [ ] **Step 2: Build spans at bake time**

In `bake_scene_text_slot()`, right after the bbox block sets `slot->bbox_valid = true;`, add:

```c
    /* Span table for the per-frame blit (17 ms bbox scan → glyph runs only).
     * Capacity: every bbox row at the per-row cap. Allocated once, PSRAM. */
    slot->spans_valid = false;
    if (slot->bbox_valid) {
        int rows = slot->bbox_y1 - slot->bbox_y0;
        int cap = rows * EVA_TEXT_MAX_SPANS_PER_ROW;
        if (!slot->spans) {
            slot->spans = heap_caps_malloc(
                (size_t)canvas_h * EVA_TEXT_MAX_SPANS_PER_ROW * sizeof(eva_span_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            slot->row_start = heap_caps_malloc(
                ((size_t)canvas_h + 1) * sizeof(uint32_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (slot->spans && slot->row_start) {
            int n = eva_text_build_spans(slot->a8, canvas_w,
                                         slot->bbox_y0, slot->bbox_y1,
                                         slot->spans, cap, slot->row_start);
            slot->spans_valid = (n >= 0);
        }
    }
```

- [ ] **Step 3: Span path in the blit**

Replace the body of `blit_text_mask_at()` with a span walk plus the old loop as fallback:

```c
static void blit_text_mask_at(const scene_text_slot_t *slot, int dst_x, int dst_y,
                              int x0, int y0, int x1, int y1,
                              uint16_t color, uint8_t base_alpha)
{
    int w = slot->mask_w;
    if (slot->spans_valid) {
        for (int y = y0; y < y1; ++y) {
            int mrow = y - dst_y;                     /* mask row */
            int srow = mrow - (int)slot->bbox_y0;     /* span-table row */
            if (srow < 0 || mrow >= (int)slot->bbox_y1) continue;
            const uint8_t *mask_row = &slot->a8[mrow * w];
            uint16_t *dst_row = &s_buf[y * EVA_WEATHER_RENDER_W];
            for (uint32_t s = slot->row_start[srow];
                 s < slot->row_start[srow + 1]; ++s) {
                int mx0 = slot->spans[s].x;
                int mx1 = mx0 + slot->spans[s].len;
                /* Clip the span to the caller's window [x0,x1) in dst space. */
                int dx0 = mx0 + dst_x, dx1 = mx1 + dst_x;
                if (dx0 < x0) dx0 = x0;
                if (dx1 > x1) dx1 = x1;
                for (int x = dx0; x < dx1; ++x) {
                    uint8_t m = mask_row[x - dst_x];
                    if (m == 0) continue;
                    uint8_t a = (uint8_t)(((uint16_t)m * (uint16_t)base_alpha) / 255U);
                    if (a == 0) continue;
                    dst_row[x] = blend565(dst_row[x], color, a);
                }
            }
        }
        return;
    }
    /* Fallback: original bbox scan. */
    for (int y = y0; y < y1; ++y) {
        const uint8_t *mask_row = &slot->a8[(y - dst_y) * w + (x0 - dst_x)];
        uint16_t *dst_row = &s_buf[y * EVA_WEATHER_RENDER_W + x0];
        int run = x1 - x0;
        for (int i = 0; i < run; ++i) {
            uint8_t m = mask_row[i];
            if (m == 0) continue;
            uint8_t a = (uint8_t)(((uint16_t)m * (uint16_t)base_alpha) / 255U);
            if (a == 0) continue;
            dst_row[i] = blend565(dst_row[i], color, a);
        }
    }
}
```

(`blit_text_slot` itself is unchanged — both shadow and fill calls go
through the span path automatically.)

- [ ] **Step 4: Build**

Run: `idf.py build` → success.

- [ ] **Step 5: Flash + measure (protocol above)**

Expected: `tx` ≤ 5 ms on all three scenes; text looks identical on
`screenshot` (clock, date, temp, desc, shadow present).
Record numbers in the table at the bottom of this plan.

---

### Task 3: `clouds.bin` packer + TOC host test (Fix B, part 1)

**Files:**
- Create: `main/eva_clp_toc.h`
- Create: `tools/test_clp_toc.c`
- Modify: `tools/cloudgen/genpool.py`
- Create: `assets/` (output dir, packed blob)

- [ ] **Step 1: Write the failing host test**

`tools/test_clp_toc.c`:

```c
/* Host test: CLP1 pack TOC parser.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_clp_toc tools/test_clp_toc.c && /tmp/test_clp_toc
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/eva_clp_toc.h"

static uint8_t blob[256];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static size_t build_blob(uint16_t count)
{
    memcpy(blob, "CLP1", 4);
    blob[4] = (uint8_t)count;
    blob[5] = (uint8_t)(count >> 8);
    size_t off = 6;
    for (int i = 0; i < count; ++i) {
        blob[off + 0] = (uint8_t)(i % 3);     /* layer */
        blob[off + 1] = (uint8_t)(i % 2);     /* pool  */
        blob[off + 2] = (uint8_t)i;           /* variant */
        blob[off + 3] = 0;
        put32(&blob[off + 4], 6u + (uint32_t)count * 12u + i * 10u); /* offset */
        put32(&blob[off + 8], 10u);                                   /* size */
        off += 12;
    }
    return off + count * 10u;   /* total blob size incl. payloads */
}

static void test_parse_ok(void)
{
    size_t total = build_blob(4);
    eva_clp_toc_t toc;
    assert(eva_clp_parse(blob, total, &toc));
    assert(toc.count == 4);
    const eva_clp_entry_t *e = eva_clp_entry(&toc, 1);
    assert(e && e->layer == 1 && e->pool == 1 && e->variant == 1);
    assert(e->offset == 6 + 4 * 12 + 10 && e->size == 10);
}

static void test_bad_magic(void)
{
    size_t total = build_blob(1);
    blob[0] = 'X';
    eva_clp_toc_t toc;
    assert(!eva_clp_parse(blob, total, &toc));
}

static void test_out_of_range_entry_rejected(void)
{
    size_t total = build_blob(2);
    /* Corrupt entry 0's size so offset+size overruns the blob. */
    put32(&blob[6 + 8], 100000u);
    eva_clp_toc_t toc;
    assert(!eva_clp_parse(blob, total, &toc));
}

int main(void)
{
    test_parse_ok();
    test_bad_magic();
    test_out_of_range_entry_rejected();
    printf("OK\n");
    return 0;
}
```

- [ ] **Step 2: Run — expect FAIL (`eva_clp_toc.h` missing)**

- [ ] **Step 3: Write `main/eva_clp_toc.h`**

```c
/* CLP1 packed-asset TOC parser (pure, host-testable).
 * Layout: "CLP1" | u16 count | count × {u8 layer, u8 pool, u8 variant,
 * u8 reserved, u32 offset, u32 size} | blobs. All LE. Offsets are absolute
 * from the start of the pack. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t layer;
    uint8_t pool;
    uint8_t variant;
    uint32_t offset;
    uint32_t size;
} eva_clp_entry_t;

#define EVA_CLP_MAX_ENTRIES 64

typedef struct {
    eva_clp_entry_t entries[EVA_CLP_MAX_ENTRIES];
    uint16_t count;
} eva_clp_toc_t;

static inline uint32_t eva_clp_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline bool eva_clp_parse(const uint8_t *pack, size_t pack_size,
                                 eva_clp_toc_t *out)
{
    if (!pack || pack_size < 6 || memcmp(pack, "CLP1", 4) != 0) return false;
    uint16_t count = (uint16_t)(pack[4] | (pack[5] << 8));
    if (count == 0 || count > EVA_CLP_MAX_ENTRIES) return false;
    size_t toc_end = 6 + (size_t)count * 12;
    if (toc_end > pack_size) return false;
    for (uint16_t i = 0; i < count; ++i) {
        const uint8_t *p = &pack[6 + (size_t)i * 12];
        eva_clp_entry_t *e = &out->entries[i];
        e->layer = p[0];
        e->pool = p[1];
        e->variant = p[2];
        e->offset = eva_clp_rd32(&p[4]);
        e->size = eva_clp_rd32(&p[8]);
        if (e->offset < toc_end || e->size == 0 ||
            (size_t)e->offset + e->size > pack_size) {
            return false;
        }
    }
    out->count = count;
    return true;
}

static inline const eva_clp_entry_t *eva_clp_entry(const eva_clp_toc_t *toc,
                                                   int idx)
{
    if (!toc || idx < 0 || idx >= toc->count) return NULL;
    return &toc->entries[idx];
}
```

- [ ] **Step 4: Run — expect `OK`**

- [ ] **Step 5: Teach `genpool.py` to pack**

In `tools/cloudgen/genpool.py`, change `OUT` to a local dir and add a
packer. Replace the module-level `OUT = ...` line with:

```python
OUT = os.path.join(os.path.dirname(__file__), "out")
PACK = os.path.join(os.path.dirname(__file__), "..", "..", "assets", "clouds.bin")
```

Add after the imports:

```python
import struct

def write_pack(entries, pack_path):
    """entries: list of (layer, pool, variant, clm_path). Writes CLP1 pack."""
    os.makedirs(os.path.dirname(pack_path), exist_ok=True)
    blobs = []
    for (_, _, _, path) in entries:
        with open(path, "rb") as f:
            blobs.append(f.read())
    toc_end = 6 + len(entries) * 12
    off = toc_end
    with open(pack_path, "wb") as f:
        f.write(b"CLP1")
        f.write(struct.pack("<H", len(entries)))
        for (layer, pool, variant, _), blob in zip(entries, blobs):
            f.write(struct.pack("<BBBBII", layer, pool, variant, 0, off, len(blob)))
            off += len(blob)
        for blob in blobs:
            f.write(blob)
    return off
```

In `main()`, collect entries while writing the `.clm` files
(`entries.append((layer, 0, v, path))` in the normal loop,
`entries.append((layer, 1, v, path))` in the storm loop), and after the
budget check add:

```python
    pack_size = write_pack(entries, PACK)
    print(f"PACK: {PACK} = {pack_size/1024/1024:.2f} MB")
    if pack_size > 7 * 1024 * 1024 - 65536:
        print("PACK OVER PARTITION"); sys.exit(1)
```

- [ ] **Step 6: Generate and check**

Run: `tools/cloudgen/.venv/bin/python tools/cloudgen/genpool.py`
Expected: 21 `.clm` files in `tools/cloudgen/out/`, `assets/clouds.bin`
≈ 5.4 MB, all gates pass. Then delete the old tree: `rm -rf spiffs_image/`
(the pack + `out/` replace it).

---

### Task 4: Device — mmap loader replaces SPIFFS (Fix B, part 2)

**Files:**
- Modify: `partitions.csv`
- Modify: `main/CMakeLists.txt`
- Modify: `main/eva_cloud_assets.c`

- [ ] **Step 1: Partition subtype**

In `partitions.csv` change the storage line to:

```
storage,  data, 0x40,    ,        7M,
```

- [ ] **Step 2: CMake — flash the pack instead of a SPIFFS image**

In `main/CMakeLists.txt` replace the
`spiffs_create_partition_image(storage ../spiffs_image FLASH_IN_PROJECT)`
line with:

```cmake
esptool_py_flash_to_partition(flash "storage" "${CMAKE_CURRENT_SOURCE_DIR}/../assets/clouds.bin")
```

Also drop `spiffs` from `PRIV_REQUIRES` and add `esp_partition`.

- [ ] **Step 3: Rewrite `eva_cloud_assets.c` internals**

Replace the includes/statics/init/count/load with the mmap version
(public API in `eva_cloud_assets.h` is unchanged):

```c
#include "eva_cloud_assets.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "eva_cloud_scale.h"
#include "eva_clp_toc.h"
#include "lz4.h"

static const char *TAG = "cloud_assets";
static const uint8_t *s_pack;          /* mmap'd partition contents */
static esp_partition_mmap_handle_t s_mmap;
static eva_clp_toc_t s_toc;
static int16_t s_index[EVA_CLOUD_LAYERS][2][EVA_CLOUD_MAX_VARIANTS]; /* -> toc idx */
static int s_counts[EVA_CLOUD_LAYERS][2];
static int64_t s_last_load_us;
static uint8_t *s_scratch;
static size_t s_scratch_bytes;

static bool ensure_scratch(size_t bytes)
{
    if (s_scratch && s_scratch_bytes >= bytes) return true;
    if (s_scratch) heap_caps_free(s_scratch);
    s_scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch_bytes = s_scratch ? bytes : 0;
    return s_scratch != NULL;
}

bool eva_cloud_assets_init(void)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "storage");
    if (!part) {
        ESP_LOGW(TAG, "storage partition not found — procedural fallback");
        return false;
    }
    esp_err_t err = esp_partition_mmap(part, 0, part->size,
                                       ESP_PARTITION_MMAP_DATA,
                                       (const void **)&s_pack, &s_mmap);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mmap failed (%s) — procedural fallback",
                 esp_err_to_name(err));
        return false;
    }
    if (!eva_clp_parse(s_pack, part->size, &s_toc)) {
        ESP_LOGW(TAG, "no CLP1 pack in storage — procedural fallback");
        return false;
    }
    memset(s_index, -1, sizeof s_index);
    memset(s_counts, 0, sizeof s_counts);
    for (int i = 0; i < s_toc.count; ++i) {
        const eva_clp_entry_t *e = &s_toc.entries[i];
        if (e->layer >= EVA_CLOUD_LAYERS || e->pool >= 2 ||
            e->variant >= EVA_CLOUD_MAX_VARIANTS) continue;
        s_index[e->layer][e->pool][e->variant] = (int16_t)i;
        if ((int)e->variant + 1 > s_counts[e->layer][e->pool]) {
            s_counts[e->layer][e->pool] = e->variant + 1;
        }
    }
    for (int l = 0; l < EVA_CLOUD_LAYERS; ++l) {
        ESP_LOGI(TAG, "layer %d: %d normal, %d storm variants (mmap)",
                 l, s_counts[l][0], s_counts[l][1]);
    }
    return true;
}

int eva_cloud_assets_count(int layer, cloud_pool_t pool)
{
    if (layer < 0 || layer >= EVA_CLOUD_LAYERS || (int)pool < 0 || pool > 1) return 0;
    return s_counts[layer][pool];
}

int64_t eva_cloud_assets_last_load_us(void)
{
    return s_last_load_us;
}

bool eva_cloud_assets_load(int layer, cloud_pool_t pool, int idx,
                           uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core,
                           int dst_w, int dst_h,
                           bool mirror_x, float scale)
{
    if (!s_pack || layer < 0 || layer >= EVA_CLOUD_LAYERS ||
        idx < 0 || idx >= s_counts[layer][pool]) {
        return false;
    }
    int16_t ti = s_index[layer][pool][idx];
    if (ti < 0) return false;
    const eva_clp_entry_t *e = &s_toc.entries[ti];
    const uint8_t *clm = s_pack + e->offset;

    int64_t t0 = esp_timer_get_time();
    clm_header_t hdr;
    if (e->size < CLM_HEADER_BYTES ||
        !clm_parse_header(clm, e->size, &hdr)) {
        ESP_LOGW(TAG, "L%d %s v%d: bad clm header", layer,
                 pool ? "storm" : "normal", idx);
        return false;
    }
    size_t src_bytes = (size_t)hdr.w * hdr.h;
    if (!ensure_scratch(src_bytes)) return false;

    uint8_t *dsts[3] = { a8_light, a8_shadow, a8_core };
    size_t off = CLM_HEADER_BYTES;
    for (int m = 0; m < 3; ++m) {
        uint32_t csize = hdr.comp_size[m];
        if (off + csize > e->size) {
            ESP_LOGW(TAG, "L%d v%d: truncated block %d", layer, idx, m);
            return false;
        }
        int dec = LZ4_decompress_safe((const char *)(clm + off),
                                      (char *)s_scratch,
                                      (int)csize, (int)src_bytes);
        if (dec != (int)src_bytes) {
            ESP_LOGW(TAG, "L%d v%d: lz4 block %d decode failed (%d)",
                     layer, idx, m, dec);
            return false;
        }
        clm_scale_mask(s_scratch, hdr.w, hdr.h,
                       dsts[m], dst_w, dst_h, scale, mirror_x);
        off += csize;
    }
    s_last_load_us = esp_timer_get_time() - t0;
    return true;
}
```

(Everything SPIFFS-related — `esp_spiffs.h`, mount conf, `fopen` scan,
the `comp` heap buffer — is gone.)

- [ ] **Step 4: Build**

Run: `idf.py build` → success. Check the flasher args now include
`clouds.bin` at the storage offset:
`grep -i clouds build/flash_args || grep -i storage build/flash_args`

- [ ] **Step 5: Flash + verify**

- Boot/`cloudinfo`: pools discovered via mmap (`5 normal, 0 storm` HIGH;
  `5/3` MID/LOW), loads succeed.
- `cloudinfo` last load expected **≤ 250 ms** (LZ4 + resample only).
- Freeze check: `weatherdebug thunderstorm 0` (forces 2 storm loads),
  during the next 120 s run `perf` 3×; **jitter max must stay < 100 ms.**
- Fallback check: `python -m esptool ... erase_region <storage_off> 0x700000`
  (offset from `build/partition_table/partition-table.csv`), reboot →
  `procedural fallback` warning, clouds still render; re-flash after.

---

### Task 5: `cl` attribution instrumentation (Fix C1)

**Files:**
- Modify: `main/eva_weather_canvas.c`

- [ ] **Step 1: Count blend bands + morphing layers**

Add statics near the other `s_prof_*`:

```c
static uint32_t s_prof_blend_bands;    /* PPA band blends this log window */
static uint32_t s_prof_morph_frames;   /* frames with ≥1 layer morphing */
```

In `blend_mask_ppa_one_band()` (and its CPU fallback `blend_mask_cpu` call
site) increment `s_prof_blend_bands++;` once per invocation. In the render
loop where the 3 `blend_layer` calls run, before them add:

```c
        if (s_strip[0].morphing || s_strip[1].morphing || s_strip[2].morphing) {
            s_prof_morph_frames++;
        }
```

Extend the perf log line with ` clb=%u mfr=%u` printing
`s_prof_blend_bands / s_frames` (bands per frame) and `s_prof_morph_frames`,
and reset both where the other accumulators reset.

- [ ] **Step 2: Build, flash, measure**

For each scene record `clb` (bands/frame) steady vs during morph. Expected
insight: `cl` ≈ clb × ~7–11 ms band cost. **Decision gate for Task 6/7:**
- If steady clear-day shows clb ≥ 2 with near-empty layers → Task 6 (row
  clipping) is justified.
- If morph windows show clb ≥ 4 → Task 7 (morph serialization) is justified.
Record the numbers; skip whichever task its gate doesn't justify.

---

### Task 6: Content-row clipping of blend bands (Fix C2, gated by Task 5)

**Files:**
- Modify: `main/eva_weather_canvas.c` (`cloud_variant_t`, `load_or_bake_variant`, `blend_mask_ppa_one_band` callers)

- [ ] **Step 1: Track populated rows per variant**

Add to `cloud_variant_t`:

```c
    uint16_t content_y0;   /* first populated row of a8_light */
    uint16_t content_y1;   /* one past last populated row */
```

After a successful load (and after the fallback bake) in
`load_or_bake_variant()`, scan the light mask once:

```c
    v->content_y0 = 0;
    v->content_y1 = (uint16_t)strip->strip_h;
    {
        int y0 = -1, y1 = -1;
        for (int y = 0; y < strip->strip_h; ++y) {
            const uint8_t *row = &v->a8_light[y * CLOUD_STRIP_W];
            bool nz = false;
            for (int x = 0; x < CLOUD_STRIP_W; x += 4) {  /* stride-4 probe */
                if (row[x]) { nz = true; break; }
            }
            if (nz) { if (y0 < 0) y0 = y; y1 = y; }
        }
        if (y0 >= 0) {
            v->content_y0 = (uint16_t)(y0 > 2 ? y0 - 2 : 0);
            v->content_y1 = (uint16_t)((y1 + 3 < strip->strip_h) ? y1 + 3
                                                                 : strip->strip_h);
        } else {
            v->content_y1 = v->content_y0;   /* empty mask — skip blends */
        }
    }
```

- [ ] **Step 2: Clip the band vertically**

In `blend_layer_variant()`, compute the clipped destination row range and
pass it down: the band currently spans dst rows `eff_y .. eff_y+strip_h`
clamped to the viewport. Intersect with
`eff_y + v->content_y0 .. eff_y + v->content_y1`; if empty, skip the PPA
call entirely. Implementation: add `int src_row0, int src_row1` params to
`blend_mask_ppa_one_band()` and use them for `in.block_offset_y`,
`block_h`, and the output offset (mirror the existing eff_y math). Keep the
CPU fallback consistent.

- [ ] **Step 3: Build, flash, measure**

Expected: clear-day steady `cl` drops sharply (HIGH cirrus band ≈ its
streak rows only); no visual change at the strip edges (2-row feather
margin included above). Screenshot per kind — clouds identical.

---

### Task 7: Morph serialization (Fix C4, gated by Task 5)

**Files:**
- Modify: `main/eva_weather_canvas.c` (morph-start condition, ~line 3449)

- [ ] **Step 1: Only one layer crossfades at a time**

In the lifecycle update where a strip is about to start morphing
(`strip->morph_clock >= strip->morph_hold_s` branch), add a global gate:

```c
            bool another_morphing = false;
            for (int j = 0; j < CLOUD_LAYER_COUNT; ++j) {
                if (j != i && s_strip[j].morphing) { another_morphing = true; break; }
            }
            if (another_morphing) {
                /* Hold this layer's clock at the threshold — it starts as
                 * soon as the current crossfade finishes. */
                strip->morph_clock = strip->morph_hold_s;
            } else if (strip->morph_clock >= strip->morph_hold_s) {
                strip->morphing = true;
                ...
            }
```

(Adapt to the exact existing control flow: the condition gains the
`!another_morphing` guard; everything else stays.)

- [ ] **Step 2: Build, flash, measure**

Expected: during-morph `clb` caps at (layers steady + 1 extra), `cl` morph
worst case ≤ ~30 ms, no visible change in cloud behaviour (morphs just
queue up).

---

### Task 8: Full verification vs spec §5 + docs + snapshot

**Files:**
- Modify: `../CLAUDE.md`
- Create: snapshot

- [ ] **Step 1: Full measurement matrix**

For clear-day / partly-cloudy-day / cloudy / thunderstorm, ≥3 morph cycles
each (protocol above). Fill in:

| scene | tick | work | tx | cl | clb | jitter max |
|---|---|---|---|---|---|---|
| clear-day (before) | 10–12 | 78–92 ms | 16–18 | 30–37 | — | 246 ms |
| clear-day (after) | | | | | | |
| partly (before) | 13 | 72 ms | 17 | 26 | — | 77 ms |
| partly (after) | | | | | | |
| thunderstorm (before) | 12–14 | 68–79 ms | 17–18 | 23–29 | — | 187 ms |
| thunderstorm (after) | | | | | | |

Pass criteria (spec §5): tick ≥ 18 Hz everywhere incl. morph windows; no
jitter > 100 ms (incl. during loads and kind switches); tx ≤ 5 ms;
cl ≤ 15 ms steady / ≤ 20 ms morph; text pixel-identical; `cloudinfo`
pools + fallback intact.

- [ ] **Step 2: C3 gate decision**

If all criteria pass, note in CLAUDE.md that C3 (pre-tinted compose to
restore shadow/core depth at unchanged cost) is available as a follow-up
quality task — do NOT implement it in this plan.

- [ ] **Step 3: Update CLAUDE.md**

- §6: cloud-assets bullet — storage is now a raw `0x40` partition with a
  `CLP1` pack (`assets/clouds.bin`, built by `genpool.py`), mmap'd; SPIFFS
  is gone. Text blit uses span tables (`eva_text_spans.h`).
- §7: replace the "12–14 Hz measured" bullet with the new numbers; update
  the ~0.7 s load note (now ≤ 250 ms, no stalls); keep the D-refactor note
  (30 FPS) as the remaining ceiling.
- §8: add snapshot line.

- [ ] **Step 4: Snapshot**

```sh
cd /Users/b42h/Desktop/JC4880P443C_I_W/42_EVE_FW
rsync -a --exclude build --exclude tools/cloudgen/.venv --exclude tools/cloudgen/__pycache__ \
    phase7_eva_weather/ phase7_eva_weather-snapshot-2026-07-02-fps-stabilization/
```

- [ ] **Step 5: Remind about the eva_wthr port**

All three 2026-07-02 packages (prebaked clouds, visual quality, FPS
stabilization) still need porting to `eva_wthr` after sign-off.
