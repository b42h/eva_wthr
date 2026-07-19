/* CLP2/CLP3 packed-asset TOC parser (pure, host-testable).
 * CLP3 = portrait-oriented cloud masks (270° CCW, matches eva_orient.h).
 * Layout: "CLP2"|"CLP3" | u16 count | count × {u8 layer, u8 pool, u8 variant,
 * u8 type, u32 offset, u32 size} | blobs. All LE. Offsets are absolute
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
    uint8_t type;
    uint32_t offset;
    uint32_t size;
} eva_clp_entry_t;

/* Pack now carries 30 cloud entries (normal/storm/merged/lit pools) + 97
 * sprite entries (bolt/ray/moon(32)/drop/trail + 32 rain + 1 fog + 32 sky)
 * = 127 (was 126 with an 8-phase moon; moon went 8 -> 32 phases 2026-07-18
 * to cut phase-quantization error, see EVA_MOON_PHASE_COUNT below).
 * Cap set to 160 for headroom. The pack builder (tools/cloudgen/genpool.py)
 * currently emits 127; if you add asset variants, keep this above the count
 * or eva_clp_parse() rejects the WHOLE pack -> procedural fallback everywhere. */
#define EVA_CLP_MAX_ENTRIES 160

#define EVA_CLP_TYPE_CLOUD 0   /* light/shadow/core cloud mask (as CLP1) */
#define EVA_CLP_TYPE_BOLT  1   /* lightning bolt: core + glow planes */
#define EVA_CLP_TYPE_RAY   2   /* sun god-ray fan, one animation phase */
#define EVA_CLP_TYPE_MOON  3   /* moon phase: alpha + luminance planes */
#define EVA_CLP_TYPE_DROP  4   /* glass rain drop: alpha + specular planes */
#define EVA_CLP_TYPE_TRAIL 5   /* wet trail a sliding drop leaves behind */
#define EVA_CLP_TYPE_RAIN  6   /* offline rain-streak A8 loop frame */
#define EVA_CLP_TYPE_FOG   7   /* offline fog band A8 (single, drifted) */
#define EVA_CLP_TYPE_SKY   8   /* offline sky keyframe RGB565 column */

/* Number of baked moon phases in the pack (tools/cloudgen/sprites.py
 * MOON_PHASE_COUNT, tools/cloudgen/genpool.py). Must match exactly — the
 * canvas quantizes moon_phase_pct (0..100) to a phase index using this. */
#define EVA_MOON_PHASE_COUNT 32

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
    if (!pack || pack_size < 6) return false;
    if (memcmp(pack, "CLP2", 4) != 0 && memcmp(pack, "CLP3", 4) != 0) return false;
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
        e->type = p[3];
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
