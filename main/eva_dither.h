/* Ordered Bayer 8×8 dithering for RGB565 sky gradients — pure header.
 * Used by eva_weather_canvas.c on device and tools/skypreview.c on host. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static const int8_t EVA_BAYER8[64] = {
     0, 48, 12, 60,  3, 51, 15, 63,
    32, 16, 44, 28, 35, 19, 47, 31,
     8, 56,  4, 52, 11, 59,  7, 55,
    40, 24, 36, 20, 43, 27, 39, 23,
     2, 50, 14, 62,  1, 49, 13, 61,
    34, 18, 46, 30, 33, 17, 45, 29,
    10, 58,  6, 54,  9, 57,  5, 53,
    42, 26, 38, 22, 41, 25, 37, 21,
};

static inline uint8_t eva_dither_u8(uint8_t c, int x, int y, int step)
{
    int t = (int)EVA_BAYER8[(y & 7) * 8 + (x & 7)];
    int v = (int)c + ((t - 32) * step + 32) / 64;
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline uint16_t eva_dither565(uint8_t r8, uint8_t g8, uint8_t b8, int x, int y)
{
    uint8_t r = eva_dither_u8(r8, x, y, 8);
    uint8_t g = eva_dither_u8(g8, x, y, 4);
    uint8_t b = eva_dither_u8(b8, x, y, 8);
    return (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                      ((uint16_t)(g & 0xfc) << 3) |
                      ((uint16_t)b >> 3));
}

#ifdef __cplusplus
}
#endif
