/* .clm header parsing + A8 mask resampling (pure, host-testable).
 * Used by eva_cloud_assets.c on device and tools/test_clm_scale.c on host. */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint16_t w;
    uint16_t h;
    uint32_t comp_size[3];   /* light, shadow, core */
} clm_header_t;

#define CLM_HEADER_BYTES 20

static inline bool clm_parse_header(const uint8_t *buf, size_t len,
                                    clm_header_t *out)
{
    if (len < CLM_HEADER_BYTES || memcmp(buf, "CLM1", 4) != 0) return false;
    out->w = (uint16_t)(buf[4] | (buf[5] << 8));
    out->h = (uint16_t)(buf[6] | (buf[7] << 8));
    for (int i = 0; i < 3; ++i) {
        const uint8_t *p = &buf[8 + i * 4];
        out->comp_size[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                            ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    return out->w > 0 && out->h > 0;
}

/* Bilinear resample of an A8 mask from src (sw×sh) into dst (dw×dh).
 * `dst_stride` is the physical row pitch of dst in bytes; pass dw when the
 * rows are packed. A larger stride lets the caller keep spare columns after
 * each row (the device duplicates the first screen-width there so a scrolled
 * window never needs a second, wrapped PPA band).
 * `scale` > 1 magnifies around the centre ("clouds closer"); X wraps
 * (strips tile seamlessly in X), Y clamps (strip edges are feathered).
 * `mirror_x` flips the output horizontally. scale==1 with equal dims and
 * no mirror is a straight copy. */
static inline void clm_scale_mask_stride(const uint8_t *src, int sw, int sh,
                                         uint8_t *dst, int dw, int dh,
                                         int dst_stride,
                                         float scale, bool mirror_x)
{
    if (dst_stride < dw) dst_stride = dw;
    if (sw == dw && sh == dh && !mirror_x && fabsf(scale - 1.0f) < 1e-3f) {
        if (dst_stride == dw) {
            memcpy(dst, src, (size_t)sw * (size_t)sh);
        } else {
            for (int y = 0; y < sh; ++y) {
                memcpy(&dst[(size_t)y * dst_stride], &src[(size_t)y * sw], (size_t)sw);
            }
        }
        return;
    }
    /* Total source step per dst pixel: resolution ratio / depth scale. */
    float step_x = ((float)sw / (float)dw) / scale;
    float step_y = ((float)sh / (float)dh) / scale;
    float cx = (float)sw * 0.5f, cy = (float)sh * 0.5f;
    /* A scaled resample of an X-periodic source is no longer dw-periodic,
     * so the runtime scroll wrap would show a hard vertical "texture edge"
     * at the dst seam. Crossfade the first EVA_CLM_SEAM_PX columns between
     * the normal sample in(x) and the tail continuation in(x + dw) — the
     * source sampling wraps, so in(x + dw) is smooth — which makes
     * dst[dw-1] flow into dst[0]. Only needed when scale shifts the period
     * (a pure resolution ratio keeps dst periodic). */
#define EVA_CLM_SEAM_PX 64
    int seam_n = (fabsf(scale - 1.0f) > 1e-3f && dw > 2 * EVA_CLM_SEAM_PX)
                 ? EVA_CLM_SEAM_PX : 0;
    for (int y = 0; y < dh; ++y) {
        float sy = cy + ((float)y - (float)dh * 0.5f) * step_y;
        int y0 = (int)floorf(sy);
        float fy = sy - (float)y0;
        int y1 = y0 + 1;
        if (y0 < 0) { y0 = 0; y1 = 0; fy = 0.0f; }
        if (y1 >= sh) { y1 = sh - 1; if (y0 > y1) y0 = y1; fy = 0.0f; }
        const uint8_t *r0 = &src[y0 * sw];
        const uint8_t *r1 = &src[y1 * sw];
        uint8_t *drow = &dst[(size_t)y * dst_stride];
        for (int x = 0; x < dw; ++x) {
            float xs = (float)x;
            float sx = cx + (xs - (float)dw * 0.5f) * step_x;
            float sxf = floorf(sx);
            float fx = sx - sxf;
            int x0 = (int)sxf % sw;
            if (x0 < 0) x0 += sw;
            int x1 = x0 + 1;
            if (x1 >= sw) x1 = 0;
            float top = (float)r0[x0] + ((float)r0[x1] - (float)r0[x0]) * fx;
            float bot = (float)r1[x0] + ((float)r1[x1] - (float)r1[x0]) * fx;
            float v = top + (bot - top) * fy;
            if (x < seam_n) {
                /* Tail continuation sample at x + dw. */
                float sx2 = cx + (xs + (float)dw - (float)dw * 0.5f) * step_x;
                float sxf2 = floorf(sx2);
                float fx2 = sx2 - sxf2;
                int x20 = (int)sxf2 % sw;
                if (x20 < 0) x20 += sw;
                int x21 = x20 + 1;
                if (x21 >= sw) x21 = 0;
                float top2 = (float)r0[x20] + ((float)r0[x21] - (float)r0[x20]) * fx2;
                float bot2 = (float)r1[x20] + ((float)r1[x21] - (float)r1[x20]) * fx2;
                float v2 = top2 + (bot2 - top2) * fy;
                float t = xs / (float)seam_n;
                t = t * t * (3.0f - 2.0f * t);
                v = v2 + (v - v2) * t;
            }
            drow[mirror_x ? (dw - 1 - x) : x] = (uint8_t)(v + 0.5f);
        }
    }
#undef EVA_CLM_SEAM_PX
}

/* Packed-row convenience wrapper (dst_stride == dw). */
static inline void clm_scale_mask(const uint8_t *src, int sw, int sh,
                                  uint8_t *dst, int dw, int dh,
                                  float scale, bool mirror_x)
{
    clm_scale_mask_stride(src, sw, sh, dst, dw, dh, dw, scale, mirror_x);
}
