/* Host test: Bayer RGB565 dither determinism and grey-ramp behaviour.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_dither tools/test_dither.c && /tmp/test_dither
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../main/eva_dither.h"

static uint16_t pack_plain565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                      ((uint16_t)(g & 0xfc) << 3) |
                      ((uint16_t)b >> 3));
}

static void unpack565(uint16_t px, int *r, int *g, int *b)
{
    *r = ((px >> 11) & 0x1f) * 255 / 31;
    *g = ((px >> 5) & 0x3f) * 255 / 63;
    *b = (px & 0x1f) * 255 / 31;
}

static void test_deterministic(void)
{
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            uint16_t a = eva_dither565(40, 80, 120, x, y);
            uint16_t b = eva_dither565(40, 80, 120, x, y);
            assert(a == b);
        }
    }
}

static void test_mid_grey_checker(void)
{
    int levels[256] = {0};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            uint16_t px = eva_dither565(128, 128, 128, x, y);
            int r, g, b;
            unpack565(px, &r, &g, &b);
            levels[r]++;
            assert(abs(g - r) <= 8);
        }
    }
    int distinct = 0;
    for (int i = 0; i < 256; ++i) if (levels[i]) distinct++;
    assert(distinct >= 2);
}

static void test_no_clipping(void)
{
    assert(eva_dither565(0, 0, 0, 3, 5) == pack_plain565(0, 0, 0));
    uint16_t hi = eva_dither565(255, 255, 255, 7, 7);
    int r, g, b;
    unpack565(hi, &r, &g, &b);
    assert(r >= 240 && g >= 240 && b >= 240);
}

static void test_block_average_beats_plain(void)
{
    const int W = 8, H = 8;
    const int tr = 37, tg = 73, tb = 109;
    double plain_sum[3] = {0}, dith_sum[3] = {0};
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            uint16_t plain = pack_plain565((uint8_t)tr, (uint8_t)tg, (uint8_t)tb);
            uint16_t dith = eva_dither565((uint8_t)tr, (uint8_t)tg, (uint8_t)tb, x, y);
            int pr, pg, pb, dr, dg, db;
            unpack565(plain, &pr, &pg, &pb);
            unpack565(dith, &dr, &dg, &db);
            plain_sum[0] += pr; plain_sum[1] += pg; plain_sum[2] += pb;
            dith_sum[0] += dr; dith_sum[1] += dg; dith_sum[2] += db;
        }
    }
    double plain_err = 0.0, dith_err = 0.0;
    for (int i = 0; i < 3; ++i) {
        double tgt = (i == 1) ? tg : (i == 0 ? tr : tb);
        plain_err += fabs(plain_sum[i] / (W * H) - tgt);
        dith_err += fabs(dith_sum[i] / (W * H) - tgt);
    }
    assert(dith_err <= plain_err + 0.5);
}

int main(void)
{
    test_deterministic();
    test_mid_grey_checker();
    test_no_clipping();
    test_block_average_beats_plain();
    printf("OK\n");
    return 0;
}
