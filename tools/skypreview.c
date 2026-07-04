/* Device-exact sky gradient preview for all weather kinds × dayparts.
 * Build:
 *   cc -std=c11 -Wall -Wextra -lm -I main -o /tmp/skypreview tools/skypreview.c && /tmp/skypreview -o /tmp/sky_grid.ppm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/eva_dither.h"
#include "../main/eva_sky_palette.h"
#include "../main/eva_weather.h"

#define RENDER_W 800
#define RENDER_H 480
#define CELL_W   200
#define CELL_H   120

typedef struct {
    const char *name;
    int minute;
    bool clock_synced;
} daypart_t;

static const daypart_t DAYPARTS[] = {
    { "day",       720,  true  },  /* noon */
    { "sunset",   1080,  true  },  /* 18:00 */
    { "night",    1260,  true  },  /* 21:00 deep night */
    { "sunrise",   360,  true  },  /* 06:00 */
};

static const weather_kind_t KINDS[] = {
    WEATHER_CLEAR_DAY,
    WEATHER_PARTLY_CLOUDY_DAY,
    WEATHER_CLOUDY,
    WEATHER_FOG,
    WEATHER_RAIN,
    WEATHER_HEAVY_RAIN,
    WEATHER_THUNDERSTORM,
    WEATHER_SNOW,
    WEATHER_SLEET,
    WEATHER_HAIL,
    WEATHER_CLEAR_NIGHT,
    WEATHER_PARTLY_CLOUDY_NIGHT,
};

static void write_ppm(const char *path, const uint16_t *buf, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; ++i) {
        uint16_t px = buf[i];
        uint8_t rgb[3] = {
            (uint8_t)(((px >> 11) & 0x1f) * 255 / 31),
            (uint8_t)(((px >> 5) & 0x3f) * 255 / 63),
            (uint8_t)((px & 0x1f) * 255 / 31),
        };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void fill_gradient_dithered(uint16_t *buf, int w, int h,
                                   eva_rgb_t top, eva_rgb_t bot)
{
    const float gamma = 2.2f;
    for (int y = 0; y < h; ++y) {
        float yn = (float)y / (float)(h - 1);
        float t = powf(yn, gamma);
        int ti = (int)(t * 255.0f + 0.5f);
        uint8_t r = (uint8_t)(top.r + (((int)bot.r - top.r) * ti) / 255);
        uint8_t g = (uint8_t)(top.g + (((int)bot.g - top.g) * ti) / 255);
        uint8_t b = (uint8_t)(top.b + (((int)bot.b - top.b) * ti) / 255);
        for (int x = 0; x < w; ++x) {
            buf[y * w + x] = eva_dither565(r, g, b, x, y);
        }
    }
}

static eva_sky_ctx_t ctx_for_daypart(const daypart_t *dp)
{
    eva_sky_ctx_t ctx = {
        .minute = dp->minute,
        .sunrise_min = 360,
        .sunset_min = 1080,
        .cloud_cover = 0.0f,
        .solar_decl_deg = 0.0f,
        .clock_synced = dp->clock_synced,
    };
    ctx.civil_twilight_min = eva_sky_civil_twilight_minutes(ctx.solar_decl_deg);
    return ctx;
}

static void render_grid(const char *out_path, int dump_json)
{
    int cols = (int)(sizeof(DAYPARTS) / sizeof(DAYPARTS[0]));
    int rows = (int)(sizeof(KINDS) / sizeof(KINDS[0]));
    int gw = cols * CELL_W;
    int gh = rows * CELL_H;
    uint16_t *grid = calloc((size_t)gw * (size_t)gh, sizeof(uint16_t));
    uint16_t *cell = malloc((size_t)CELL_W * (size_t)CELL_H * sizeof(uint16_t));
    if (!grid || !cell) { fprintf(stderr, "oom\n"); exit(1); }

    if (dump_json) {
        printf("[\n");
    }

    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            eva_sky_ctx_t ctx = ctx_for_daypart(&DAYPARTS[c]);
            eva_sky_t sky = eva_sky_for_kind(KINDS[r], &ctx);
            fill_gradient_dithered(cell, CELL_W, CELL_H, sky.top, sky.bottom);
            for (int y = 0; y < CELL_H; ++y) {
                memcpy(&grid[(r * CELL_H + y) * gw + c * CELL_W],
                       &cell[y * CELL_W], (size_t)CELL_W * sizeof(uint16_t));
            }
            if (dump_json) {
                int idx = r * cols + c;
                printf("  {\"kind\":%d,\"daypart\":\"%s\",\"top\":[%d,%d,%d],\"bottom\":[%d,%d,%d]}%s\n",
                       (int)KINDS[r], DAYPARTS[c].name,
                       sky.top.r, sky.top.g, sky.top.b,
                       sky.bottom.r, sky.bottom.g, sky.bottom.b,
                       (idx == rows * cols - 1) ? "" : ",");
            }
        }
    }
    if (dump_json) {
        printf("]\n");
    } else {
        write_ppm(out_path, grid, gw, gh);
        printf("wrote %s (%dx%d, %d kinds × %d dayparts)\n", out_path, gw, gh, rows, cols);
    }
    free(cell);
    free(grid);
}

int main(int argc, char **argv)
{
    const char *out = "sky_grid.ppm";
    int dump_json = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-json") == 0) dump_json = 1;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) out = argv[++i];
    }
    render_grid(out, dump_json);
    return 0;
}
