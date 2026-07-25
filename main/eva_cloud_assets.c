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
static int16_t s_index[EVA_CLOUD_LAYERS][CLOUD_POOL_COUNT][EVA_CLOUD_MAX_VARIANTS]; /* -> toc idx */
static int s_counts[EVA_CLOUD_LAYERS][CLOUD_POOL_COUNT];
static int64_t s_last_load_us;
static uint8_t *s_scratch;
static size_t s_scratch_bytes;

typedef struct {
    eva_sprite_t sprite;
    uint8_t type, subtype, variant;
} eva_sprite_slot_t;

/* Non-cloud entries loaded into resident PSRAM. genpool.py now packs 96 of
 * them (bolt/ray/moon/drop/trail + 32 rain + 1 fog + 32 sky). 128 gives
 * headroom; too small and rain/sky sprites get dropped and fall back to the
 * procedural path per element. Keep in sync with EVA_CLP_MAX_ENTRIES. */
static eva_sprite_slot_t s_sprites[128];
static int s_sprite_count;

static bool ensure_scratch(size_t bytes)
{
    if (s_scratch && s_scratch_bytes >= bytes) return true;
    if (s_scratch) heap_caps_free(s_scratch);
    s_scratch = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_scratch_bytes = s_scratch ? bytes : 0;
    return s_scratch != NULL;
}

/* Shared by the per-frame cloud loader and the once-at-boot sprite loader:
 * LZ4-decompress one .clm plane block (`csize` bytes at `clm + off`) into
 * `dst` (`dst_bytes` bytes), bounds-checked against the entry's total
 * `clm_size`. `log_ctx` prefixes the warning so callers keep their own
 * layer/pool/index or sprite-entry-index context in the log. */
static bool clm_decompress_plane(const uint8_t *clm, size_t clm_size,
                                 size_t off, uint32_t csize,
                                 uint8_t *dst, size_t dst_bytes,
                                 const char *log_ctx, int plane_idx)
{
    if (off + csize > clm_size) {
        ESP_LOGW(TAG, "%s: truncated block %d", log_ctx, plane_idx);
        return false;
    }
    int dec = LZ4_decompress_safe((const char *)(clm + off), (char *)dst,
                                  (int)csize, (int)dst_bytes);
    if (dec != (int)dst_bytes) {
        ESP_LOGW(TAG, "%s: lz4 block %d decode failed (%d)",
                 log_ctx, plane_idx, dec);
        return false;
    }
    return true;
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
        ESP_LOGW(TAG, "no CLP2/CLP3 pack in storage — procedural fallback");
        return false;
    }
    memset(s_index, -1, sizeof s_index);
    memset(s_counts, 0, sizeof s_counts);
    for (int i = 0; i < s_toc.count; ++i) {
        const eva_clp_entry_t *e = &s_toc.entries[i];
        if (e->type != EVA_CLP_TYPE_CLOUD) continue;
        if (e->layer >= EVA_CLOUD_LAYERS || e->pool >= CLOUD_POOL_COUNT ||
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

    /* Non-cloud (sprite) entries: decompress every present plane once into
     * resident PSRAM. Sprite entries encode (type, subtype, variant) in
     * (e->type, e->layer, e->variant) — see tools/cloudgen/genpool.py
     * add_sprite(). */
    s_sprite_count = 0;
    size_t sprite_bytes = 0;
    for (int i = 0; i < s_toc.count; ++i) {
        const eva_clp_entry_t *e = &s_toc.entries[i];
        if (e->type == EVA_CLP_TYPE_CLOUD) continue;
        if (s_sprite_count >= (int)(sizeof s_sprites / sizeof s_sprites[0])) {
            ESP_LOGW(TAG, "sprite table full — dropping entry %d", i);
            continue;
        }
        const uint8_t *clm = s_pack + e->offset;
        char log_ctx[24];
        snprintf(log_ctx, sizeof log_ctx, "sprite entry %d", i);
        clm_header_t hdr;
        if (e->size < CLM_HEADER_BYTES || !clm_parse_header(clm, e->size, &hdr)) {
            ESP_LOGW(TAG, "%s: bad clm header", log_ctx);
            continue;
        }
        size_t plane_bytes = (size_t)hdr.w * hdr.h;
        eva_sprite_t sp = { .plane = { NULL, NULL, NULL }, .w = hdr.w, .h = hdr.h };
        size_t off = CLM_HEADER_BYTES;
        bool ok = true;
        for (int m = 0; m < 3; ++m) {
            uint32_t csize = hdr.comp_size[m];
            if (csize == 0) {
                sp.plane[m] = NULL;
                continue;
            }
            uint8_t *buf = heap_caps_malloc(plane_bytes,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!buf) {
                ESP_LOGW(TAG, "%s: PSRAM alloc failed (%u B)",
                         log_ctx, (unsigned)plane_bytes);
                ok = false;
                break;
            }
            if (!clm_decompress_plane(clm, e->size, off, csize, buf,
                                      plane_bytes, log_ctx, m)) {
                heap_caps_free(buf);
                ok = false;
                break;
            }
            sp.plane[m] = buf;
            sprite_bytes += plane_bytes;
            off += csize;
        }
        if (!ok) {
            for (int m = 0; m < 3; ++m) {
                if (sp.plane[m]) heap_caps_free((void *)sp.plane[m]);
            }
            continue;
        }
        s_sprites[s_sprite_count].sprite = sp;
        s_sprites[s_sprite_count].type = e->type;
        s_sprites[s_sprite_count].subtype = e->layer;  /* e->layer doubles as
                                                         * subtype for sprite
                                                         * entries (non-cloud
                                                         * types don't have a
                                                         * layer concept). */
        s_sprites[s_sprite_count].variant = e->variant;
        s_sprite_count++;
    }
    ESP_LOGI(TAG, "sprites: %d resident, %u KB",
             s_sprite_count, (unsigned)(sprite_bytes / 1024));
    ESP_LOGI(TAG, "free PSRAM after asset init: %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    return true;
}

int eva_cloud_assets_count(int layer, cloud_pool_t pool)
{
    if (layer < 0 || layer >= EVA_CLOUD_LAYERS ||
        (int)pool < 0 || (int)pool >= CLOUD_POOL_COUNT) return 0;
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
        (int)pool < 0 || (int)pool >= CLOUD_POOL_COUNT ||
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

    char log_ctx[24];
    snprintf(log_ctx, sizeof log_ctx, "L%d v%d", layer, idx);
    uint8_t *dsts[3] = { a8_light, a8_shadow, a8_core };
    size_t off = CLM_HEADER_BYTES;
    for (int m = 0; m < 3; ++m) {
        uint32_t csize = hdr.comp_size[m];
        if (!clm_decompress_plane(clm, e->size, off, csize, s_scratch,
                                  src_bytes, log_ctx, m)) {
            return false;
        }
        clm_scale_mask(s_scratch, hdr.w, hdr.h,
                       dsts[m], dst_w, dst_h, scale, mirror_x);
        off += csize;
    }
    s_last_load_us = esp_timer_get_time() - t0;
    return true;
}

bool eva_cloud_assets_sprite(int type, int subtype, int variant,
                             eva_sprite_t *out)
{
    if (!out) return false;
    for (int i = 0; i < s_sprite_count; ++i) {
        if (s_sprites[i].type == type && s_sprites[i].subtype == subtype &&
            s_sprites[i].variant == variant) {
            *out = s_sprites[i].sprite;
            return true;
        }
    }
    return false;
}
