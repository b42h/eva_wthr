/* Shared sky palette — pure header, no ESP includes.
 * Used by eva_weather_canvas.c on device and tools/skypreview.c on host. */
#pragma once

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "eva_weather.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVA_OBSERVER_LAT_DEG   48.915155f
#define EVA_SCENE_EVENING_MIN  105

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} eva_rgb_t;

typedef struct {
    const char *name;
    eva_rgb_t top;
    eva_rgb_t bottom;
} eva_sky_t;

typedef struct {
    int minute;
    int sunrise_min;
    int sunset_min;
    float cloud_cover;          /* 0..1 lifestyle cover */
    float solar_decl_deg;
    float civil_twilight_min;   /* civil-twilight half-duration, minutes */
    bool clock_synced;
} eva_sky_ctx_t;

static inline uint8_t eva_sky_clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline eva_rgb_t eva_sky_lerp_rgb(eva_rgb_t a, eva_rgb_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    eva_rgb_t out = {
        .r = (uint8_t)(a.r + (int)((b.r - a.r) * t)),
        .g = (uint8_t)(a.g + (int)((b.g - a.g) * t)),
        .b = (uint8_t)(a.b + (int)((b.b - a.b) * t)),
    };
    return out;
}

static inline float eva_sky_sun_curve(float progress)
{
    return sinf(progress * 3.1415926f);
}

/* Night-ness 0..1: 0 = sun up, 1 = past civil twilight. */
static inline float eva_sky_nightness(int m, int sr, int ss, float civil_twilight)
{
    if (m >= sr && m <= ss) {
        return 0.0f;
    }
    if (civil_twilight <= 1.0f) return 1.0f;
    float past = (m < sr) ? (float)(sr - m) : (float)(m - ss);
    float n = past / civil_twilight;
    if (n > 1.0f) n = 1.0f;
    return n;
}

static inline float eva_sky_civil_twilight_minutes(float solar_decl_deg)
{
    const float DEG2RAD = 3.14159265f / 180.0f;
    float decl = solar_decl_deg * DEG2RAD;
    float lat  = EVA_OBSERVER_LAT_DEG * DEG2RAD;

    float cos_lat = cosf(lat), sin_lat = sinf(lat);
    float cos_decl = cosf(decl), sin_decl = sinf(decl);
    float denom = cos_lat * cos_decl;
    if (denom < 1e-4f) denom = 1e-4f;

    float c0 = (0.0f - sin_lat * sin_decl) / denom;
    float c6 = (sinf(-6.0f * DEG2RAD) - sin_lat * sin_decl) / denom;
    if (c0 < -1.0f) c0 = -1.0f; else if (c0 > 1.0f) c0 = 1.0f;
    if (c6 < -1.0f) c6 = -1.0f; else if (c6 > 1.0f) c6 = 1.0f;
    float h0 = acosf(c0);
    float h6 = acosf(c6);

    float minutes = (h6 - h0) / DEG2RAD * 4.0f;
    if (minutes < 20.0f) minutes = 20.0f;
    else if (minutes > 180.0f) minutes = 180.0f;
    return minutes;
}

static inline eva_sky_t eva_clear_sky_palette(const eva_sky_ctx_t *ctx)
{
    int m = ctx->minute;
    int sr = ctx->sunrise_min;
    int ss = ctx->sunset_min;
    float cover = ctx->cloud_cover;
    float decl = ctx->solar_decl_deg;

    const eva_rgb_t night_top = {8, 12, 28};
    const eva_rgb_t night_bot = {20, 20, 42};
    const eva_rgb_t twi_top   = {38, 40, 78};
    eva_rgb_t twi_bot_warm    = {255, 150, 96};
    eva_rgb_t twi_bot_muted   = {175, 158, 148};
    eva_rgb_t day_top         = {24, 78, 142};
    eva_rgb_t day_bot         = {120, 178, 224};

    {
        eva_rgb_t day_grey = {72, 88, 102};
        day_top = eva_sky_lerp_rgb(day_top, day_grey, cover * 0.42f);
        day_bot = eva_sky_lerp_rgb(day_bot, day_grey, cover * 0.38f);
        twi_bot_warm = eva_sky_lerp_rgb(twi_bot_warm, twi_bot_muted, cover);
    }

    {
        float season = decl / 23.44f;
        if (season < -1.0f) season = -1.0f;
        else if (season > 1.0f) season = 1.0f;
        int r_shift = (int)(-season * 14.0f);
        int g_shift = (int)(season * 10.0f);
        int b_shift = (int)(season * 6.0f);
        twi_bot_warm.r = eva_sky_clamp_u8((int)twi_bot_warm.r + r_shift);
        twi_bot_warm.g = eva_sky_clamp_u8((int)twi_bot_warm.g + g_shift);
        twi_bot_warm.b = eva_sky_clamp_u8((int)twi_bot_warm.b + b_shift);
    }

    const eva_rgb_t twi_bot = twi_bot_warm;
    eva_rgb_t top, bot;

    if (m >= sr && m <= ss) {
        float daylight = (float)(ss - sr);
        if (daylight < 1.0f) daylight = 1.0f;
        float progress = (float)(m - sr) / daylight;
        float elev = eva_sky_sun_curve(progress);
        if (m >= ss - EVA_SCENE_EVENING_MIN) {
            float evening = (float)(ss - m) / (float)EVA_SCENE_EVENING_MIN;
            if (evening < 0.0f) evening = 0.0f;
            if (evening > 1.0f) evening = 1.0f;
            elev *= (0.22f + 0.78f * evening);
        }
        float t = elev * elev * (3.0f - 2.0f * elev);
        top = eva_sky_lerp_rgb(twi_top, day_top, t);
        bot = eva_sky_lerp_rgb(twi_bot, day_bot, t);
        if (m >= ss - EVA_SCENE_EVENING_MIN) {
            float evening = (float)(ss - m) / (float)EVA_SCENE_EVENING_MIN;
            if (evening > 1.0f) evening = 1.0f;
            top = eva_sky_lerp_rgb(top, twi_top, 0.55f * (1.0f - evening));
            bot = eva_sky_lerp_rgb(bot, twi_bot, 0.72f * (1.0f - evening));
        }
    } else {
        float t = eva_sky_nightness(m, sr, ss, ctx->civil_twilight_min);
        top = eva_sky_lerp_rgb(twi_top, night_top, t);
        bot = eva_sky_lerp_rgb(twi_bot, night_bot, t);
    }
    return (eva_sky_t){ "clear-cycle", top, bot };
}

