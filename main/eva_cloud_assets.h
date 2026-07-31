/* Pre-baked cloud mask + sprite assets, packed into a CLP2 blob that is
 * memory-mapped directly from a raw flash partition (no SPIFFS).
 * See docs/2026-07-02-prebaked-cloud-masks-design.md. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EVA_CLOUD_LAYERS       3
#define EVA_CLOUD_MAX_VARIANTS 8

typedef enum {
    CLOUD_POOL_NORMAL       = 0,
    CLOUD_POOL_STORM        = 1,
    CLOUD_POOL_STORM_MERGED = 2,   /* MID+LOW pre-composited, 1 blend */
    CLOUD_POOL_STORM_LIT    = 3,   /* internally-lit storm deck (strike) */
    CLOUD_POOL_COUNT        = 4,
} cloud_pool_t;

/* Memory-map the "storage" partition, parse its CLP2 TOC, index cloud
 * entries by layer/pool/variant, and decompress all non-cloud (sprite)
 * entries into resident PSRAM. Returns true if at least one layer has at
 * least one variant in either pool. */
bool eva_cloud_assets_init(void);

/* Stop serving anything backed by the mmap'd pack, so the storage partition can
 * be rewritten underneath us during a pack OTA. After this returns,
 * eva_cloud_assets_count() reports 0 and eva_cloud_assets_load() refuses, which
 * makes the canvas fall back to procedural clouds — an already-supported state.
 *
 * Sprites (moon/bolt/fog/rain) were decompressed into PSRAM at init and do NOT
 * alias the mapping, so they keep working.
 *
 * There is no resume: the mapping is only re-established by a reboot, which the
 * pack OTA does anyway. */
void eva_cloud_assets_suspend(void);

/* Number of variants discovered for `layer` in `pool` (0 if none / init failed). */
int eva_cloud_assets_count(int layer, cloud_pool_t pool);

/* Decompress variant `idx` of `layer`/`pool` into the caller's A8 buffers
 * (dst_w × dst_h each), applying optional X-mirror and a bilinear
 * depth scale (>1 = clouds closer / magnified around the centre).
 * Any source resolution in the file is resampled to dst. Returns false
 * on read/decode error (caller falls back to procedural bake). */
bool eva_cloud_assets_load(int layer, cloud_pool_t pool, int idx,
                           uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core,
                           int dst_w, int dst_h, int dst_stride,
                           bool mirror_x, float scale);

/* Duration of the last successful load, in microseconds (for cloudinfo). */
int64_t eva_cloud_assets_last_load_us(void);

typedef struct {
    const uint8_t *plane[3];   /* light, shadow, core (clouds) or type-
                                 * specific pair (sprites); NULL when the
                                 * pack stored an empty plane. */
    uint16_t w;
    uint16_t h;
} eva_sprite_t;

/* Resident sprite lookup (decompressed once at init). type/subtype/variant
 * per the CLP2 sprite map — see EVA_CLP_TYPE_* in eva_clp_toc.h. Returns
 * false if the pack lacks that sprite — caller falls back to its
 * procedural drawing. */
bool eva_cloud_assets_sprite(int type, int subtype, int variant,
                             eva_sprite_t *out);

#ifdef __cplusplus
}
#endif
