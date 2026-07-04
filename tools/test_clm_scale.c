/* Host test: .clm header parse + bilinear scale/mirror resampler.
 * Build: cc -std=c11 -Wall -Wextra -lm -o /tmp/test_clm_scale tools/test_clm_scale.c && /tmp/test_clm_scale
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../main/eva_cloud_scale.h"

static void test_header_parse(void)
{
    uint8_t hdr[20] = { 'C', 'L', 'M', '1',
                        0x20, 0x03,   /* w = 800 */
                        0x00, 0x03,   /* h = 768 */
                        1, 0, 0, 0,  2, 0, 0, 0,  3, 0, 0, 0 };
    clm_header_t h;
    assert(clm_parse_header(hdr, sizeof hdr, &h));
    assert(h.w == 800 && h.h == 768);
    assert(h.comp_size[0] == 1 && h.comp_size[1] == 2 && h.comp_size[2] == 3);
    hdr[3] = '2';
    assert(!clm_parse_header(hdr, sizeof hdr, &h));
}

static void test_scale_identity(void)
{
    enum { W = 64, H = 48 };
    static uint8_t src[W * H], dst[W * H];
    for (int i = 0; i < W * H; ++i) src[i] = (uint8_t)(i * 7u);
    clm_scale_mask(src, W, H, dst, W, H, 1.0f, false);
    assert(memcmp(src, dst, sizeof src) == 0);
}

static void test_scale_mirror(void)
{
    enum { W = 8, H = 2 };
    uint8_t src[W * H] = { 0 }, dst[W * H];
    src[0] = 200;                       /* leftmost pixel of row 0 */
    clm_scale_mask(src, W, H, dst, W, H, 1.0f, true);
    assert(dst[W - 1] == 200 && dst[0] == 0);
}

static void test_scale_zoom_centred(void)
{
    enum { W = 100, H = 100 };
    static uint8_t src[W * H], dst[W * H];
    memset(src, 0, sizeof src);
    src[50 * W + 50] = 255;             /* centre pixel */
    clm_scale_mask(src, W, H, dst, W, H, 1.25f, false);
    /* Zoom is centred → the bright spot stays near the centre and spreads. */
    int best_x = -1, best_y = -1, best = -1;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
            if (dst[y * W + x] > best) { best = dst[y * W + x]; best_x = x; best_y = y; }
    assert(best > 100);
    assert(abs(best_x - 50) <= 2 && abs(best_y - 50) <= 2);
}

static void test_scale_x_wraps(void)
{
    enum { W = 100, H = 10 };
    static uint8_t src[W * H], dst[W * H];
    memset(src, 0, sizeof src);
    for (int y = 0; y < H; ++y) src[y * W + 0] = 255;   /* bright seam column */
    clm_scale_mask(src, W, H, dst, W, H, 0.8f, false);  /* zoom OUT pulls in wrapped content */
    /* No hard zero gap where the wrap happened: the seam column must still
     * exist somewhere with high intensity. */
    int maxv = 0;
    for (int i = 0; i < W * H; ++i) if (dst[i] > maxv) maxv = dst[i];
    assert(maxv > 150);
}

/* The runtime scrolls the strip and wraps it at W: dst column W-1 sits next
 * to dst column 0 on screen. A scaled resample of a periodic source is no
 * longer W-periodic, so without seam blending a hard vertical "texture edge"
 * appears at every wrap (seen on hardware 2026-07-03 once the light plane
 * became dense). The seam step must stay comparable to the interior step. */
static void test_scale_wrap_seam_continuity(void)
{
    enum { W = 200, H = 40 };
    static uint8_t src[W * H], dst[W * H];
    /* Smooth periodic but ASYMMETRIC source (two harmonics, phase-shifted) —
     * a symmetric wave hides the seam because the two sample points land on
     * mirror-equal values. */
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            double ph = 2.0 * 3.14159265358979 * (double)x / (double)W;
            double v = 96.0 * (1.0 + __builtin_sin(ph)) +
                       48.0 * (1.0 + __builtin_sin(2.0 * ph + 1.3));
            src[y * W + x] = (uint8_t)(v * 255.0 / 288.0 + 0.5);
        }
    }
    for (int s = 0; s < 3; ++s) {
        float scale = (s == 0) ? 0.85f : (s == 1) ? 1.10f : 1.25f;
        clm_scale_mask(src, W, H, dst, W, H, scale, false);
        double seam = 0.0, interior = 0.0;
        for (int y = 0; y < H; ++y) {
            seam += abs((int)dst[y * W + (W - 1)] - (int)dst[y * W + 0]);
            for (int x = 1; x < W; ++x) {
                interior += abs((int)dst[y * W + x] - (int)dst[y * W + x - 1]);
            }
        }
        seam /= H;
        interior /= (double)H * (W - 1);
        assert(seam < interior * 4.0 + 2.0);
    }
}

int main(void)
{
    test_header_parse();
    test_scale_identity();
    test_scale_mirror();
    test_scale_zoom_centred();
    test_scale_x_wraps();
    test_scale_wrap_seam_continuity();
    printf("OK\n");
    return 0;
}