static inline eva_sky_t eva_sky_for_kind(weather_kind_t kind, const eva_sky_ctx_t *ctx)
{
    int m = ctx->minute;
    int sr = ctx->sunrise_min;
    int ss = ctx->sunset_min;

    if (!ctx->clock_synced &&
        (kind == WEATHER_CLEAR_NIGHT || kind == WEATHER_PARTLY_CLOUDY_NIGHT)) {
        return (eva_sky_t){ "night", {8, 12, 28}, {20, 20, 42} };
    }

    {
        eva_rgb_t day_top, day_bot, night_top, night_bot;
        const char *tag;
        bool matched = true;
        switch (kind) {
        case WEATHER_RAIN:
            tag = "rain";
            day_top   = (eva_rgb_t){ 42,  52,  68}; day_bot   = (eva_rgb_t){ 96, 108, 118};
            night_top = (eva_rgb_t){ 16,  20,  28}; night_bot = (eva_rgb_t){ 36,  42,  52};
            break;
        case WEATHER_SLEET:
            tag = "sleet";
            day_top   = (eva_rgb_t){ 54,  62,  78}; day_bot   = (eva_rgb_t){118, 128, 140};
            night_top = (eva_rgb_t){ 20,  24,  34}; night_bot = (eva_rgb_t){ 44,  50,  62};
            break;
        case WEATHER_HEAVY_RAIN:
            tag = "heavy-rain";
            day_top   = (eva_rgb_t){ 18,  26,  40}; day_bot   = (eva_rgb_t){ 52,  60,  74};
            night_top = (eva_rgb_t){ 10,  14,  22}; night_bot = (eva_rgb_t){ 26,  32,  42};
            break;
        case WEATHER_THUNDERSTORM:
            tag = "thunderstorm";
            /* Day base lifted 2026-07-03 (user: near-black daytime storm read
             * as a dimmed panel). Dramatic grey, not midnight blue; lightning
             * contrast comes from the flash boost, not a black screen. */
            day_top   = (eva_rgb_t){ 52,  60,  76}; day_bot   = (eva_rgb_t){108, 112, 124};
            night_top = (eva_rgb_t){  8,  12,  20}; night_bot = (eva_rgb_t){ 22,  26,  36};
            break;
        case WEATHER_SNOW:
            tag = "snow";
            day_top   = (eva_rgb_t){116, 132, 150}; day_bot   = (eva_rgb_t){205, 214, 220};
            night_top = (eva_rgb_t){ 14,  18,  28}; night_bot = (eva_rgb_t){ 32,  38,  52};
            break;
        case WEATHER_HAIL:
            tag = "hail";
            day_top   = (eva_rgb_t){ 72,  82,  98}; day_bot   = (eva_rgb_t){148, 156, 168};
            night_top = (eva_rgb_t){ 12,  16,  26}; night_bot = (eva_rgb_t){ 28,  34,  48};
            break;
        case WEATHER_FOG:
            tag = "fog";
            day_top   = (eva_rgb_t){130, 138, 145}; day_bot   = (eva_rgb_t){215, 216, 210};
            night_top = (eva_rgb_t){ 24,  28,  36}; night_bot = (eva_rgb_t){ 46,  50,  60};
            break;
        case WEATHER_CLOUDY:
            tag = "cloudy";
            day_top   = (eva_rgb_t){ 78,  92, 108}; day_bot   = (eva_rgb_t){150, 160, 170};
            night_top = (eva_rgb_t){ 16,  20,  28}; night_bot = (eva_rgb_t){ 34,  40,  52};
            break;
        default:
            matched = false;
            tag = "";
            day_top = day_bot = night_top = night_bot = (eva_rgb_t){0, 0, 0};
            break;
        }
        if (matched) {
            float n = eva_sky_nightness(m, sr, ss, ctx->civil_twilight_min);
            if (n > 0.45f) {
                n = n * 1.12f;
                if (n > 1.0f) n = 1.0f;
            }
            return (eva_sky_t){ tag,
                                eva_sky_lerp_rgb(day_top, night_top, n),
                                eva_sky_lerp_rgb(day_bot, night_bot, n) };
        }
    }
    return eva_clear_sky_palette(ctx);
}

#ifdef __cplusplus
}
#endif
