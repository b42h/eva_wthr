#include "eva_weather_canvas.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ppa.h"

/* --- Fibonacci timing core ------------------------------------------------
 * Every cadence in Eva is expressed in Fibonacci numbers so animations share
 * the same golden rhythm and don't accidentally lock into beats. Keep this
 * table in sync with `main/main.c` (the eye/clock side already uses it).
 *
 * Where a value must NOT be Fibonacci (PSRAM block sizes, power-of-two
 * masks, hardware DMA alignment), it stays a plain literal and the comment
 * notes why. */
#define FIB_1     1
#define FIB_2     2
#define FIB_3     3
#define FIB_5     5
#define FIB_8     8
#define FIB_13    13
#define FIB_21    21
#define FIB_34    34
#define FIB_55    55
#define FIB_89    89
#define FIB_144   144
#define FIB_233   233
#define FIB_377   377
#define FIB_610   610

#define PARTICLE_MAX 512        /* power-of-two pool, hardware-friendly DMA. */
#define TIMER_MS    FIB_13      /* 13 ms tick ≈ 77 Hz target. LVGL flush caps real rate. */
#define LOG_EVERY_FRAMES FIB_21 /* ~270 ms between FPS log lines while tuning. */
/* Native panel resolution — render straight into the display buffer to avoid
 * a separate upscale pass (~8 ms on the previous 400×240→800×480 SRM). */
#define EVA_WEATHER_RENDER_W 800
#define EVA_WEATHER_RENDER_H 480
#define EVA_PHI 1.6180339f
#define EVA_PHI2 (EVA_PHI * EVA_PHI)
#define EVA_INV_PHI 0.61803399f

/* --- PPA-composited clouds ------------------------------------------------
 * Three cloud layers keep two A8 geometry variants each. Every variant has
 * light/shadow/core masks, tinted and alpha-blended over the cached sky with
 * the P4 PPA blend engine. Variants crossfade and rebake over time so clouds
 * appear, dissolve, and reform instead of looping as one static strip. */
#define CLOUD_STRIP_W 800
#define CLOUD_LAYER_HIGH 0
#define CLOUD_LAYER_MID  1
#define CLOUD_LAYER_LOW  2
#define CLOUD_LAYER_COUNT 3
#define CLOUD_VARIANT_COUNT 2

typedef enum {
    P_NONE = 0,
    P_RAIN,
    P_SNOW,
    P_HAIL,
    P_STAR,
    P_DUST,
    P_FOG,
} particle_kind_t;

typedef struct {
    particle_kind_t kind;
    float x;
    float y;
    float vx;
    float vy;
    float size;
    float alpha;
    float phase;
    float spin;
    uint8_t layer;
} particle_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    const char *name;
    rgb_t top;
    rgb_t bottom;
} sky_t;

typedef struct {
    uint8_t *a8_light;
    uint8_t *a8_shadow;
    uint8_t *a8_core;
} cloud_variant_t;

/* Multi-mask cloud strip — A8 textures per layer for top-down lighting and
 * slow shape lifecycle.
 *
 *   a8_light  — top portion of each blob (sun-facing side). Painted with
 *               warm/bright tint in `tint_light_*` to simulate direct sunlight.
 *   a8_shadow — bottom portion of each blob (anti-sun, downward-facing).
 *               Painted with cooler/darker tint in `tint_shadow_*` to simulate
 *               the cloud's self-shadow + lack of sky scattering on the underside.
 *   a8_core   — dense centre/belly. Painted darker to make clouds read as
 *               volumes rather than translucent stickers.
 *
 * Each strip owns two variants. Only one is normally drawn; during a periodic
 * lifecycle window the next variant crossfades in, then the old one is rebaked
 * off-screen. This makes individual puffs appear/disappear without changing
 * the live clearoutside cloud percentages.
 *
 * Reference: looking at a real partly-cloudy day, cumulus tops are nearly
 * white from direct sun, sides are mid-grey from Rayleigh sky reflection,
 * bottoms are dark grey because almost no light reaches them. Storm clouds
 * (cumulonimbus) take this further — their bases are nearly black even at
 * midday because anvil depth blocks all overhead light. */
typedef struct {
    int y_start;
    int strip_h;
    cloud_variant_t variant[CLOUD_VARIANT_COUNT];
    uint8_t active_variant;
    bool morphing;
    float morph_t;
    float morph_clock;
    float morph_hold_s;
    float morph_duration_s;
    float scroll_x;
    float scroll_y_off;   /* small vertical bob driven by wind + phase, in px */
    float base_speed;
    /* Light side (top): bright/warm in day, dim cool in night. */
    uint8_t tint_light_r;
    uint8_t tint_light_g;
    uint8_t tint_light_b;
    /* Shadow side (bottom): dark grey storm, mid grey day, very dark night. */
    uint8_t tint_shadow_r;
    uint8_t tint_shadow_g;
    uint8_t tint_shadow_b;
    /* Dense inner body/belly: subtle in fair weather, strong in storms. */
    uint8_t tint_core_r;
    uint8_t tint_core_g;
    uint8_t tint_core_b;
    uint8_t alpha_scale;
} cloud_strip_t;

static const char *TAG = "eva_canvas";
LV_FONT_DECLARE(eva_font_clock_144_extralight);
LV_FONT_DECLARE(eva_font_uk_22);

static lv_obj_t *s_canvas;
static lv_timer_t *s_timer;
static uint16_t *s_buf;
static uint16_t *s_bg_buf;
static uint16_t *s_composite_buf;
static uint16_t *s_display_buf;
static uint16_t *s_render_buf;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_dpi_fb[2];
static uint16_t *s_dpi_scan_fb;
static uint16_t *s_dpi_back_fb;
static SemaphoreHandle_t s_ppa_done_sem;
static SemaphoreHandle_t s_vsync_sem;
static TaskHandle_t s_render_task;
static portMUX_TYPE s_frame_mux = portMUX_INITIALIZER_UNLOCKED;
static ppa_client_handle_t s_ppa_srm;
static ppa_client_handle_t s_ppa_blend;
static bool s_ppa_disabled;
static bool s_ppa_blend_disabled;
static weather_kind_t s_kind = WEATHER_CLOUDY;
static weather_kind_t s_prev_kind = WEATHER_UNKNOWN;
static volatile int s_time_offset_hours;

/* Forward declaration — needed by draw_scene_text_overlays() above the
 * actual definition further down. */
static bool is_night_kind(weather_kind_t kind);
static float s_density_scale = 1.0f;
static particle_t s_particles[PARTICLE_MAX];
static cloud_strip_t s_strip[CLOUD_LAYER_COUNT] = {
    /* Calm-air base drift speeds in px/s — chosen from the Fibonacci ladder so
     * the three layers move at golden-ratio offsets. HIGH cirrus barely moves,
     * MID altocumulus drifts visibly, LOW cumulus is the parallax foreground.
     * Final speed = base * wind_factor(kph) and the LOW layer takes a larger
     * multiplier under heavy wind (cumulus catches gusts more than cirrus). */
    /* Vertical layout for native 480 px canvas. Migrated from the old 240 px
     * render space where layers spanned y=10..200; ×2 keeps the same relative
     * positions but fills the full screen instead of leaving the bottom half
     * empty.
     *   HIGH cirrus:        y =  20..120  (top of sky)
     *   MID altocumulus:    y = 100..260  (middle band, overlaps slightly)
     *   LOW cumulus:        y = 220..400  (foreground / horizon)
     * Combined coverage:    y =  20..400 (out of 480) — leaves ~80 px below
     * the lowest layer for the ground line / horizon haze. */
    [CLOUD_LAYER_HIGH] = {
        .y_start = 20, .strip_h = 100, .base_speed = (float)FIB_2,
        .morph_hold_s = (float)FIB_89, .morph_duration_s = (float)FIB_13,
    },
    [CLOUD_LAYER_MID] = {
        .y_start = 100, .strip_h = 160, .base_speed = (float)FIB_5,
        .morph_hold_s = (float)FIB_55, .morph_duration_s = (float)FIB_13,
    },
    [CLOUD_LAYER_LOW] = {
        .y_start = 220, .strip_h = 180, .base_speed = (float)FIB_13,
        .morph_hold_s = (float)FIB_34, .morph_duration_s = (float)FIB_21,
    },
};
/* Sun position cache — populated by draw_sun_or_moon() in the bg-cache pass,
 * read by draw_sun_god_rays() after the cloud composite. Lets sunlight bleed
 * THROUGH the cloud cover instead of being permanently shadowed by it. */
static int s_sun_x = -1;
static int s_sun_y = -1;
static int s_sun_r = 0;
static bool s_sun_visible = false;
static float s_sun_strength = 0.0f;

static uint16_t s_target = 120;
static uint16_t s_max_target = 160;
static uint32_t s_rng = 0x4880e5a5U;
static int64_t s_last_us;
static uint32_t s_frames;
static int64_t s_accum_us;
static int64_t s_accum_tick_us;
static int64_t s_last_frame_us;
static int64_t s_last_tick_exit_us;
static int64_t s_accum_lvgl_slot_us;
static uint8_t s_over_budget;
static uint8_t s_under_budget;
static float s_lightning_alpha;
static uint8_t s_lightning_cooldown;
static int16_t s_lightning_x[6];
static int16_t s_lightning_y[6];
static uint8_t s_bg_ttl;
static float s_bg_dt;
static uint8_t s_composite_ttl;
static float s_composite_dt;
static bool s_visible;
/* Sun event minutes-of-day from clearoutside astronomy. -1 = unknown -> fall back to
 * hardcoded 6:00 / 18:00 used in the original time-of-day spec. The window
 * around each event is fixed at 60 minutes (sunrise: [-60,+60] etc). */
static int16_t s_sunrise_min  = -1;
static int16_t s_sunset_min   = -1;
static int16_t s_moonrise_min = -1;
static int16_t s_moonset_min  = -1;
static uint8_t s_moon_phase_pct;   /* 0..100 illumination */
static uint8_t s_moon_waning;      /* 1 if waning, 0 otherwise */
/* Cloud coverage per layer (0..100). Updated by set_kind defaults and live
 * weather state. Used by both cloud strip compositor and the sky-cover
 * computation that fades sun/moon visibility. */
static uint8_t s_cloud_pct[3] = {0, 0, 0};
/* Test-mode overrides: -1 = no override, 0..100 = forced value.
 * Used by the test sliders panel in main.c. */
static int16_t s_test_cloud_pct_override[3] = {-1, -1, -1};
static int16_t s_test_wind_kph_override = -1;
/* Lifestyle cloud cover from open-meteo. Drives sky_cover_fraction().
 * 0 = clear, 100 = overcast. Separate from s_cloud_pct[] which is the
 * three-layer visual breakdown used for bake_strip. */
static uint8_t s_cloud_cover_pct = 0;
static uint8_t s_fog_pct = 0;
/* Wind state, fed by eva_weather_canvas_set_weather. Used to bias rain/snow
 * particle horizontal velocity so heavy crosswind visibly slants the streaks.
 * Direction is "from" (meteorological convention) — wind FROM the West means
 * particles drift EAST (positive vx) on screen. */
static float s_wind_vx_bias = 0.0f;   /* px/s, signed */
static float s_wind_kph_eff = 0.0f;   /* magnitude for snow wobble amplitude */
#define SUN_WINDOW_MIN FIB_55   /* sunrise/sunset transition window, ~55 min. */
static int64_t s_prof_bg_us;
static int64_t s_prof_clouds_us;
static int64_t s_prof_particles_us;
static int64_t s_prof_lightning_us;
static char s_clock_text[16] = "00:00";
static char s_temp_text[16] = "+0C";
static char s_desc_text[96] = "";
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;

/* Pre-baked A8 text overlays.
 *
 * The three on-canvas labels (clock, temp, desc) change at most once per
 * minute, but draw_scene_text_overlays() was running the full per-glyph
 * decode + scaled blit on EVERY frame (17-60 Hz). Each clock frame walked
 * 5 glyphs × ~7000 dst pixels with nearest-neighbour scaling — pure waste
 * since the result was identical for ~1000 consecutive frames.
 *
 * Cache strategy:
 *   - One A8 mask per slot, sized to the worst-case rendered footprint.
 *   - bake_text_slot() rebuilds the mask only when the input text or scale
 *     changes (cache key = text + scale_q8). The bake reuses the existing
 *     per-glyph decoder but writes into the slot's A8 buffer instead of the
 *     RGB565 render buffer, so there's no colour/alpha at bake time.
 *   - blit_text_slot() composites the cached A8 onto s_buf with the
 *     requested colour+alpha. This is a single linear pass over the slot's
 *     footprint, no glyph decoding.
 *
 * Storage: 86 KB per slot worst case (720×120 A8) × 3 slots = ~260 KB PSRAM.
 * Cheap vs the gain (≈10 ms/frame off render_weather hot path). */
#define TEXT_SLOT_BUF_W   720
#define TEXT_SLOT_BUF_H   120
#define TEXT_SLOT_BUF_BYTES (TEXT_SLOT_BUF_W * TEXT_SLOT_BUF_H)

typedef struct {
    uint8_t *a8;            /* allocated in PSRAM, TEXT_SLOT_BUF_BYTES */
    uint16_t mask_w;        /* tight bounding width  (≤ TEXT_SLOT_BUF_W) */
    uint16_t mask_h;        /* tight bounding height (≤ TEXT_SLOT_BUF_H) */
    char key_text[96];      /* last baked text */
    uint16_t key_scale_q8;  /* last baked scale */
    bool valid;
} text_slot_t;

static text_slot_t s_clock_slot;
static text_slot_t s_temp_slot;
static text_slot_t s_desc_slot;

/* Static glyph draw buffer for draw_text_utf8.
 *
 * Root cause: lv_font_get_bitmap_fmt_txt() does `bitmap_out = draw_buf->data`
 * with no NULL check. Passing NULL as draw_buf dereferences NULL → crash.
 * LVGL labels normally work because LVGL's label widget allocates a draw_buf
 * via the draw layer; our render task lives outside LVGL and must supply one.
 *
 * Size: largest expected glyph is eva_font_clock_144_extralight (~102×102 A8
 * ≈ 10.4 KB). 160×160 = 25600 bytes gives comfortable headroom for any
 * accented Cyrillic glyphs and montserrat_48 (~48×48). */
#define GLYPH_BUF_W      160
#define GLYPH_BUF_H      160
#define GLYPH_BUF_BYTES  (GLYPH_BUF_W * GLYPH_BUF_H)  /* A8 = 1 byte/px */
static uint8_t  s_glyph_raw[GLYPH_BUF_BYTES] WORD_ALIGNED_ATTR;
static lv_draw_buf_t s_glyph_draw_buf;

/* Pre-baked cloud sprite atlas: 8 shapes, each 200×100 A8 mask.
 * Allocated once in PSRAM at init and never freed (static lifetime).
 * A8 format: 1 byte per pixel, 0=transparent → 255=opaque. */
typedef struct {
    uint8_t *a8_data;           /* 200×100 = 20000 bytes per sprite */
    uint8_t base_r, base_g, base_b;  /* tint for daytime rendering */
} cloud_sprite_t;

#define CLOUD_SPRITE_W 200
#define CLOUD_SPRITE_H 100
#define CLOUD_SPRITE_BYTES (CLOUD_SPRITE_W * CLOUD_SPRITE_H)
#define CLOUD_SPRITE_ATLAS_COUNT 8
static cloud_sprite_t s_cloud_atlas[CLOUD_SPRITE_ATLAS_COUNT];
static bool s_cloud_atlas_inited;

typedef struct {
    float x;        /* normalized, wrap-safe range roughly -0.25..1.25 */
    float y;        /* 0 = horizon, 1 = viewer */
    float scale;    /* 0.3 horizon, ~2.0 near viewer */
    float vx;       /* normalized lateral drift */
    uint8_t alpha;
    uint8_t shape_id;  /* index into s_cloud_atlas[0..7] */
    uint8_t seed;
} cloud3d_t;

#define CLOUD_3D_MAX 24
static cloud3d_t s_clouds3d[CLOUD_3D_MAX];
static bool s_clouds3d_inited;
static uint8_t s_clouds3d_active = CLOUD_3D_MAX;

static uint32_t rnd_u32(void)
{
    s_rng = s_rng * 1664525U + 1013904223U;
    return s_rng;
}

static float rndf(float lo, float hi)
{
    return lo + (hi - lo) * ((float)(rnd_u32() & 0xffffU) / 65535.0f);
}

static uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((uint16_t)(r & 0xf8) << 8) |
                      ((uint16_t)(g & 0xfc) << 3) |
                      ((uint16_t)b >> 3));
}

static uint16_t rgb565_from(rgb_t c)
{
    return rgb565(c.r, c.g, c.b);
}

static uint16_t blend565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    if (alpha == 0) return dst;
    if (alpha == 255) return src;

    int sr = (src >> 11) & 0x1f;
    int sg = (src >> 5) & 0x3f;
    int sb = src & 0x1f;
    int dr = (dst >> 11) & 0x1f;
    int dg = (dst >> 5) & 0x3f;
    int db = dst & 0x1f;

    dr += ((sr - dr) * alpha) >> 8;
    dg += ((sg - dg) * alpha) >> 8;
    db += ((sb - db) * alpha) >> 8;
    return (uint16_t)((dr << 11) | (dg << 5) | db);
}

static float phi_layer_scale(uint8_t layer)
{
    switch (layer % 3) {
    case 0:
        return 1.0f;
    case 1:
        return EVA_PHI;
    default:
        return EVA_PHI2;
    }
}

static uint16_t phi_count(float base, float mul)
{
    int count = (int)lroundf(base * mul);
    if (count < 0) count = 0;
    if (count > PARTICLE_MAX) count = PARTICLE_MAX;
    return (uint16_t)count;
}

static float density_scale_from_weather(const weather_state_t *st)
{
    if (!st) return 1.0f;
    switch (st->precip_type) {
    case PRECIP_DRIZZLE:     return 0.22f;
    case PRECIP_LIGHT_RAIN:  return 0.32f;
    case PRECIP_RAIN:        return 1.00f;
    case PRECIP_HEAVY_RAIN:  return 1.25f;
    case PRECIP_SNOW:        return 0.80f;
    case PRECIP_SLEET:       return 0.90f;
    case PRECIP_HAIL:        return 0.85f;
    case PRECIP_THUNDER:     return 1.15f;
    case PRECIP_NONE:        break;
    }

    const char *desc = st->desc;
    if (!desc || !desc[0]) return 1.0f;
    if (strstr(desc, "Злива") || strstr(desc, "злива") ||
        strstr(desc, "Силь") || strstr(desc, "силь") ||
        strstr(desc, "Heavy") || strstr(desc, "heavy") ||
        strstr(desc, "Torrential") || strstr(desc, "torrential")) {
        return 1.25f;
    }
    if (strstr(desc, "Помір") || strstr(desc, "помір") ||
        strstr(desc, "Moderate") || strstr(desc, "moderate")) {
        return 0.85f;
    }
    if (strstr(desc, "Легк") || strstr(desc, "легк") ||
        strstr(desc, "Мряк") || strstr(desc, "мряк") ||
        strstr(desc, "Місцями") || strstr(desc, "місцями") ||
        strstr(desc, "Light") || strstr(desc, "light") ||
        strstr(desc, "Drizzle") || strstr(desc, "drizzle") ||
        strstr(desc, "Patchy") || strstr(desc, "patchy")) {
        return 0.32f;
    }
    return 1.0f;
}

static uint8_t particle_layer_for_slot(uint16_t slot, uint16_t total)
{
    if (total <= 1) return 0;
    float p = (float)slot / (float)total;
    float w0 = 1.0f;
    float w1 = EVA_PHI;
    float w2 = EVA_PHI2;
    float sum = w0 + w1 + w2;
    if (p < w0 / sum) return 0;
    if (p < (w0 + w1) / sum) return 1;
    return 2;
}

static void blend_px(int x, int y, uint16_t color, uint8_t alpha)
{
    if ((unsigned)x >= EVA_WEATHER_RENDER_W || (unsigned)y >= EVA_WEATHER_RENDER_H) {
        return;
    }
    uint16_t *p = &s_buf[y * EVA_WEATHER_RENDER_W + x];
    *p = blend565(*p, color, alpha);
}

static uint8_t smooth_u8(float t, uint8_t max_alpha)
{
    if (t <= 0.0f) return 0;
    if (t >= 1.0f) return max_alpha;
    float s = t * t * (3.0f - 2.0f * t);
    return (uint8_t)(s * (float)max_alpha + 0.5f);
}

static uint8_t glyph_alpha_at(const uint8_t *bitmap, lv_font_glyph_format_t format, uint32_t px_idx)
{
    if (!bitmap) return 0;
    switch (format) {
    case LV_FONT_GLYPH_FORMAT_A1: {
        uint8_t bit = (bitmap[px_idx >> 3] >> (7 - (px_idx & 7))) & 0x1;
        return bit ? 255 : 0;
    }
    case LV_FONT_GLYPH_FORMAT_A2: {
        uint8_t b = bitmap[px_idx >> 2];
        uint8_t v = (b >> ((3 - (px_idx & 3)) * 2)) & 0x3;
        return (uint8_t)(v * 85);
    }
    case LV_FONT_GLYPH_FORMAT_A4: {
        uint8_t b = bitmap[px_idx >> 1];
        uint8_t v = (px_idx & 1) ? (b & 0x0f) : (b >> 4);
        return (uint8_t)(v * 17);
    }
    case LV_FONT_GLYPH_FORMAT_A8:
        return bitmap[px_idx];
    default:
        return 0;
    }
}

static uint32_t utf8_next(const char *text, size_t *idx)
{
    if (!text || !idx) return 0;
    const uint8_t b0 = (uint8_t)text[*idx];
    if (b0 == 0) return 0;
    if (b0 < 0x80) {
        (*idx)++;
        return b0;
    }
    if ((b0 & 0xE0) == 0xC0) {
        const uint8_t b1 = (uint8_t)text[*idx + 1];
        if ((b1 & 0xC0) != 0x80) { (*idx)++; return '?'; }
        *idx += 2;
        return ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(b1 & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0) {
        const uint8_t b1 = (uint8_t)text[*idx + 1];
        const uint8_t b2 = (uint8_t)text[*idx + 2];
        if ((b1 & 0xC0) != 0x80 || (b2 & 0xC0) != 0x80) { (*idx)++; return '?'; }
        *idx += 3;
        return ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(b1 & 0x3F) << 6) | (uint32_t)(b2 & 0x3F);
    }
    (*idx)++;
    return '?';
}

static int text_width_utf8(const lv_font_t *font, const char *text)
{
    if (!font || !text) return 0;
    int width = 0;
    size_t i = 0;
    while (text[i]) {
        size_t i_next = i;
        uint32_t letter = utf8_next(text, &i_next);
        size_t j = i_next;
        uint32_t letter_next = utf8_next(text, &j);
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, letter, letter_next)) {
            width += g.adv_w;
            lv_font_glyph_release_draw_data(&g);
        }
        i = i_next;
    }
    return width;
}

/* Scaled variant of draw_text_utf8 using nearest-neighbour pixel upscaling
 * (or downscaling) of the existing A8 glyph bitmaps. The font itself is not
 * touched — each src pixel of the decoded glyph is replicated into a
 * dst-space rectangle whose size is determined by `scale_q8` (8-bit
 * fixed-point, 256 = 1.0×).
 *
 * Used for the clock so it can shrink at night and grow during the day
 * without swapping fonts. Other text (temp, desc) keeps the default path
 * since their fonts already match the canvas resolution.
 *
 * scale_q8 = 256 → identity (same look as draw_text_utf8).
 * scale_q8 = 358 → 1.40× (e.g. day clock).
 * scale_q8 = 154 → 0.60× (e.g. night clock).
 *
 * Width returned by text_width_utf8_scaled() must use the same scale so the
 * caller can centre the resulting block. */
static int text_width_utf8_scaled(const lv_font_t *font, const char *text, uint16_t scale_q8)
{
    int w = text_width_utf8(font, text);
    /* Round half-up to keep tracking close to the visual width. */
    return (int)(((int32_t)w * (int32_t)scale_q8 + 128) >> 8);
}

static int text_height_utf8_scaled(const lv_font_t *font, uint16_t scale_q8)
{
    if (!font) return 0;
    return (int)(((int32_t)font->line_height * (int32_t)scale_q8 + 128) >> 8);
}

static void draw_text_utf8_scaled(const lv_font_t *font, const char *text,
                                  int x, int y, uint16_t color, uint8_t alpha,
                                  uint16_t scale_q8)
{
    if (!font || !text || !text[0] || alpha == 0 || scale_q8 == 0) return;

    /* Scaled metrics in dst space. */
    int line_h_scaled = text_height_utf8_scaled(font, scale_q8);
    int base_line_scaled = (int)(((int32_t)font->base_line * (int32_t)scale_q8 + 128) >> 8);
    int line_top = y + (line_h_scaled - base_line_scaled);

    int pen_x_q8 = x << 8;   /* fractional pen position to avoid drift on small scales */
    size_t i = 0;

    while (text[i]) {
        size_t i_next = i;
        uint32_t letter = utf8_next(text, &i_next);
        size_t j = i_next;
        uint32_t letter_next = utf8_next(text, &j);

        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, letter, letter_next)) {
            i = i_next;
            continue;
        }
        if (g.box_w > 0 && g.box_h > 0) {
            if (g.box_w > GLYPH_BUF_W || g.box_h > GLYPH_BUF_H) {
                pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
                lv_font_glyph_release_draw_data(&g);
                i = i_next;
                continue;
            }
            const void *bm_ret = lv_font_get_glyph_bitmap(&g, &s_glyph_draw_buf);
            const uint8_t *bitmap = bm_ret ? s_glyph_raw : NULL;
            if (!bitmap) {
                pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
                lv_font_glyph_release_draw_data(&g);
                i = i_next;
                continue;
            }

            /* Scaled glyph footprint in dst space. */
            int dst_box_w = (int)(((int32_t)g.box_w * (int32_t)scale_q8 + 128) >> 8);
            int dst_box_h = (int)(((int32_t)g.box_h * (int32_t)scale_q8 + 128) >> 8);
            int dst_ofs_x = (int)(((int32_t)g.ofs_x * (int32_t)scale_q8 + 128) >> 8);
            int dst_ofs_y = (int)(((int32_t)g.ofs_y * (int32_t)scale_q8 + 128) >> 8);

            int pen_x = (pen_x_q8 + 128) >> 8;
            int gx0 = pen_x + dst_ofs_x;
            int gy0 = line_top - dst_box_h - dst_ofs_y;

            /* Walk dst pixels; sample src via inverse scale. inv = 256/scale_q8
             * pre-computed in q16 so the inner loop only needs a multiply+shift. */
            uint32_t inv_q16 = ((uint32_t)1 << 24) / (uint32_t)scale_q8;   /* (256<<16)/scale_q8 */

            for (int dy = 0; dy < dst_box_h; ++dy) {
                uint32_t sy = ((uint32_t)dy * inv_q16) >> 16;
                if (sy >= g.box_h) continue;
                const uint8_t *src_row = &bitmap[sy * g.box_w];
                int py = gy0 + dy;
                for (int dx = 0; dx < dst_box_w; ++dx) {
                    uint32_t sx = ((uint32_t)dx * inv_q16) >> 16;
                    if (sx >= g.box_w) continue;
                    uint8_t glyph_a = src_row[sx];
                    if (glyph_a == 0) continue;
                    uint8_t a = (uint8_t)(((uint16_t)glyph_a * alpha) / 255U);
                    blend_px(gx0 + dx, py, color, a);
                }
            }
        }
        pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
        lv_font_glyph_release_draw_data(&g);
        i = i_next;
    }
}

static void draw_text_utf8(const lv_font_t *font, const char *text,
                           int x, int y, uint16_t color, uint8_t alpha)
{
    if (!font || !text || !text[0] || alpha == 0) return;
    int pen_x = x;
    int line_top = y + (font->line_height - font->base_line);
    size_t i = 0;

    while (text[i]) {
        size_t i_next = i;
        uint32_t letter = utf8_next(text, &i_next);
        size_t j = i_next;
        uint32_t letter_next = utf8_next(text, &j);

        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, letter, letter_next)) {
            i = i_next;
            continue;
        }
        if (g.box_w > 0 && g.box_h > 0) {
            /* Skip glyphs that don't fit in our static draw_buf. */
            if (g.box_w > GLYPH_BUF_W || g.box_h > GLYPH_BUF_H) {
                pen_x += g.adv_w;
                lv_font_glyph_release_draw_data(&g);
                i = i_next;
                continue;
            }
            /* Pass the static draw_buf; lv_font_get_bitmap_fmt_txt requires
             * a valid (non-NULL) draw_buf and writes the decoded A8 pixels
             * into draw_buf->data, then returns draw_buf itself. We read
             * pixels from s_glyph_raw. */
            const void *bm_ret = lv_font_get_glyph_bitmap(&g, &s_glyph_draw_buf);
            const uint8_t *bitmap = bm_ret ? s_glyph_raw : NULL;
            int gx0 = pen_x + g.ofs_x;
            int gy0 = line_top - g.box_h - g.ofs_y;
            /* lv_font_get_bitmap_fmt_txt always decodes to A8 (1 byte/px)
             * regardless of the source font's bpp, with stride = box_w. */
            for (uint32_t gy = 0; gy < g.box_h; ++gy) {
                for (uint32_t gx = 0; gx < g.box_w; ++gx) {
                    uint32_t px_idx = gy * g.box_w + gx;
                    uint8_t glyph_a = glyph_alpha_at(bitmap, LV_FONT_GLYPH_FORMAT_A8, px_idx);
                    if (glyph_a == 0) continue;
                    uint8_t a = (uint8_t)(((uint16_t)glyph_a * alpha) / 255U);
                    blend_px(gx0 + (int)gx, gy0 + (int)gy, color, a);
                }
            }
        }
        pen_x += g.adv_w;
        lv_font_glyph_release_draw_data(&g);
        i = i_next;
    }
}

/* Bake `text` at `scale_q8` into slot->a8 as a tight A8 mask. Writes a
 * mask_w × mask_h footprint starting at the top-left of slot->a8. Mask
 * pixels accumulate alpha; an existing mask is overwritten (memset 0 first).
 * No-op if (text,scale_q8) already matches slot's cached key. */
static void bake_text_slot(text_slot_t *slot, const lv_font_t *font,
                           const char *text, uint16_t scale_q8)
{
    if (!slot || !slot->a8 || !font || !text) return;

    if (slot->valid && slot->key_scale_q8 == scale_q8 &&
        strncmp(slot->key_text, text, sizeof(slot->key_text)) == 0) {
        return;  /* cache hit */
    }

    int total_w = text_width_utf8_scaled(font, text, scale_q8);
    int total_h = text_height_utf8_scaled(font, scale_q8);
    if (total_w <= 0 || total_h <= 0) {
        slot->mask_w = 0;
        slot->mask_h = 0;
        slot->valid = true;
        strlcpy(slot->key_text, text, sizeof(slot->key_text));
        slot->key_scale_q8 = scale_q8;
        return;
    }
    if (total_w > TEXT_SLOT_BUF_W) total_w = TEXT_SLOT_BUF_W;
    if (total_h > TEXT_SLOT_BUF_H) total_h = TEXT_SLOT_BUF_H;
    memset(slot->a8, 0, (size_t)total_w * (size_t)total_h);

    int line_h_scaled = text_height_utf8_scaled(font, scale_q8);
    int base_line_scaled = (int)(((int32_t)font->base_line * (int32_t)scale_q8 + 128) >> 8);
    int line_top = (line_h_scaled - base_line_scaled);

    int pen_x_q8 = 0;
    size_t i = 0;
    while (text[i]) {
        size_t i_next = i;
        uint32_t letter = utf8_next(text, &i_next);
        size_t j = i_next;
        uint32_t letter_next = utf8_next(text, &j);

        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, letter, letter_next)) {
            i = i_next;
            continue;
        }
        if (g.box_w > 0 && g.box_h > 0) {
            if (g.box_w > GLYPH_BUF_W || g.box_h > GLYPH_BUF_H) {
                pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
                lv_font_glyph_release_draw_data(&g);
                i = i_next;
                continue;
            }
            const void *bm_ret = lv_font_get_glyph_bitmap(&g, &s_glyph_draw_buf);
            const uint8_t *bitmap = bm_ret ? s_glyph_raw : NULL;
            if (!bitmap) {
                pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
                lv_font_glyph_release_draw_data(&g);
                i = i_next;
                continue;
            }

            int dst_box_w = (int)(((int32_t)g.box_w * (int32_t)scale_q8 + 128) >> 8);
            int dst_box_h = (int)(((int32_t)g.box_h * (int32_t)scale_q8 + 128) >> 8);
            int dst_ofs_x = (int)(((int32_t)g.ofs_x * (int32_t)scale_q8 + 128) >> 8);
            int dst_ofs_y = (int)(((int32_t)g.ofs_y * (int32_t)scale_q8 + 128) >> 8);

            int pen_x = (pen_x_q8 + 128) >> 8;
            int gx0 = pen_x + dst_ofs_x;
            int gy0 = line_top - dst_box_h - dst_ofs_y;

            uint32_t inv_q16 = ((uint32_t)1 << 24) / (uint32_t)scale_q8;

            for (int dy = 0; dy < dst_box_h; ++dy) {
                int py = gy0 + dy;
                if ((unsigned)py >= (unsigned)total_h) continue;
                uint32_t sy = ((uint32_t)dy * inv_q16) >> 16;
                if (sy >= g.box_h) continue;
                const uint8_t *src_row = &bitmap[sy * g.box_w];
                uint8_t *dst_row = &slot->a8[py * total_w];
                for (int dx = 0; dx < dst_box_w; ++dx) {
                    int px = gx0 + dx;
                    if ((unsigned)px >= (unsigned)total_w) continue;
                    uint32_t sx = ((uint32_t)dx * inv_q16) >> 16;
                    if (sx >= g.box_w) continue;
                    uint8_t a = src_row[sx];
                    if (a == 0) continue;
                    /* Last-writer-wins for overlapping glyphs (kerning rare
                     * for our texts; keeps the bake branch-light). */
                    if (a > dst_row[px]) dst_row[px] = a;
                }
            }
        }
        pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
        lv_font_glyph_release_draw_data(&g);
        i = i_next;
    }

    slot->mask_w = (uint16_t)total_w;
    slot->mask_h = (uint16_t)total_h;
    slot->key_scale_q8 = scale_q8;
    strlcpy(slot->key_text, text, sizeof(slot->key_text));
    slot->valid = true;
}

/* Blit the slot's cached A8 mask onto s_buf at (dst_x, dst_y) using the
 * given colour and base alpha. Hot path: one row of mask = one row of
 * RGB565 writes, only touching pixels with non-zero mask alpha. */
static void blit_text_slot(const text_slot_t *slot, int dst_x, int dst_y,
                           uint16_t color, uint8_t base_alpha)
{
    if (!slot || !slot->valid || !slot->a8 || base_alpha == 0) return;
    if (slot->mask_w == 0 || slot->mask_h == 0) return;

    int w = slot->mask_w;
    int h = slot->mask_h;

    int x0 = dst_x, y0 = dst_y;
    int x1 = dst_x + w, y1 = dst_y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EVA_WEATHER_RENDER_W) x1 = EVA_WEATHER_RENDER_W;
    if (y1 > EVA_WEATHER_RENDER_H) y1 = EVA_WEATHER_RENDER_H;
    if (x0 >= x1 || y0 >= y1) return;

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

static void draw_scene_text_overlays(void)
{
    char clock_txt[sizeof(s_clock_text)];
    char temp_txt[sizeof(s_temp_text)];
    char desc_txt[sizeof(s_desc_text)];
    portENTER_CRITICAL(&s_text_mux);
    memcpy(clock_txt, s_clock_text, sizeof(clock_txt));
    memcpy(temp_txt, s_temp_text, sizeof(temp_txt));
    memcpy(desc_txt, s_desc_text, sizeof(desc_txt));
    portEXIT_CRITICAL(&s_text_mux);
    clock_txt[sizeof(clock_txt) - 1] = '\0';
    temp_txt[sizeof(temp_txt) - 1] = '\0';
    desc_txt[sizeof(desc_txt) - 1] = '\0';

    /* Clock: native size (1.0×) by day, 0.6× downscale at night. Both bake
     * paths go through the slot cache, so per-frame cost is just the A8 blit. */
    const lv_font_t *clock_font = &eva_font_clock_144_extralight;
    uint16_t clock_scale_q8 = is_night_kind(s_kind) ? 154 : 256;
    bake_text_slot(&s_clock_slot, clock_font, clock_txt, clock_scale_q8);
    int clock_x = (EVA_WEATHER_RENDER_W - s_clock_slot.mask_w) / 2;
    int clock_y = (EVA_WEATHER_RENDER_H - s_clock_slot.mask_h) / 2;
    blit_text_slot(&s_clock_slot, clock_x, clock_y, rgb565(255, 255, 255), 240);

    const lv_font_t *temp_font = &lv_font_montserrat_48;
    bake_text_slot(&s_temp_slot, temp_font, temp_txt, 256);
    int temp_x = (EVA_WEATHER_RENDER_W - s_temp_slot.mask_w) / 2;
    int temp_y = 56;
    blit_text_slot(&s_temp_slot, temp_x, temp_y, rgb565(255, 255, 255), 255);

    const lv_font_t *desc_font = &eva_font_uk_22;
    bake_text_slot(&s_desc_slot, desc_font, desc_txt, 256);
    int desc_x = (EVA_WEATHER_RENDER_W - s_desc_slot.mask_w) / 2;
    int desc_y = 116;
    blit_text_slot(&s_desc_slot, desc_x, desc_y, rgb565(240, 243, 248), 240);
}

static void fill_gradient(rgb_t top, rgb_t bottom)
{
    for (int y = 0; y < EVA_WEATHER_RENDER_H; ++y) {
        int t = (y * 255) / (EVA_WEATHER_RENDER_H - 1);
        rgb_t c = {
            .r = (uint8_t)(top.r + (((int)bottom.r - top.r) * t) / 255),
            .g = (uint8_t)(top.g + (((int)bottom.g - top.g) * t) / 255),
            .b = (uint8_t)(top.b + (((int)bottom.b - top.b) * t) / 255),
        };
        uint16_t px = rgb565_from(c);
        uint16_t *row = &s_buf[y * EVA_WEATHER_RENDER_W];
        for (int x = 0; x < EVA_WEATHER_RENDER_W; ++x) {
            row[x] = px;
        }
    }
}

static int minutes_now(void)
{
    time_t now = time(NULL);
    struct tm tm_now = {0};
    int offset_hours = s_time_offset_hours;
    if (now > 1700000000) {
        if (offset_hours != 0) {
            now += (time_t)offset_hours * 3600;
        }
        localtime_r(&now, &tm_now);
    } else {
        int total_s = (int)((esp_timer_get_time() / 1000000) % (24 * 3600));
        total_s += offset_hours * 3600;
        total_s %= (24 * 3600);
        if (total_s < 0) {
            total_s += 24 * 3600;
        }
        tm_now.tm_hour = total_s / 3600;
        tm_now.tm_min = (total_s / 60) % 60;
    }
    return tm_now.tm_hour * 60 + tm_now.tm_min;
}

/* Effective sunrise/sunset for the current scene. Returns hardcoded 6:00/18:00
 * if clearoutside astronomy hasn't been fetched yet. */
static void sun_events(int *out_sunrise, int *out_sunset)
{
    *out_sunrise = (s_sunrise_min >= 0) ? s_sunrise_min : 360;   /* 06:00 */
    *out_sunset  = (s_sunset_min  >= 0) ? s_sunset_min  : 1080;  /* 18:00 */
}

static sky_t sky_for_kind(weather_kind_t kind)
{
    int m = minutes_now();
    int sr, ss;
    sun_events(&sr, &ss);
    bool sunrise = (m >= sr - SUN_WINDOW_MIN && m < sr + SUN_WINDOW_MIN);
    bool sunset  = (m >= ss - SUN_WINDOW_MIN && m < ss + SUN_WINDOW_MIN);
    bool day     = (!sunrise && !sunset && m >= sr - SUN_WINDOW_MIN && m < ss + SUN_WINDOW_MIN);
    bool night = !sunrise && !day && !sunset;

    /* Explicit kind tags override the time-of-day fallback. Previously these
     * only set the new state to true without clearing the others, so a
     * CLEAR_NIGHT kind at e.g. sunset wall-clock time would still drop into
     * the sunset branch below and render a pink-rose sky. Force the other
     * states off so the right branch wins. */
    if (kind == WEATHER_CLEAR_NIGHT || kind == WEATHER_PARTLY_CLOUDY_NIGHT) {
        night = true;
        day = false;
        sunrise = false;
        sunset = false;
    }
    if (kind == WEATHER_CLEAR_DAY || kind == WEATHER_PARTLY_CLOUDY_DAY) {
        day = true;
        night = false;
    }

    if (kind == WEATHER_RAIN || kind == WEATHER_SLEET) {
        return (sky_t){ "rain", {34, 44, 60}, {82, 92, 104} };
    }
    if (kind == WEATHER_HEAVY_RAIN) {
        return (sky_t){ "heavy-rain", {18, 26, 40}, {52, 60, 74} };
    }
    if (kind == WEATHER_THUNDERSTORM) {
        return (sky_t){ "thunderstorm", {12, 18, 30}, {46, 48, 58} };
    }
    if (kind == WEATHER_SNOW || kind == WEATHER_HAIL) {
        return (sky_t){ "snow", {116, 132, 150}, {205, 214, 220} };
    }
    if (kind == WEATHER_FOG) {
        return (sky_t){ "fog", {130, 138, 145}, {215, 216, 210} };
    }
    if (kind == WEATHER_CLOUDY) {
        return (sky_t){ "cloudy", {78, 92, 108}, {150, 160, 170} };
    }
    if (sunrise) {
        /* p in [0..1] across the 2-hour window centred on actual sunrise. */
        float p = (float)(m - (sr - SUN_WINDOW_MIN)) / (float)(2 * SUN_WINDOW_MIN);
        if (p < 0.0f) p = 0.0f; else if (p > 1.0f) p = 1.0f;
        rgb_t a = {26, 26, 46};
        rgb_t b = {255, 160, 122};
        rgb_t c = {255, 215, 0};
        rgb_t top = {
            .r = (uint8_t)(a.r + (b.r - a.r) * p),
            .g = (uint8_t)(a.g + (b.g - a.g) * p),
            .b = (uint8_t)(a.b + (b.b - a.b) * p),
        };
        rgb_t bot = {
            .r = (uint8_t)(a.r + (c.r - a.r) * p),
            .g = (uint8_t)(a.g + (c.g - a.g) * p),
            .b = (uint8_t)(a.b + (c.b - a.b) * p),
        };
        return (sky_t){ "sunrise", top, bot };
    }
    if (sunset) {
        float p = (float)(m - (ss - SUN_WINDOW_MIN)) / (float)(2 * SUN_WINDOW_MIN);
        if (p < 0.0f) p = 0.0f; else if (p > 1.0f) p = 1.0f;
        rgb_t a = {255, 107, 107};
        rgb_t b = {255, 160, 122};
        rgb_t n = {26, 26, 46};
        rgb_t top = {
            .r = (uint8_t)(a.r + (n.r - a.r) * p),
            .g = (uint8_t)(a.g + (n.g - a.g) * p),
            .b = (uint8_t)(a.b + (n.b - a.b) * p),
        };
        rgb_t bot = {
            .r = (uint8_t)(b.r + (n.r - b.r) * p),
            .g = (uint8_t)(b.g + (n.g - b.g) * p),
            .b = (uint8_t)(b.b + (n.b - b.b) * p),
        };
        return (sky_t){ "sunset", top, bot };
    }
    if (night) {
        return (sky_t){ "night", {8, 12, 28}, {24, 22, 46} };
    }
    return (sky_t){ "day", {24, 78, 142}, {74, 152, 214} };
}

static bool sky_kind_is_clearish(weather_kind_t kind)
{
    return kind == WEATHER_CLEAR_DAY || kind == WEATHER_PARTLY_CLOUDY_DAY ||
           kind == WEATHER_CLEAR_NIGHT || kind == WEATHER_PARTLY_CLOUDY_NIGHT;
}

static void draw_day_sky_depth(void)
{
    if (is_night_kind(s_kind) || !sky_kind_is_clearish(s_kind)) return;

    const int cx = EVA_WEATHER_RENDER_W / 2;
    const int cy = EVA_WEATHER_RENDER_H / 5;
    const int max_d2 = cx * cx + EVA_WEATHER_RENDER_H * EVA_WEATHER_RENDER_H;
    const uint16_t zenith = rgb565(7, 37, 92);
    for (int y = 0; y < EVA_WEATHER_RENDER_H; y += 2) {
        for (int x = 0; x < EVA_WEATHER_RENDER_W; x += 2) {
            int dx = x - cx;
            int dy = y - cy;
            float d = (float)(dx * dx + dy * dy) / (float)max_d2;
            if (d < 0.0f) d = 0.0f;
            if (d > 1.0f) d = 1.0f;
            uint8_t a = smooth_u8(1.0f - d, 42);
            if (!a) continue;
            blend_px(x, y, zenith, a);
            blend_px(x + 1, y, zenith, a);
            blend_px(x, y + 1, zenith, a);
            blend_px(x + 1, y + 1, zenith, a);
        }
    }
}

static void draw_sun_sky_glare(void)
{
    if (!s_sun_visible || s_sun_strength <= 0.0f || is_night_kind(s_kind)) return;

    int r = (int)(260.0f + 90.0f * s_sun_strength);
    int r2 = r * r;
    uint16_t cool_glare = rgb565(210, 232, 255);
    uint16_t warm_core = rgb565(255, 250, 218);
    for (int y = s_sun_y - r; y <= s_sun_y + r; y += 2) {
        if ((unsigned)y >= EVA_WEATHER_RENDER_H) continue;
        for (int x = s_sun_x - r; x <= s_sun_x + r; x += 2) {
            if ((unsigned)x >= EVA_WEATHER_RENDER_W) continue;
            int dx = x - s_sun_x;
            int dy = y - s_sun_y;
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            float t = 1.0f - (float)d2 / (float)r2;
            uint8_t a = smooth_u8(t, (uint8_t)(34.0f * s_sun_strength));
            if (!a) continue;
            blend_px(x, y, cool_glare, a);
            blend_px(x + 1, y, cool_glare, a);
            blend_px(x, y + 1, cool_glare, a);
            blend_px(x + 1, y + 1, cool_glare, a);
            if (t > 0.42f) {
                uint8_t wa = smooth_u8((t - 0.42f) / 0.58f, (uint8_t)(48.0f * s_sun_strength));
                blend_px(x, y, warm_core, wa);
                blend_px(x + 1, y, warm_core, wa);
                blend_px(x, y + 1, warm_core, wa);
                blend_px(x + 1, y + 1, warm_core, wa);
            }
        }
    }
}

static void draw_filled_circle(int cx, int cy, int r, uint16_t color, uint8_t alpha)
{
    if (r <= 0) return;
    int r2 = r * r;
    int x0 = cx - r;
    int x1 = cx + r;
    int y0 = cy - r;
    int y1 = cy + r;
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= r2) {
                uint8_t a = alpha;
                if (d2 > (r2 * 3) / 4) {
                    a = (uint8_t)((alpha * (r2 - d2)) / (r2 / 4 + 1));
                }
                blend_px(x, y, color, a);
            }
        }
    }
}

/* Paint a Gaussian blob split into two alpha masks based on vertical position
 * within the blob. Pixels above cy get accumulated into `a8_light`, pixels
 * below into `a8_shadow`. The split is smoothed by a ~ry-wide transition band
 * so there's no hard horizontal line. `light_bias` shifts the split centre
 * up or down (positive = more shadow, less light, used for storm bases). */
/* Paint a Gaussian blob into the light/shadow A8 strips. The strip wraps
 * horizontally — a blob near x=0 or x=w-1 spills onto the opposite edge so
 * it stays whole during the runtime scroll. Vertical clamping stays because
 * each layer has a fixed strip_h (top/bottom feathered by feather_strip_edges
 * afterwards). */
static void blob_gaussian_triple(uint8_t *a8_light, uint8_t *a8_shadow,
                                 uint8_t *a8_core,
                                 int w, int h,
                                 float cx, float cy, float rx, float ry,
                                 uint8_t peak_alpha,
                                 float light_bias,
                                 float core_gain)
{
    if (!a8_light || !a8_shadow || !a8_core || rx <= 0.0f || ry <= 0.0f) return;

    int x0 = (int)floorf(cx - rx * 2.5f);
    int x1 = (int)ceilf(cx + rx * 2.5f);
    int y0 = (int)floorf(cy - ry * 2.5f);
    int y1 = (int)ceilf(cy + ry * 2.5f);
    /* X coords are wrapped, not clamped — keeps blobs that straddle the
     * strip seam (x ≈ 0 or x ≈ w-1) fully painted. Y stays clamped because
     * a layer has fixed vertical extent. */
    if (y0 < 0) y0 = 0;
    if (y1 >= h) y1 = h - 1;

    float kx = 0.7f / (rx * rx);
    float ky = 0.7f / (ry * ry);
    float split_cy = cy + light_bias * ry;
    float band = ry * 0.5f;
    if (band < 1.0f) band = 1.0f;

    for (int y = y0; y <= y1; ++y) {
        float dy = (float)y - cy;
        float ey = dy * dy * ky;
        float t = ((float)y - split_cy) / band + 0.5f;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        float light_frac = 1.0f - (t * t * (3.0f - 2.0f * t));

        uint8_t *row_light  = &a8_light[y * w];
        uint8_t *row_shadow = &a8_shadow[y * w];
        uint8_t *row_core   = &a8_core[y * w];
        for (int x = x0; x <= x1; ++x) {
            /* Wrap x onto [0, w) so blobs that overhang either edge of the
             * strip continue onto the opposite side. (w is a power of two
             * for the runtime scroll — but we don't assume that here so the
             * code works if CLOUD_STRIP_W ever changes.) */
            int xi = x % w;
            if (xi < 0) xi += w;

            float dx = (float)x - cx;
            float v = expf(-(dx * dx * kx + ey));
            int total = (int)(v * (float)peak_alpha + 0.5f);
            if (total <= 0) continue;
            int add_l = (int)((float)total * light_frac + 0.5f);
            int add_s = total - add_l;
            float core_t = (v - 0.35f) / 0.50f;
            if (core_t < 0.0f) core_t = 0.0f;
            else if (core_t > 1.0f) core_t = 1.0f;
            core_t = core_t * core_t * (3.0f - 2.0f * core_t);
            float lower = ((float)y - split_cy) / (ry * 1.2f) + 0.5f;
            if (lower < 0.0f) lower = 0.0f;
            else if (lower > 1.0f) lower = 1.0f;
            int add_c = (int)((float)total * core_t *
                              (0.35f + 0.45f * lower) * core_gain + 0.5f);
            int out_l = (int)row_light[xi] + add_l;
            int out_s = (int)row_shadow[xi] + add_s;
            int out_c = (int)row_core[xi] + add_c;
            row_light[xi]  = (uint8_t)(out_l > 255 ? 255 : out_l);
            row_shadow[xi] = (uint8_t)(out_s > 255 ? 255 : out_s);
            row_core[xi]   = (uint8_t)(out_c > 255 ? 255 : out_c);
        }
    }
}

static uint8_t scaled_peak(int base, float lo, float hi)
{
    int v = (int)((float)base * rndf(lo, hi) + 0.5f);
    return clamp_u8(v);
}

static void bake_strip_high(uint8_t *a8_light, uint8_t *a8_shadow,
                            uint8_t *a8_core, int w, int h)
{
    memset(a8_light, 0, w * h);
    memset(a8_shadow, 0, w * h);
    memset(a8_core, 0, w * h);
    /* Cirrus: thin streaks, the blob primitive wraps X so blobs near the
     * seam stay whole. We place anchors across the FULL strip width — any
     * blob whose centre+rx lands past the right edge spills onto the left. */
    /* Cirrus peak alpha = FIB_144 (translucent veils). Light bias -0.4 keeps
     * the streaks mostly in the light mask since they're thin enough that
     * there's no real underside shadow in reality. */
    for (int i = 0; i < FIB_5; ++i) {                /* 5 thin veils */
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             rndf(0.0f, (float)w),
                             rndf((float)h * 0.26f, (float)h * 0.62f),
                             rndf((float)FIB_89, (float)FIB_144),
                             rndf((float)FIB_2, (float)FIB_5),
                             scaled_peak(FIB_89, 0.45f, 0.80f),
                             -0.55f, 0.10f);
    }
}

static void bake_strip_mid(uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core, int w, int h)
{
    memset(a8_light, 0, w * h);
    memset(a8_shadow, 0, w * h);
    memset(a8_core, 0, w * h);
    /* Mid layer: small broken fair-weather cumulus, not a continuous band. */
    for (int i = 0; i < FIB_8; ++i) {
        float cx = rndf(0.0f, (float)w);
        float cy = rndf((float)h * 0.34f, (float)h * 0.72f);
        float base_rx = rndf((float)FIB_34, (float)FIB_55 + 12.0f);
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             cx, cy + rndf(5.0f, 14.0f),
                             base_rx, rndf((float)FIB_8 + 5.0f, (float)FIB_13 + 7.0f),
                             scaled_peak(FIB_144 + FIB_34, 0.66f, 1.02f),
                             0.65f, 0.85f);
        for (int j = 0; j < FIB_3 + FIB_1; ++j) {
            blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                                 cx + rndf(-base_rx * 0.65f, base_rx * 0.65f),
                                 cy + rndf(-(float)FIB_21, (float)FIB_8),
                                 rndf((float)FIB_13 + 7.0f, (float)FIB_34),
                                 rndf((float)FIB_8 + 4.0f, (float)FIB_21),
                                 scaled_peak(FIB_233 - FIB_21, 0.72f, 1.08f),
                                 -0.45f, 0.38f);
        }
        for (int j = 0; j < FIB_3; ++j) {
            blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                                 cx + rndf(-base_rx * 0.75f, base_rx * 0.75f),
                                 cy + rndf(-(float)FIB_34, -(float)FIB_8),
                                 rndf((float)FIB_5, (float)FIB_13 + 2.0f),
                                 rndf((float)FIB_5, (float)FIB_13),
                                 scaled_peak(FIB_89, 0.35f, 0.70f),
                                 -0.7f, 0.10f);
        }
    }
}

static void bake_strip_low(uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core, int w, int h)
{
    memset(a8_light, 0, w * h);
    memset(a8_shadow, 0, w * h);
    memset(a8_core, 0, w * h);
    /* Low layer: larger cumulus masses with dark bellies and torn bright
     * tops. These are the shapes that should echo the user's real reference
     * photo: strong volume, broken edge, many different scales. */
    for (int i = 0; i < FIB_5; ++i) {
        float cx = rndf(0.0f, (float)w);
        float cy = rndf((float)h * 0.42f, (float)h * 0.82f);
        float mass = rndf(0.72f, 1.18f);
        float base_rx = rndf((float)FIB_34 + 8.0f, (float)FIB_89) * mass;
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h, cx, cy,
                             base_rx,
                             rndf((float)FIB_13 + 6.0f, (float)FIB_21 + 10.0f) * mass,
                             scaled_peak(FIB_233, 0.68f, 0.96f),
                             0.62f, 1.00f);
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             cx + rndf(-28.0f, 28.0f), cy + rndf(14.0f, 30.0f),
                             base_rx * rndf(0.82f, 1.28f),
                             rndf((float)FIB_8 + 2.0f, (float)FIB_13 + 5.0f) * mass,
                             scaled_peak(FIB_144 + FIB_21, 0.62f, 0.94f),
                             1.05f, 1.25f);
        for (int j = 0; j < FIB_5 + FIB_1; ++j) {
            float jx = rndf(-base_rx * 0.62f, base_rx * 0.62f);
            float jy = rndf(-(float)FIB_55, -(float)FIB_8);
            blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                                 cx + jx, cy + jy,
                                 rndf((float)FIB_13 + 7.0f, (float)FIB_34 + 12.0f) * mass,
                                 rndf((float)FIB_13, (float)FIB_21 + 5.0f) * mass,
                                 scaled_peak(FIB_233 - FIB_13, 0.70f, 1.08f),
                                 -0.55f, 0.38f);
        }
        for (int j = 0; j < FIB_8; ++j) {
            float jx = rndf(-base_rx * 0.78f, base_rx * 0.78f);
            float jy = rndf(-(float)FIB_55 - 12.0f, (float)FIB_13);
            blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                                 cx + jx, cy + jy,
                                 rndf((float)FIB_5 + 2.0f, (float)FIB_13 + 5.0f),
                                 rndf((float)FIB_5 + 1.0f, (float)FIB_13 + 3.0f),
                                 scaled_peak(FIB_144, 0.22f, 0.60f),
                                 -0.85f, 0.06f);
        }
    }
}

/* Multiply the top and bottom edges of an A8 strip by a smoothstep ramp so
 * the layer's transition into the sky gradient looks continuous instead of
 * a hard horizontal line. Run after the blob bake. */
static void feather_strip_edges(uint8_t *a8, int w, int h, int fade_px)
{
    if (fade_px <= 0 || h < 2 * fade_px) return;
    for (int y = 0; y < fade_px; ++y) {
        float t = (float)y / (float)fade_px;
        /* smoothstep(0, 1, t) = t*t*(3 - 2*t) */
        float k = t * t * (3.0f - 2.0f * t);
        int k_q = (int)(k * 256.0f);
        uint8_t *row_top = &a8[y * w];
        uint8_t *row_bot = &a8[(h - 1 - y) * w];
        for (int x = 0; x < w; ++x) {
            row_top[x] = (uint8_t)((row_top[x] * k_q) >> 8);
            row_bot[x] = (uint8_t)((row_bot[x] * k_q) >> 8);
        }
    }
}

static void bake_strip_for_layer(int layer, cloud_variant_t *v, int h)
{
    if (!v || !v->a8_light || !v->a8_shadow || !v->a8_core) return;
    if (layer == CLOUD_LAYER_HIGH) {
        bake_strip_high(v->a8_light, v->a8_shadow, v->a8_core,
                        CLOUD_STRIP_W, h);
    } else if (layer == CLOUD_LAYER_MID) {
        bake_strip_mid(v->a8_light, v->a8_shadow, v->a8_core,
                       CLOUD_STRIP_W, h);
    } else {
        bake_strip_low(v->a8_light, v->a8_shadow, v->a8_core,
                       CLOUD_STRIP_W, h);
    }
    feather_strip_edges(v->a8_light,  CLOUD_STRIP_W, h, FIB_13);
    feather_strip_edges(v->a8_shadow, CLOUD_STRIP_W, h, FIB_13);
    feather_strip_edges(v->a8_core,   CLOUD_STRIP_W, h, FIB_13);
}

static void init_cloud_strips(void)
{
    size_t total = 0;
    /* Re-seed the canvas RNG once so the stable boot-seed doesn't lead to
     * mirror-symmetric cumulus placement (visible bug on first screenshots).
     * Using esp_timer_get_time() gives each boot a fresh distribution. */
    s_rng ^= (uint32_t)esp_timer_get_time();
    for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
        cloud_strip_t *strip = &s_strip[i];
        size_t bytes = (size_t)CLOUD_STRIP_W * strip->strip_h;
        for (int j = 0; j < CLOUD_VARIANT_COUNT; ++j) {
            cloud_variant_t *v = &strip->variant[j];
            if (!v->a8_light) {
                v->a8_light = heap_caps_aligned_alloc(64, bytes,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (!v->a8_shadow) {
                v->a8_shadow = heap_caps_aligned_alloc(64, bytes,
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (!v->a8_core) {
                v->a8_core = heap_caps_aligned_alloc(64, bytes,
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            }
            if (!v->a8_light || !v->a8_shadow || !v->a8_core) {
                ESP_LOGE(TAG, "cloud strip %d variant %d alloc failed", i, j);
                abort();
            }
            bake_strip_for_layer(i, v, strip->strip_h);
            total += 3 * bytes;
        }
        strip->active_variant = 0;
        strip->morphing = false;
        strip->morph_t = 0.0f;
        strip->morph_clock = -rndf(0.0f, strip->morph_hold_s * 0.65f);
    }
    ESP_LOGI(TAG, "baked HIGH/MID/LOW morph cloud strips (2 variants, light+shadow+core), total %u KB PSRAM",
             (unsigned)(total / 1024));
}

static void draw_line(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha, int thickness)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        for (int oy = -thickness; oy <= thickness; ++oy) {
            for (int ox = -thickness; ox <= thickness; ++ox) {
                blend_px(x0 + ox, y0 + oy, color, alpha);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

/* No reset_clouds() — A8 cloud strips scroll continuously across weather
 * changes. Their geometry evolves through slow variant crossfades and
 * off-screen rebakes, while per-kind changes only affect tint/alpha. */

static void spawn_particle(particle_t *p, particle_kind_t kind, bool from_top, uint16_t slot)
{
    memset(p, 0, sizeof(*p));
    p->kind = kind;
    p->layer = particle_layer_for_slot(slot, s_target);
    float z = phi_layer_scale(p->layer);
    float speed_z = 0.62f * z;
    float size_z = 0.82f + 0.11f * z;
    float alpha_z = 0.80f + 0.08f * z;
    p->x = rndf(0.0f, EVA_WEATHER_RENDER_W - 1.0f);
    p->y = from_top ? rndf(-120.0f, -8.0f) : rndf(0.0f, EVA_WEATHER_RENDER_H - 1.0f);
    p->phase = rndf(0.0f, 6.28f);
    p->spin = rndf(-1.0f, 1.0f);
    switch (kind) {
    case P_RAIN:
        /* Base vy 330..540 px/s. Horizontal velocity = wind bias + small
         * natural turbulence jitter. When wind is unknown (-1 from state),
         * s_wind_vx_bias is 0 and rain falls effectively straight. */
    {
        float light = (s_density_scale < 0.70f) ? 0.68f : 1.0f;
        p->vx = (s_wind_vx_bias + rndf(-20.0f, 20.0f)) * speed_z;
        p->vy = rndf(330.0f, 540.0f) * speed_z;
        p->size = rndf(7.0f, 15.0f) * size_z * light;
        p->alpha = rndf(0.38f, 0.72f) * alpha_z * light;
        break;
    }
    case P_SNOW:
        /* Snow is much lighter — wind effect is ~30 % of rain's, plus a
         * symmetric jitter for natural wobble. */
        p->vx = (s_wind_vx_bias * 0.30f + rndf(-14.0f, 14.0f)) * speed_z;
        p->vy = rndf(28.0f, 86.0f) * speed_z;
        p->size = rndf(1.4f, 3.8f) * size_z;
        p->alpha = rndf(0.45f, 0.90f) * alpha_z;
        break;
    case P_HAIL:
        /* Hail is heavy — wind effect is only ~15 % of rain's. */
        p->vx = (s_wind_vx_bias * 0.15f + rndf(-25.0f, 25.0f)) * speed_z;
        p->vy = rndf(360.0f, 620.0f) * speed_z;
        p->size = rndf(2.0f, 5.0f) * size_z;
        p->alpha = rndf(0.72f, 0.95f) * alpha_z;
        break;
    case P_STAR:
        p->y = rndf(58.0f, EVA_WEATHER_RENDER_H * 0.58f);
        p->size = rndf(1.0f, 2.2f) * size_z;
        p->alpha = rndf(0.35f, 0.95f) * alpha_z;
        break;
    case P_DUST:
        p->y = rndf(EVA_WEATHER_RENDER_H * 0.32f, EVA_WEATHER_RENDER_H * 0.78f);
        p->vx = rndf(-4.0f, 7.0f) * speed_z;
        p->vy = rndf(-2.0f, 3.0f) * speed_z;
        p->size = rndf(1.0f, 2.0f) * size_z;
        p->alpha = rndf(0.18f, 0.42f) * alpha_z;
        break;
    case P_FOG:
        /* Subtle drifting bands rather than visible bubbles. Smaller radius
         * and much lower alpha than before — fog should feel like a haze
         * over the gradient, not separate translucent circles. */
        p->y = rndf(EVA_WEATHER_RENDER_H * 0.42f, EVA_WEATHER_RENDER_H * 0.96f);
        p->vx = rndf(-10.0f, 15.0f) * speed_z;
        p->vy = rndf(-1.0f, 1.0f);
        p->size = rndf(18.0f, 38.0f) * size_z;
        p->alpha = rndf(0.04f, 0.09f) * alpha_z;
        break;
    default:
        break;
    }
}

static particle_kind_t particle_kind_for_slot(uint16_t slot)
{
    switch (s_kind) {
    case WEATHER_RAIN:
        return P_RAIN;
    case WEATHER_HEAVY_RAIN:
        return P_RAIN;
    case WEATHER_THUNDERSTORM:
        return P_RAIN;
    case WEATHER_SNOW:
        return P_SNOW;
    case WEATHER_SLEET:
        return (slot & 1) ? P_SNOW : P_RAIN;
    case WEATHER_HAIL:
        return P_HAIL;
    case WEATHER_FOG:
        return P_FOG;
    case WEATHER_CLEAR_NIGHT:
    case WEATHER_PARTLY_CLOUDY_NIGHT:
        return P_STAR;
    case WEATHER_CLEAR_DAY:
        return P_DUST;
    default:
        return P_NONE;
    }
}

static void target_for_kind(weather_kind_t kind)
{
    float d = s_density_scale;
    switch (kind) {
    case WEATHER_RAIN:
        s_target = phi_count(64.0f * d, EVA_PHI2); s_max_target = phi_count(64.0f * d, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_HEAVY_RAIN:
        /* ~2x baseline of regular rain, capped at PARTICLE_MAX inside phi_count. */
        s_target = phi_count(120.0f * d, EVA_PHI2); s_max_target = phi_count(120.0f * d, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_THUNDERSTORM:
        s_target = phi_count(80.0f * d, EVA_PHI2); s_max_target = phi_count(80.0f * d, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_SNOW:
        s_target = phi_count(56.0f * d, EVA_PHI2); s_max_target = phi_count(56.0f * d, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_SLEET:
        s_target = phi_count(64.0f * d, EVA_PHI2); s_max_target = phi_count(64.0f * d, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_HAIL:
        s_target = phi_count(34.0f * d, EVA_PHI2); s_max_target = phi_count(34.0f * d, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_FOG:
        s_target = phi_count(34.0f, EVA_PHI2); s_max_target = phi_count(34.0f, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_CLEAR_NIGHT:
    case WEATHER_PARTLY_CLOUDY_NIGHT:
        s_target = phi_count(34.0f, EVA_PHI2); s_max_target = phi_count(34.0f, EVA_PHI2 * EVA_PHI); break;
    case WEATHER_CLEAR_DAY:
        s_target = phi_count(17.0f, EVA_PHI); s_max_target = phi_count(17.0f, EVA_PHI2); break;
    default:
        s_target = 0; s_max_target = 80; break;
    }
}

static void reset_particles_for_kind(void)
{
    target_for_kind(s_kind);
    memset(s_particles, 0, sizeof(s_particles));
    for (uint16_t i = 0; i < s_target && i < PARTICLE_MAX; ++i) {
        particle_kind_t pk = particle_kind_for_slot(i);
        if (pk != P_NONE) {
            spawn_particle(&s_particles[i], pk, false, i);
        }
    }
    /* Cloud strip scroll continues across weather changes for visual continuity. */
    s_prev_kind = s_kind;
}

static void ensure_particle_count(void)
{
    for (uint16_t i = 0; i < PARTICLE_MAX; ++i) {
        if (i < s_target) {
            particle_kind_t pk = particle_kind_for_slot(i);
            if (pk != P_NONE && s_particles[i].kind != pk) {
                spawn_particle(&s_particles[i], pk, false, i);
            }
        } else {
            s_particles[i].kind = P_NONE;
        }
    }
}

static bool is_night_kind(weather_kind_t kind)
{
    /* Explicit night kinds always render as night. */
    if (kind == WEATHER_CLEAR_NIGHT || kind == WEATHER_PARTLY_CLOUDY_NIGHT) {
        return true;
    }
    /* Explicit day kinds always render as day, even at 3 AM — the CDC
     * debug path uses these to force a day scene regardless of the wall clock.
     * Without this branch, weatherdebug clear-day at night would still draw
     * the moon. */
    if (kind == WEATHER_CLEAR_DAY || kind == WEATHER_PARTLY_CLOUDY_DAY) {
        return false;
    }
    /* Day-neutral kinds (RAIN, CLOUDY, FOG, SNOW, ...) fall back on the
     * current local time vs sunrise/sunset. */
    int sr, ss;
    sun_events(&sr, &ss);
    int m = minutes_now();
    if (sr < ss) {
        return (m < sr || m >= ss);
    }
    return (m >= ss && m < sr);
}

/* Single-pass moon renderer that draws only the lit fraction, using the
 * illuminated percentage from clearoutside plus the waxing/waning flag to
 * build a physically-shaped phase disc. */
static void draw_moon_phase(int cx, int cy, int r, uint16_t col, uint8_t alpha)
{
    if (r <= 0 || alpha == 0) return;
    int r2 = r * r;
    /* New moon: nothing to draw — entire disc is dark, no overlay needed
     * because the sky/clouds already provide the background. */
    if (s_moon_phase_pct <= 2) return;

    /* Full moon: simple lit disc. */
    if (s_moon_phase_pct >= 98) {
        draw_filled_circle(cx, cy, r, col, alpha);
        return;
    }

    /* Clearoutside's percentage is illumination, not a phase angle. Convert
     * it to a light direction for an orthographic sphere render:
     *   illum = 1.0 → full moon
     *   illum = 0.5 → quarter
     *   illum = 0.0 → new moon
     *
     * Waxing means the bright side sits on the right; waning means the left.
     * That matches the real moon as seen from the northern hemisphere. */
    float illum = (float)s_moon_phase_pct * 0.01f;
    if (illum < 0.0f) illum = 0.0f;
    if (illum > 1.0f) illum = 1.0f;
    float phase_angle = acosf(fmaxf(-1.0f, fminf(1.0f, 2.0f * illum - 1.0f)));
    float light_x = sinf(phase_angle);
    if (s_moon_waning) light_x = -light_x;
    float light_z = cosf(phase_angle);

    /* Fibonacci-based soft edge widths. The terminator uses a small smooth
     * band so the boundary doesn't look razor-sharp, and the rim still fades
     * off the disc like the rest of the sky elements. */
    const int rim_w  = FIB_8;
    const float term_soft = 0.17f;
    /* Pre-compute squared distances at which feathering starts. We compare
     * Euclidean distance (via sqrt) only when we're already in the feather
     * band to keep the inner loop cheap. */
    const int rim_inner = (r > rim_w) ? (r - rim_w) : 0;
    const int rim_inner2 = rim_inner * rim_inner;

    int x0 = cx - r;
    int x1 = cx + r;
    int y0 = cy - r;
    int y1 = cy + r;
    for (int y = y0; y <= y1; ++y) {
        int dy = y - cy;
        int dy2 = dy * dy;
        if (dy2 > r2) continue;
        for (int x = x0; x <= x1; ++x) {
            int dx = x - cx;
            int d2 = dx * dx + dy2;
            if (d2 > r2) continue;           /* outside moon — skip entirely */

            float nx = (float)dx / (float)r;
            float ny = (float)dy / (float)r;
            float nz2 = 1.0f - nx * nx - ny * ny;
            if (nz2 < 0.0f) continue;
            float nz = sqrtf(nz2);
            float lit = nx * light_x + nz * light_z;
            if (lit <= 0.0f) continue;        /* dark side → skip entirely */

            uint8_t a = alpha;

            /* Soften the terminator just enough to avoid a knife-edge cut. */
            if (lit < term_soft) {
                float u = lit / term_soft;
                if (u < 0.0f) u = 0.0f;
                if (u > 1.0f) u = 1.0f;
                float smooth = u * u * (3.0f - 2.0f * u);
                a = (uint8_t)((float)a * smooth);
            }

            /* Outer rim: Fibonacci-width smoothstep fade from full alpha at
             * (r - FIB_8) down to 0 at r. */
            if (d2 > rim_inner2) {
                float dist = sqrtf((float)d2);
                float t = (float)r - dist;           /* px into feather, 0..rim_w */
                if (t < 0.0f) t = 0.0f;
                float u = t / (float)rim_w;          /* 0 at rim, 1 at inner edge */
                if (u > 1.0f) u = 1.0f;
                float smooth = u * u * (3.0f - 2.0f * u);
                a = (uint8_t)((float)a * smooth);
            }

            if (a == 0) continue;
            blend_px(x, y, col, a);
        }
    }
}

/* Returns 0..1 progress through the visible arc, or -1 if the body is below
 * the horizon right now (caller should skip drawing).
 *
 * `rise` and `set` are minutes-of-day; if set < rise the arc wraps midnight
 * (typical for the moon — rises late evening, sets next morning). `now_min`
 * is the current local minute-of-day (0..1439). */
static float arc_progress(int rise, int set, int now_min)
{
    if (rise < 0 || set < 0) return -1.0f;
    int duration = set - rise;
    if (duration <= 0) duration += 1440;   /* wrap midnight */
    int elapsed = now_min - rise;
    if (elapsed < 0) elapsed += 1440;
    if (elapsed >= duration) return -1.0f; /* below horizon */
    return (float)elapsed / (float)duration;
}

static float sun_curve(float progress)
{
    /* 0 at sunrise/sunset, 1 near solar noon. */
    if (progress <= 0.0f || progress >= 1.0f) return 0.0f;
    return sinf(progress * 3.1415926f);
}

/* Total sky cover in [0..1], modelling each layer as an independent
 * transmittance. High clouds are weighted a bit lighter for sun/moon
 * visibility so thin cirrus does not erase the luminary too aggressively.
 * Three layers fully at 100 % still give total=1.0 (sky is solid). */
static float sky_cover_fraction(void)
{
    /* Single source of truth: open-meteo's lifestyle cloud cover. Replaces
     * the old multiplicative-transmittance combine of L/M/H which over-
     * counted overlapping layers and hid the sun on sunny-with-gaps days.
     * Until the coordinator populates this (first fetch), returns 0 = clear. */
    return (float)s_cloud_cover_pct * 0.01f;
}

static void draw_sun_or_moon(float t)
{
    s_sun_visible = false;
    s_sun_strength = 0.0f;
    s_sun_x = -1;
    s_sun_y = -1;
    s_sun_r = 0;

    int m = minutes_now();

    /* Storm kinds never show the luminary — overcast is total enough that
     * neither sun nor moon should be visible. Lightning is the only light
     * source during a thunderstorm. */
    bool stormy = (s_kind == WEATHER_THUNDERSTORM ||
                   s_kind == WEATHER_HEAVY_RAIN);
    if (stormy) return;

    /* Compute attenuation from cloud cover. Even at 85 % cover the disc
     * should still be visible as a pale glow ("сонце крізь хмари"); only at
     * truly overcast (≥98 %) does it vanish. The disc gets a higher floor
     * than the halos so it punches through partial cloud, while the outer
     * flare/corona still fade off aggressively (those wash out in real
     * skies long before the disc itself does). */
    float cover = sky_cover_fraction();
    if (cover >= 0.98f) return;            /* near-overcast: hide entirely */
    float vis_raw = 1.0f - (cover / 0.98f);
    /* Smoothstep keeps the *halo* falloff soft. The disc uses a much
     * gentler curve (sqrt-like) so at 85 % cover it's still ~40 % alpha
     * instead of 5 %. */
    float vis = vis_raw * vis_raw * (3.0f - 2.0f * vis_raw);
    float vis_disc = sqrtf(vis_raw);

    if (is_night_kind(s_kind)) {
        /* Default hardcoded position if moon astronomy isn't available yet. */
        float mx = 0.75f, my = 0.27f;
        float progress = arc_progress(s_moonrise_min, s_moonset_min, m);
        if (progress >= 0.0f) {
            float angle = progress * 3.1415926f;
            mx = 0.50f + sinf(angle - 1.5707963f) * 0.40f;
            my = 0.30f - sinf(angle) * 0.16f;
        }
        int moon_x = (int)(EVA_WEATHER_RENDER_W * mx);
        int moon_y = (int)(EVA_WEATHER_RENDER_H * my);
        /* Moon doesn't have a sun-like halo, just the disc — but we still
         * scale its visibility by cloud cover for partly-cloudy nights. */
        /* Moon disc alpha = FIB_233 baseline (close to old 230), scaled by
         * vis. The shadow disc that carves the lit fraction is FIB_233 +
         * FIB_8 ≈ 241. Cutoff at FIB_13 (13) — below this the silhouette
         * is invisible anyway, skip the draws. */
        uint8_t alpha_moon = (uint8_t)((float)FIB_233 * vis);
        if (alpha_moon < FIB_13) return;
        uint16_t moon_col = rgb565(245, 244, 225);
        /* Moon radius doubled to match the 2× sun (the moon used to look
         * tiny against the bigger sun and the native 800×480 canvas).
         * Was FIB_21 + FIB_8 = 29, now ~58. */
        const int moon_r = (FIB_21 + FIB_8) * 2;
        /* Single-pass lit-only renderer — only the illuminated fraction is
         * painted. The dark side stays transparent, so no shadow disc ever
         * bleeds outside the moon silhouette into the surrounding sky. */
        draw_moon_phase(moon_x, moon_y, moon_r, moon_col, alpha_moon);
        return;
    }

    /* Day path: show the sun only while it is above the horizon, so it
     * naturally rises from the left, peaks near noon, then drops away on
     * the right instead of lingering as a fixed sprite after sunset. */
    int sr, ss;
    sun_events(&sr, &ss);
    bool forced_day = (s_kind == WEATHER_CLEAR_DAY || s_kind == WEATHER_PARTLY_CLOUDY_DAY);
    if ((m < sr || m >= ss) && !forced_day) return;   /* below the horizon */

    float daylight = (float)(ss - sr);
    if (daylight <= 1.0f) daylight = 12.0f * 60.0f;
    float progress = (float)(m - sr) / daylight;
    if (forced_day && (m < sr || m >= ss)) {
        progress = 0.46f;   /* debug/unsynced clock: show a believable daytime sun */
    }
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    float arc = sun_curve(progress);
    float horizon_boost = 1.0f - arc;  /* bigger and warmer near horizon */
    float sun_x_n = 0.04f + progress * 0.92f;
    float sun_y_n = 0.24f - arc * 0.16f;
    int sun_x = (int)(EVA_WEATHER_RENDER_W * sun_x_n);
    int sun_y = (int)(EVA_WEATHER_RENDER_H * sun_y_n);
    /* Real phone photos show a small white saturated core with most of the
     * perceived size coming from glare. Keep the disc compact and let the
     * sky-glare/god-ray passes sell the brightness. */
    int r = (int)(18.0f + horizon_boost * 9.0f + sinf(t * 0.15f) * 1.2f);
    uint8_t a_outer  = (uint8_t)((float)FIB_21 * vis * (0.65f + 0.35f * horizon_boost));
    uint8_t a_corona = (uint8_t)((float)FIB_34 * vis * (0.75f + 0.25f * horizon_boost));
    uint8_t a_glow   = (uint8_t)((float)FIB_89 * vis_disc * (0.85f + 0.15f * horizon_boost));
    uint8_t a_disc   = (uint8_t)((float)255 * vis_disc);
    if (a_outer)  draw_filled_circle(sun_x, sun_y, r * 8, rgb565(210, 232, 255), a_outer);
    if (a_corona) draw_filled_circle(sun_x, sun_y, r * 5, rgb565(255, 244, 204), a_corona);
    if (a_glow)   draw_filled_circle(sun_x, sun_y, r * 2, rgb565(255, 252, 220), a_glow);
    if (a_disc)   draw_filled_circle(sun_x, sun_y, r,     rgb565(255, 255, 246), a_disc);

    /* Remember sun position + radius so the post-cloud god-ray pass (drawn
     * after the cloud composite) can paint a warm light overlay through the
     * cloud cover — sun should illuminate, not be hidden behind clouds. */
    s_sun_x = sun_x;
    s_sun_y = sun_y;
    s_sun_r = r;
    s_sun_visible = (a_disc > FIB_8);
    s_sun_strength = vis_disc;
}

/* God-rays / sunlight scatter — painted AFTER clouds so the sun reads as a
 * light source illuminating the scene, not a disc hidden behind a grey wall.
 *
 * Two passes:
 *   1. A huge, very-low-alpha warm circle centred on the sun. This is the
 *      "atmospheric haze" that makes the sky lighter and warmer around the
 *      sun even when clouds partially cover it. Additive-ish via blend_px so
 *      it lifts both clear sky pixels and darker cloud pixels toward warm white.
 *   2. Radial light shafts (8 rays) emanating outward — sampled as wedges of
 *      decreasing alpha. These read as visible "god rays" piercing the clouds.
 *
 * Skipped during storms (sun already hidden by draw_sun_or_moon) and at night. */
static void draw_sun_god_rays(float t)
{
    if (!s_sun_visible || s_sun_r <= 0) return;
    if (is_night_kind(s_kind)) return;
    bool stormy = (s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_HEAVY_RAIN);
    if (stormy) return;

    /* Attenuate the ray strength by sky cover — at fully overcast (>=98 %)
     * the sun pass already returned, but for partly-cloudy days we want the
     * rays to feel stronger when there are gaps (low cover) than when the
     * sky is almost full (mostly diffuse light). */
    float cover = sky_cover_fraction();
    float ray_vis = 1.0f - cover * 0.6f;        /* keeps some warmth even on cloudy days */
    if (ray_vis < 0.20f) ray_vis = 0.20f;

    /* Pass 1: large soft warm halo over the whole sun region. Radius is 8×
     * the disc — covers most of the sky above the sun. Alpha is very low so
     * it doesn't wash out clouds, just tints them warm. */
    int halo_r = s_sun_r * 8;
    uint8_t halo_alpha = (uint8_t)(18.0f * ray_vis);
    if (halo_alpha) {
        draw_filled_circle(s_sun_x, s_sun_y, halo_r,
                           rgb565(255, 220, 150), halo_alpha);
    }

    /* Pass 2: 8 radial rays emanating from the sun. Drawn as wedges via
     * per-pixel marching: for each ray angle, walk pixel-by-pixel outward
     * and paint a perpendicular strip whose width and alpha decay with
     * distance. Step=2 px so the strips overlap into smooth beams instead
     * of looking like a string of beads (the previous step=s_sun_r/3 left
     * visible gaps between sample circles). */
    const int NUM_RAYS = 8;
    float base_angle = t * 0.04f;               /* slow rotation, ~14 °/s */
    int ray_len = s_sun_r * 10;                 /* reach far across screen */

    for (int k = 0; k < NUM_RAYS; ++k) {
        float ang = base_angle + (float)k * (2.0f * 3.1415926f / (float)NUM_RAYS);
        float dx_u = cosf(ang);
        float dy_u = sinf(ang);
        /* Perpendicular unit vector (for the strip's transverse axis). */
        float nx = -dy_u;
        float ny =  dx_u;

        /* Walk outward in 2 px increments so adjacent samples overlap. */
        for (int d = s_sun_r; d < ray_len; d += 2) {
            float fade = 1.0f - (float)d / (float)ray_len;
            if (fade <= 0.0f) break;
            /* Beam half-width grows slowly with distance, capped so the ray
             * stays narrow at the tip rather than ballooning. */
            int half_w = 2 + (d - s_sun_r) / 24;
            if (half_w > 8) half_w = 8;
            uint8_t a_center = (uint8_t)(24.0f * fade * fade * ray_vis);
            if (a_center == 0) continue;

            int cx_d = s_sun_x + (int)(dx_u * (float)d);
            int cy_d = s_sun_y + (int)(dy_u * (float)d);

            /* Paint a soft perpendicular strip with falloff toward the edges. */
            for (int t_off = -half_w; t_off <= half_w; ++t_off) {
                int px = cx_d + (int)(nx * (float)t_off);
                int py = cy_d + (int)(ny * (float)t_off);
                /* Triangle falloff: center is brightest, edge is 0. */
                int abs_t = (t_off < 0) ? -t_off : t_off;
                uint8_t a = (uint8_t)((int)a_center * (half_w + 1 - abs_t) / (half_w + 1));
                if (a) {
                    blend_px(px, py, rgb565(245, 236, 210), a);
                }
            }
        }
    }
}

/* Helper PRNG for procedural sprite generation, seeded per sprite */
static float cloud_sprite_rnd_seeded(uint32_t *seed, float lo, float hi)
{
    *seed = (*seed) * 1664525U + 1013904223U;
    return lo + (hi - lo) * ((float)(*seed & 0xffffU) / 65535.0f);
}

/* Generate a single cloud sprite as a 200×100 A8 mask.
 * Procedurally creates bulbous cumulus-like shapes using overlapping circles.
 * Each shape_id produces a different outline so clouds don't look identical. */
static void generate_cloud_sprite(int shape_id, cloud_sprite_t *out)
{
    uint8_t *a8 = out->a8_data;
    memset(a8, 0, CLOUD_SPRITE_BYTES);

    /* Pseudo-random parameters per shape_id for variety */
    uint32_t seed = 0x12345678U + (uint32_t)shape_id * 0x6c5ce7U;

    /* 3-4 overlapping circles per shape, creating bulbous cloud outline */
    int num_bumps = 3 + (shape_id % 2);  /* 3 or 4 bumps */
    for (int b = 0; b < num_bumps; ++b) {
        float cx_norm = 0.2f + (float)b * (0.6f / (float)num_bumps) +
                        cloud_sprite_rnd_seeded(&seed, -0.1f, 0.1f);
        float cy_norm = 0.3f + cloud_sprite_rnd_seeded(&seed, -0.15f, 0.15f);
        float r_norm  = 0.25f + cloud_sprite_rnd_seeded(&seed, -0.08f, 0.12f);

        int cx = (int)(cx_norm * CLOUD_SPRITE_W);
        int cy = (int)(cy_norm * CLOUD_SPRITE_H);
        int r  = (int)(r_norm * CLOUD_SPRITE_W);

        if (r < 5) r = 5;
        if (r > 80) r = 80;

        /* Draw anti-aliased circle into A8 buffer using Bresenham with alpha blending */
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                int py = cy + dy;
                int px = cx + dx;
                if (px < 0 || px >= CLOUD_SPRITE_W || py < 0 || py >= CLOUD_SPRITE_H)
                    continue;

                float dist = sqrtf((float)(dx * dx + dy * dy));
                float rel_dist = dist / (float)r;
                uint8_t alpha;
                if (rel_dist < 0.95f) {
                    alpha = 255;  /* fully opaque interior */
                } else if (rel_dist < 1.05f) {
                    /* soft feather edge for anti-aliasing */
                    alpha = (uint8_t)(255.0f * (1.05f - rel_dist) * 10.0f);
                } else {
                    continue;  /* outside */
                }

                int idx = py * CLOUD_SPRITE_W + px;
                /* max() blend: keep brightest alpha */
                a8[idx] = (alpha > a8[idx]) ? alpha : a8[idx];
            }
        }
    }

    /* Set base tint color (will be modulated per-cloud at render time) */
    out->base_r = (uint8_t)(200 + (shape_id * 7) % 50);
    out->base_g = (uint8_t)(200 + ((shape_id + 3) * 7) % 50);
    out->base_b = (uint8_t)(210 + ((shape_id + 1) * 5) % 30);
}

/* Initialize cloud sprite atlas once at startup.
 * Allocates 8 × 20000 bytes = 160 KB in PSRAM for all cloud sprites. */
static void cloud_sprite_atlas_init_if_needed(void)
{
    if (s_cloud_atlas_inited) return;

    ESP_LOGI(TAG, "Allocating cloud sprite atlas (%d sprites × %d bytes)...",
             CLOUD_SPRITE_ATLAS_COUNT, CLOUD_SPRITE_BYTES);

    for (int i = 0; i < CLOUD_SPRITE_ATLAS_COUNT; ++i) {
        /* Allocate A8 data buffer in PSRAM (64-byte aligned for DMA/PPA) */
        s_cloud_atlas[i].a8_data = (uint8_t *)heap_caps_aligned_alloc(64, CLOUD_SPRITE_BYTES,
                                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_cloud_atlas[i].a8_data) {
            ESP_LOGE(TAG, "Failed to allocate cloud sprite %d", i);
            return;
        }

        /* Generate the procedural sprite */
        generate_cloud_sprite(i, &s_cloud_atlas[i]);
    }

    s_cloud_atlas_inited = true;
    ESP_LOGI(TAG, "Cloud sprite atlas ready (%d KB allocated)",
             (CLOUD_SPRITE_ATLAS_COUNT * CLOUD_SPRITE_BYTES) / 1024);
}

static void cloud3d_respawn(cloud3d_t *c, bool at_horizon)
{
    c->x = rndf(-0.10f, 1.10f);
    c->y = at_horizon ? rndf(-0.08f, 0.02f) : rndf(-0.10f, 1.05f);
    c->scale = 0.3f + c->y * 1.7f;
    c->vx = rndf(-0.25f, 0.25f);
    c->alpha = (uint8_t)rndf(96.0f, 220.0f);
    c->shape_id = (uint8_t)(rnd_u32() % CLOUD_SPRITE_ATLAS_COUNT);
    c->seed = (uint8_t)(rnd_u32() & 0xffU);
}

static void cloud3d_init_if_needed(void)
{
    if (s_clouds3d_inited) return;
    cloud_sprite_atlas_init_if_needed();
    for (int i = 0; i < CLOUD_3D_MAX; ++i) {
        cloud3d_respawn(&s_clouds3d[i], false);
    }
    s_clouds3d_inited = true;
}

/* CPU-side blend fallback for cloud sprites when PPA unavailable or as reference.
 * Blends a single cloud sprite A8 mask into the render buffer at given position/scale
 * using per-cloud tint color and alpha. Used when PPA blend is disabled or for testing. */
static void blend_cloud_sprite_cpu(uint16_t *dst, int dst_w, int dst_h,
                                    const cloud_sprite_t *sprite, int dst_x, int dst_y,
                                    float scale, uint8_t alpha, uint16_t tint_col)
{
    /* Scaled dimensions (sprite 200×100 × scale) */
    int sw = (int)((float)CLOUD_SPRITE_W * scale);
    int sh = (int)((float)CLOUD_SPRITE_H * scale);
    if (sw < 2 || sh < 2) return;

    /* Sprite origin at dst_x, dst_y (center-ish positioning) */
    int sx0 = dst_x - sw / 2;
    int sy0 = dst_y - sh / 2;

    for (int sy = 0; sy < sh; ++sy) {
        int src_y = (int)((float)sy / scale);
        if (src_y < 0 || src_y >= CLOUD_SPRITE_H) continue;

        for (int sx = 0; sx < sw; ++sx) {
            int src_x = (int)((float)sx / scale);
            if (src_x < 0 || src_x >= CLOUD_SPRITE_W) continue;

            int dx = sx0 + sx;
            int dy = sy0 + sy;
            if (dx < 0 || dx >= dst_w || dy < 0 || dy >= dst_h) continue;

            uint8_t sprite_alpha = sprite->a8_data[src_y * CLOUD_SPRITE_W + src_x];
            if (sprite_alpha == 0) continue;

            uint8_t final_alpha = (uint8_t)(((int)sprite_alpha * alpha) >> 8);
            int dst_idx = dy * dst_w + dx;
            dst[dst_idx] = blend565(dst[dst_idx], tint_col, final_alpha);
        }
    }
}

/* Draw all active cloud3d particles using sprite rendering.
 * Updates each cloud's position/scale, then renders either via PPA or CPU fallback. */
static void draw_clouds_3d(float dt)
{
    cloud3d_init_if_needed();
    int active = s_clouds3d_active;
    if (active < 1) active = 1;
    if (active > CLOUD_3D_MAX) active = CLOUD_3D_MAX;

    /* Physics: approach motion (y increases as clouds move toward viewer) */
    for (int i = 0; i < active; ++i) {
        cloud3d_t *c = &s_clouds3d[i];
        float lateral_wind = s_wind_vx_bias * 0.0007f;
        c->y += dt * (0.15f + c->scale * 0.10f);  /* closer clouds move faster */
        c->scale = 0.3f + c->y * 1.7f;
        c->x += dt * (c->vx + lateral_wind);

        /* Wrap horizontally */
        if (c->x < -0.25f) c->x += 1.5f;
        if (c->x > 1.25f) c->x -= 1.5f;

        /* Respawn at horizon when passing viewer */
        if (c->y > 1.15f) {
            cloud3d_respawn(c, true);
        }
    }

    /* Sort by depth (y) for painter's algorithm (back-to-front) */
    int order[CLOUD_3D_MAX];
    for (int i = 0; i < active; ++i) order[i] = i;
    for (int i = 1; i < active; ++i) {
        int key = order[i];
        float key_y = s_clouds3d[key].y;
        int j = i - 1;
        while (j >= 0 && s_clouds3d[order[j]].y > key_y) {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }

    /* Determine cloud coloration based on weather type */
    bool stormy = (s_kind == WEATHER_RAIN || s_kind == WEATHER_HEAVY_RAIN ||
                   s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_SLEET ||
                   s_kind == WEATHER_HAIL);
    bool night = is_night_kind(s_kind);
    uint16_t base_col = night ? rgb565(132, 144, 170)
                             : (stormy ? rgb565(162, 172, 188) : rgb565(236, 240, 246));

    const float horizon_y = 0.18f;
    const float viewer_y = 0.88f;

    /* Render each cloud sprite in sorted order */
    for (int k = 0; k < active; ++k) {
        cloud3d_t *c = &s_clouds3d[order[k]];
        float y = c->y;

        /* Frustum cull: skip clouds outside vertical bounds */
        if (y < -0.12f || y > 1.20f) continue;

        /* Screen-space position: map horizon_y..viewer_y to top..bottom of screen */
        int cx = (int)(c->x * EVA_WEATHER_RENDER_W);
        int cy = (int)((horizon_y + y * (viewer_y - horizon_y)) * EVA_WEATHER_RENDER_H);

        /* Alpha fades out near horizon and when far (negative y) */
        uint8_t fade = (uint8_t)(clamp_u8((int)(c->alpha * (0.25f + 0.75f * y))));
        if (fade < 8) continue;

        /* Get sprite and blend into framebuffer */
        const cloud_sprite_t *sprite = &s_cloud_atlas[c->shape_id % CLOUD_SPRITE_ATLAS_COUNT];

        /* For Phase 3, use CPU blending as foundation.
         * PPA blending of A8→RGB565 requires color + alpha registers which are
         * driver-dependent on ESP32-P4. CPU fallback is correct, scalable, and
         * allows focus on 3D perspective algorithm before optimization.
         *
         * Future (Phase 4): When PPA A8 blending is stable, can optionally
         * use PPA SRM for scaling followed by blend operation. */
        blend_cloud_sprite_cpu(s_buf, EVA_WEATHER_RENDER_W, EVA_WEATHER_RENDER_H,
                               sprite, cx, cy, c->scale, fade, base_col);
    }
}

/* s_cloud_pct and s_fog_pct moved to the top-of-file static block so they
 * can be referenced from sky_cover_fraction() (above this point in the file).
 * Wind state — same reason. */

/* Helper: assign light+shadow+core tint to one strip. */
static inline void set_tint(cloud_strip_t *s,
                            uint8_t lr, uint8_t lg, uint8_t lb,
                            uint8_t sr, uint8_t sg, uint8_t sb,
                            uint8_t cr, uint8_t cg, uint8_t cb)
{
    s->tint_light_r = lr;  s->tint_light_g = lg;  s->tint_light_b = lb;
    s->tint_shadow_r = sr; s->tint_shadow_g = sg; s->tint_shadow_b = sb;
    s->tint_core_r = cr;   s->tint_core_g = cg;   s->tint_core_b = cb;
}

static void update_cloud_tints(void)
{
    bool stormy = (s_kind == WEATHER_RAIN || s_kind == WEATHER_HEAVY_RAIN ||
                   s_kind == WEATHER_THUNDERSTORM ||
                   s_kind == WEATHER_HAIL || s_kind == WEATHER_SLEET);
    bool foggy  = (s_kind == WEATHER_FOG);
    bool night = is_night_kind(s_kind);

    if (foggy) {
        /* Fog: low contrast between light/shadow — fog is omnidirectional
         * scattering, no clear sun direction. Both tints are warm-grey. */
        set_tint(&s_strip[CLOUD_LAYER_HIGH], 232, 232, 226, 195, 195, 190, 178, 178, 172);
        set_tint(&s_strip[CLOUD_LAYER_MID],  220, 218, 212, 175, 175, 170, 155, 155, 150);
        set_tint(&s_strip[CLOUD_LAYER_LOW],  208, 208, 202, 158, 158, 154, 136, 136, 132);
    } else if (stormy) {
        /* Storm: dramatic top/bottom contrast — cumulonimbus signature.
         * Top still gets some light through anvil edges (~mid grey).
         * Bottom is near-black because anvil depth blocks sky reflection. */
        set_tint(&s_strip[CLOUD_LAYER_HIGH], 170, 174, 188,  95, 100, 112,  72,  78,  92);
        set_tint(&s_strip[CLOUD_LAYER_MID],  140, 145, 158,  62,  66,  78,  44,  48,  60);
        set_tint(&s_strip[CLOUD_LAYER_LOW],  118, 122, 136,  38,  42,  52,  24,  28,  38);
    } else if (night) {
        /* Night: moonlight illuminates tops faintly cool-blue. Shadows
         * are very dark navy but NOT black — keeps cumulus from reading
         * as a solid silhouette against the dim sky. */
        set_tint(&s_strip[CLOUD_LAYER_HIGH], 130, 138, 158,  60,  66,  86,  48,  54,  72);
        set_tint(&s_strip[CLOUD_LAYER_MID],  108, 116, 138,  48,  54,  72,  34,  40,  58);
        set_tint(&s_strip[CLOUD_LAYER_LOW],   92, 100, 122,  35,  42,  58,  22,  28,  42);
    } else {
        /* Day: direct sunlight on tops (near-white with warm hint),
         * sky-blue scattered shadow on undersides. */
        set_tint(&s_strip[CLOUD_LAYER_HIGH], 255, 255, 252, 205, 216, 232, 170, 184, 204);
        set_tint(&s_strip[CLOUD_LAYER_MID],  255, 255, 252, 160, 176, 198, 112, 130, 158);
        set_tint(&s_strip[CLOUD_LAYER_LOW],  255, 255, 252, 118, 132, 156,  70,  84, 108);
    }

    for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
        uint8_t pct = s_cloud_pct[i];
        /* Map cloud_pct (0..100) to alpha_scale (0..254). 254 = FIB_233 + FIB_21
         * keeps the alpha ladder on the Fibonacci grid. The +50 in the
         * numerator is rounding (half of 100). */
        uint16_t max_alpha = (uint16_t)(FIB_233 + FIB_21);
        if (!stormy && !foggy && !night && pct > 0) {
            max_alpha = 255;
        }
        s_strip[i].alpha_scale = pct == 0 ? 0
                                          : (uint8_t)((pct * max_alpha + 50) / 100);
    }
}

/* Non-linear wind acceleration. Returns a multiplier that gets applied to
 * each layer's base_speed.
 *
 * Profile:
 *   0 kph  → 1.0×    (calm, only natural drift)
 *   10 kph → ~1.6×   (light breeze, noticeable)
 *   30 kph → ~2.8×   (moderate, clearly fast)
 *   60 kph → ~4.8×   (strong, dramatic)
 *   90 kph → ~6.5×   (storm-level, racing across screen)
 *   120kph → ~8.0×   (capped at FIB_8)
 *
 * sqrt-based curve matches perceived wind intensity better than linear:
 * a 30 → 60 kph doubling feels like ~1.7× more movement, not 2×. */
static float wind_speed_multiplier(float kph)
{
    if (kph <= 0.0f) return 1.0f;
    /* 1 + sqrt(kph) * phi/3 gives 10kph→1.7, 30kph→2.95, 60kph→4.18,
     * 90kph→5.12, 120kph→5.9. We then square-root once more to soften
     * the high end and clamp to FIB_8. */
    float m = 1.0f + sqrtf(kph) * (EVA_PHI / 3.0f);
    if (m > (float)FIB_8) m = (float)FIB_8;
    return m;
}

static void advance_cloud_scroll(float dt)
{
    float wf = wind_speed_multiplier(s_wind_kph_eff);
    /* Per-layer wind sensitivity. LOW cumulus catches gusts most (full wf),
     * MID altocumulus a bit less (wf^0.85), HIGH cirrus barely budges with
     * surface wind (wf^0.6). Golden-ratio exponent stack for the rhythm. */
    static const float layer_wind_exp[3] = {
        [CLOUD_LAYER_HIGH] = 0.5f,                /* ~1/φ */
        [CLOUD_LAYER_MID]  = 1.0f / EVA_PHI,      /* 0.618 — calmer */
        [CLOUD_LAYER_LOW]  = 1.0f,                /* full wind effect */
    };
    /* Direction of horizontal scroll follows the wind:
     *   s_wind_vx_bias > 0 → wind blows east → clouds drift east  (scroll_x ↑, default direction)
     *   s_wind_vx_bias < 0 → wind blows west → clouds drift west  (scroll_x ↓)
     *   |s_wind_vx_bias| ≈ 0 → calm, keep a gentle east-going default drift.
     * The threshold of ~4 px/s (≈ 2 kph) avoids jittery direction flips
     * when live wind data dithers around zero. */
    float dir_sign = 1.0f;
    if (s_wind_vx_bias < -4.0f) dir_sign = -1.0f;
    else if (s_wind_vx_bias > 4.0f) dir_sign = 1.0f;
    /* else: leave default east-going drift for calm conditions. */

    /* Vertical drift component. Wind direction in degrees: 0=North, 90=East.
     * Our screen X grows eastward (dir_x = -sin(rad)), Y down so Y component
     * uses cos(rad) — wind from the south drags clouds slightly downward.
     * Magnitude is small (~10 % of horizontal) so the effect is subtle. */
    float t_now = (float)esp_timer_get_time() * 1e-6f;
    for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
        float layer_wf = powf(wf, layer_wind_exp[i]);
        float speed_x = s_strip[i].base_speed * layer_wf * dir_sign;
        s_strip[i].scroll_x += dt * speed_x;
        /* Vertical bob: gentle sin wave whose amplitude grows with wind.
         * Each layer gets a different phase so they don't bob in unison. */
        float bob_amp = 0.4f + s_wind_kph_eff * 0.04f;     /* 0.4 .. ~5 px */
        float bob_freq = 0.18f + 0.04f * (float)i;          /* slow, distinct */
        s_strip[i].scroll_y_off =
            bob_amp * sinf(t_now * bob_freq + (float)i * EVA_PHI);
        while (s_strip[i].scroll_x >= (float)CLOUD_STRIP_W) {
            s_strip[i].scroll_x -= (float)CLOUD_STRIP_W;
        }
        while (s_strip[i].scroll_x < 0.0f) {
            s_strip[i].scroll_x += (float)CLOUD_STRIP_W;
        }
    }
}

/* Blend one mask (either light or shadow) using its associated tint colour.
 * `eff_y` is the effective destination y (strip->y_start + wind bob), clamped
 * by caller. CPU fallback used when PPA is unavailable. */
static void blend_mask_cpu(const cloud_strip_t *strip,
                           const uint8_t *mask,
                           uint8_t tr, uint8_t tg, uint8_t tb,
                           uint8_t alpha_scale,
                           int eff_y, int src_x, int dst_x, int width)
{
    if (!mask || width <= 0 || alpha_scale == 0) return;
    uint16_t tint = rgb565(tr, tg, tb);
    for (int y = 0; y < strip->strip_h; ++y) {
        const uint8_t *src = &mask[y * CLOUD_STRIP_W + src_x];
        int dst_y = eff_y + y;
        if ((unsigned)dst_y >= EVA_WEATHER_RENDER_H) continue;
        for (int x = 0; x < width; ++x) {
            uint8_t a = (uint8_t)(((uint16_t)src[x] * alpha_scale) / 255);
            if (a) {
                blend_px(dst_x + x, dst_y, tint, a);
            }
        }
    }
}

static esp_err_t blend_mask_ppa_one_band(const cloud_strip_t *strip,
                                         const uint8_t *mask,
                                         uint8_t tr, uint8_t tg, uint8_t tb,
                                         uint8_t alpha_scale,
                                         int eff_y,
                                         int src_x, int dst_x, int width)
{
    if (!s_ppa_blend || s_ppa_blend_disabled || !mask || alpha_scale == 0 || width <= 0) {
        return ESP_FAIL;
    }
    /* PPA needs non-negative offsets and the band must fit in the working
     * buffer. If wind bob pushed us partially outside, clip the strip height
     * to the visible region. */
    int strip_top = eff_y;
    int strip_h = strip->strip_h;
    int src_top = 0;
    if (strip_top < 0) {
        src_top = -strip_top;
        strip_h -= src_top;
        strip_top = 0;
    }
    if (strip_top + strip_h > EVA_WEATHER_RENDER_H) {
        strip_h = EVA_WEATHER_RENDER_H - strip_top;
    }
    if (strip_h <= 0) return ESP_OK;   /* fully off-screen: skip silently */

    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = s_buf,
            .pic_w = EVA_WEATHER_RENDER_W,
            .pic_h = EVA_WEATHER_RENDER_H,
            .block_w = (uint32_t)width,
            .block_h = (uint32_t)strip_h,
            .block_offset_x = (uint32_t)dst_x,
            .block_offset_y = (uint32_t)strip_top,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = (void *)mask,
            .pic_w = CLOUD_STRIP_W,
            .pic_h = (uint32_t)strip->strip_h,
            .block_w = (uint32_t)width,
            .block_h = (uint32_t)strip_h,
            .block_offset_x = (uint32_t)src_x,
            .block_offset_y = (uint32_t)src_top,
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer = s_buf,
            .buffer_size = EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
            .pic_w = EVA_WEATHER_RENDER_W,
            .pic_h = EVA_WEATHER_RENDER_H,
            .block_offset_x = (uint32_t)dst_x,
            .block_offset_y = (uint32_t)strip_top,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .fg_alpha_update_mode = PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = (float)alpha_scale / 256.0f,
        .fg_fix_rgb_val = { .b = tb, .g = tg, .r = tr },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_blend(s_ppa_blend, &cfg);
}

/* Composite one layer: shadow mask first (under the sky), light mask over.
 * Wraps the strip across the seam if scroll lands close to the edge.
 * `eff_y` accounts for the layer's wind-driven vertical bob — small offset
 * but stops the layer feeling locked into a fixed band. */
static void blend_layer_variant(cloud_strip_t *strip,
                                const cloud_variant_t *v,
                                uint8_t alpha_scale)
{
    if (!v || !v->a8_light || !v->a8_shadow || !v->a8_core || alpha_scale == 0) return;

    int scroll = (int)strip->scroll_x;
    if (scroll >= CLOUD_STRIP_W) scroll = 0;
    int max_src_w = CLOUD_STRIP_W - scroll;
    int first_w  = max_src_w >= EVA_WEATHER_RENDER_W ? EVA_WEATHER_RENDER_W : max_src_w;
    int second_w = EVA_WEATHER_RENDER_W - first_w;
    int eff_y = strip->y_start + (int)strip->scroll_y_off;

    /* Per-pass alpha modulation by coverage.
     *
     * Bug fix (2026-05-26): previously all 3 passes (shadow / core / light)
     * used the same `alpha_scale`. At low cover the masks are thin and the
     * darker shadow+core passes dominated visually — clouds came out grey/
     * dark even on a sunny day. At high cover the bright top pass painted
     * over everything and clouds went bright white. That was the opposite of
     * real meteorology.
     *
     * Cumulus humilis (clear/partly-cloudy days): fluffy, lit, almost no
     *   visible shadow — the underside is just barely tinted.
     * Stratocumulus / nimbostratus (overcast): solid base, strong shadow.
     * Cumulonimbus (storm): heavy dark base with bright anvil top.
     *
     * Mapping (alpha_scale 0..254 = pct 0..100 %):
     *   shadow_alpha = alpha_scale * (0.15 + 0.85 * pct)
     *   core_alpha   = alpha_scale * (0.10 + 0.90 * pct)
     *   light_alpha  = alpha_scale  (full strength regardless of cover)
     * So at pct=10 % shadow contributes ~24 % of light's weight; at pct=90 %
     * shadow contributes ~87 %. Light always paints the lit cap fully, so
     * thin clouds read as white tufts rather than dim grey smudges. */
    uint8_t shadow_alpha;
    uint8_t core_alpha;
    {
        /* alpha_scale ~ pct * 2.54, recover pct in 0..254 then map. */
        uint32_t a = alpha_scale;
        /* shadow weight: 15 % at pct=0, 100 % at pct=100. */
        uint32_t sw_q = (uint32_t)((a * 85u) / 100u + (254u * 15u) / 100u * a / 254u);
        if (sw_q > 254u) sw_q = 254u;
        shadow_alpha = (uint8_t)((a * sw_q) / 254u);
        /* core weight: 10 % at pct=0, 100 % at pct=100. */
        uint32_t cw_q = (uint32_t)((a * 90u) / 100u + (254u * 10u) / 100u * a / 254u);
        if (cw_q > 254u) cw_q = 254u;
        core_alpha = (uint8_t)((a * cw_q) / 254u);
    }
    uint8_t light_alpha = alpha_scale;

    /* Light-only rendering: shadow and core skipped for performance.
     *
     * Trade-off: clouds lose under-shadow and dense-centre passes. On
     * clear / partly-cloudy days this is close to what real cumulus
     * humilis looks like (lit, fluffy, very faint underside). On
     * overcast/storm scenes clouds will read flatter than before — the
     * darker base is gone — but the device cannot sustain 3-pass blends
     * within frame budget.
     *
     * If shadow needs to come back later, the gate should be
     * `s_cloud_cover_pct > 70 && tick_hz > target_hz` (only spend on
     * overcast when we have spare headroom). */
    (void)shadow_alpha;
    (void)core_alpha;

    esp_err_t err = ESP_OK;
    if (light_alpha) {
        err = blend_mask_ppa_one_band(strip, v->a8_light,
                                       strip->tint_light_r,
                                       strip->tint_light_g,
                                       strip->tint_light_b,
                                       light_alpha,
                                       eff_y, scroll, 0, first_w);
        if (err == ESP_OK && second_w > 0) {
            err = blend_mask_ppa_one_band(strip, v->a8_light,
                                           strip->tint_light_r,
                                           strip->tint_light_g,
                                           strip->tint_light_b,
                                           light_alpha,
                                           eff_y, 0, first_w, second_w);
        }
    }
    if (err == ESP_OK) return;

    if (s_ppa_blend && !s_ppa_blend_disabled) {
        ESP_LOGW(TAG, "PPA cloud blend failed once (err=%d), using CPU fallback", (int)err);
        s_ppa_blend_disabled = true;
    }
    /* CPU fallback for the single light pass. */
    if (light_alpha) {
        blend_mask_cpu(strip, v->a8_light,
                       strip->tint_light_r, strip->tint_light_g, strip->tint_light_b,
                       light_alpha,
                       eff_y, scroll, 0, first_w);
        if (second_w > 0) {
            blend_mask_cpu(strip, v->a8_light,
                           strip->tint_light_r, strip->tint_light_g, strip->tint_light_b,
                           light_alpha,
                           eff_y, 0, first_w, second_w);
        }
    }
}

static uint8_t alpha_scaled_by_float(uint8_t alpha, float k)
{
    if (k <= 0.0f || alpha == 0) return 0;
    if (k >= 1.0f) return alpha;
    return (uint8_t)((float)alpha * k + 0.5f);
}

static void update_cloud_lifecycle(float dt)
{
    for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
        cloud_strip_t *strip = &s_strip[i];
        if (strip->alpha_scale == 0) continue;

        strip->morph_clock += dt;
        if (!strip->morphing) {
            if (strip->morph_clock >= strip->morph_hold_s) {
                strip->morphing = true;
                strip->morph_t = 0.0f;
                strip->morph_clock = 0.0f;
            }
            continue;
        }

        float duration = strip->morph_duration_s > 1.0f ? strip->morph_duration_s : 1.0f;
        strip->morph_t += dt / duration;
        if (strip->morph_t >= 1.0f) {
            strip->active_variant ^= 1U;
            strip->morphing = false;
            strip->morph_t = 0.0f;
            strip->morph_clock = 0.0f;
            bake_strip_for_layer(i, &strip->variant[strip->active_variant ^ 1U],
                                 strip->strip_h);
        }
    }
}

static void blend_layer(cloud_strip_t *strip)
{
    /* Below this threshold the layer would only contribute imperceptible
     * pixels (alpha < FIB_8 ≈ 3 %) but still trigger 3 PPA blends. Skip it
     * so clear-day (cloud_pct~5) doesn't pay the full cloudy cost. */
    if (strip->alpha_scale < FIB_8) return;

    uint8_t active = strip->active_variant;
    uint8_t next = active ^ 1U;
    if (!strip->morphing) {
        blend_layer_variant(strip, &strip->variant[active], strip->alpha_scale);
        return;
    }

    float t = strip->morph_t;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    float eased = t * t * (3.0f - 2.0f * t);
    uint8_t a0 = alpha_scaled_by_float(strip->alpha_scale, 1.0f - eased);
    uint8_t a1 = alpha_scaled_by_float(strip->alpha_scale, eased);
    blend_layer_variant(strip, &strip->variant[active], a0);
    blend_layer_variant(strip, &strip->variant[next], a1);
}

static void draw_sun_cloud_light_variant(const cloud_strip_t *strip,
                                         const cloud_variant_t *v,
                                         uint8_t alpha_scale)
{
    if (!s_sun_visible || s_sun_strength <= 0.0f || is_night_kind(s_kind)) return;
    if (!strip || !v || !v->a8_light || alpha_scale == 0) return;

    int eff_y = strip->y_start + (int)strip->scroll_y_off;
    int scroll = (int)strip->scroll_x;
    if (scroll >= CLOUD_STRIP_W) scroll = 0;

    int r = (int)(260.0f + 80.0f * s_sun_strength);
    int r2 = r * r;
    int x0 = s_sun_x - r;
    int x1 = s_sun_x + r;
    int y0 = s_sun_y - r;
    int y1 = s_sun_y + r;
    if (x0 < 0) x0 = 0;
    if (x1 >= EVA_WEATHER_RENDER_W) x1 = EVA_WEATHER_RENDER_W - 1;
    if (y0 < eff_y) y0 = eff_y;
    if (y1 >= eff_y + strip->strip_h) y1 = eff_y + strip->strip_h - 1;
    if (y0 > y1 || x0 > x1) return;

    uint16_t rim = rgb565(255, 255, 246);
    uint16_t warm = rgb565(255, 236, 184);
    for (int y = y0; y <= y1; y += 2) {
        int ly = y - eff_y;
        if ((unsigned)ly >= (unsigned)strip->strip_h) continue;
        const uint8_t *light_row = &v->a8_light[ly * CLOUD_STRIP_W];
        for (int x = x0; x <= x1; x += 2) {
            int sx = (scroll + x) % CLOUD_STRIP_W;
            int mask = light_row[sx];
            if (mask <= FIB_13) continue;

            int dx = x - s_sun_x;
            int dy = y - s_sun_y;
            int d2 = dx * dx + dy * dy;
            if (d2 >= r2) continue;
            float near = 1.0f - (float)d2 / (float)r2;
            float top_bias = 1.0f - (float)ly / (float)strip->strip_h;
            if (top_bias < 0.0f) top_bias = 0.0f;
            float k = (0.25f + 0.75f * near) * (0.45f + 0.55f * top_bias) * s_sun_strength;
            int a = (int)((float)mask * (float)alpha_scale * k / 255.0f);
            if (a <= 0) continue;
            if (a > 86) a = 86;
            blend_px(x, y, rim, (uint8_t)a);
            blend_px(x + 1, y, rim, (uint8_t)a);
            blend_px(x, y + 1, rim, (uint8_t)a);
            blend_px(x + 1, y + 1, rim, (uint8_t)a);
            if (near > 0.35f) {
                uint8_t wa = smooth_u8((near - 0.35f) / 0.65f, (uint8_t)(a / 2));
                blend_px(x, y, warm, wa);
                blend_px(x + 1, y, warm, wa);
                blend_px(x, y + 1, warm, wa);
                blend_px(x + 1, y + 1, warm, wa);
            }
        }
    }
}

static void draw_sun_cloud_lighting(void)
{
    if (!s_sun_visible || is_night_kind(s_kind)) return;
    bool stormy = (s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_HEAVY_RAIN);
    if (stormy) return;

    for (int i = CLOUD_LAYER_COUNT - 1; i >= 0; --i) {
        cloud_strip_t *strip = &s_strip[i];
        if (strip->alpha_scale == 0) continue;
        int active = strip->active_variant % CLOUD_VARIANT_COUNT;
        int next = (active + 1) % CLOUD_VARIANT_COUNT;
        if (!strip->morphing) {
            draw_sun_cloud_light_variant(strip, &strip->variant[active], strip->alpha_scale);
            continue;
        }
        float t = strip->morph_t;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        float eased = t * t * (3.0f - 2.0f * t);
        draw_sun_cloud_light_variant(strip, &strip->variant[active],
                                     alpha_scaled_by_float(strip->alpha_scale, 1.0f - eased));
        draw_sun_cloud_light_variant(strip, &strip->variant[next],
                                     alpha_scaled_by_float(strip->alpha_scale, eased));
    }
}

static void compose_clouds_into_working_buffer(float dt)
{
    update_cloud_tints();
    update_cloud_lifecycle(dt);
    advance_cloud_scroll(dt);
    /* Single-layer cloud rendering — only LOW cumulus (most visually
     * dominant). HIGH cirrus and MID altocumulus added too much PPA cost
     * (3 passes × 3 layers = 9 PPA blends, ~80-115 ms cl) on this device's
     * PSRAM bandwidth budget. LOW alone preserves the "cloud" look in
     * exchange for less stratified depth — the trade chosen because the
     * device cannot sustain >12 Hz with all three.
     *
     * Lifecycle (tint/scroll/morph) still ticks for all three layers so
     * that switching back to multi-layer is a one-line change here.
     * If we ever want to add MID back on heavy cover scenes, the right
     * condition is `s_cloud_cover_pct > 70` — but only after async cloud
     * bake or PPA non-blocking are in place. */
    blend_layer(&s_strip[CLOUD_LAYER_LOW]);
}

static void draw_fog_bands(float t)
{
    uint16_t fog = rgb565(230, 232, 225);
    for (int band = 0; band < 4; ++band) {
        int base_y = (int)(EVA_WEATHER_RENDER_H * (0.34f + band * 0.16f));
        float drift = sinf(t * 0.18f + band) * 22.0f;
        for (int x = 0; x < EVA_WEATHER_RENDER_W; x += 2) {
            int y = base_y + (int)(sinf((float)x * 0.020f + t * 0.30f + band) * 18.0f + drift);
            for (int yy = 0; yy < 18; ++yy) {
                blend_px(x, y + yy, fog, FIB_34 - FIB_5);     /* 29 — wisp alpha */
                blend_px(x + 1, y + yy, fog, FIB_34 - FIB_5);
            }
        }
    }
}

static void update_and_draw_particles(float dt, float t)
{
    ensure_particle_count();
    for (uint16_t i = 0; i < s_target && i < PARTICLE_MAX; ++i) {
        particle_t *p = &s_particles[i];
        switch (p->kind) {
        case P_RAIN: {
            p->x += p->vx * dt + sinf(t * 0.6f + p->phase) * 0.35f;
            p->y += p->vy * dt;
            /* Respawn when off-screen in any direction (wind can push rain
             * left OR right depending on dir). The 80 px slop avoids visible
             * pop-in at the edges. */
            if (p->y > EVA_WEATHER_RENDER_H + 40.0f ||
                p->x < -80.0f || p->x > EVA_WEATHER_RENDER_W + 80.0f) {
                spawn_particle(p, P_RAIN, true, i);
            }
            uint8_t alpha = clamp_u8((int)(p->alpha * 200.0f));
            int x0 = (int)p->x;
            int y0 = (int)p->y;
            /* Streak direction follows actual velocity, so wind from the
             * east tilts streaks /, west tilts them \. Streak length is
             * p->size; horizontal extent is size * (vx/vy) ratio. vy is
             * always > 0 for rain so no divide-by-zero. */
            float vy_safe = (p->vy > 1.0f) ? p->vy : 1.0f;
            int x1 = x0 + (int)(p->size * (p->vx / vy_safe));
            int y1 = y0 + (int)p->size;
            draw_line(x0, y0, x1, y1,
                      rgb565(214, 234, 255), alpha, p->size > 14.0f ? 1 : 0);
            break;
        }
        case P_SNOW: {
            float sway_amp = 16.0f + fminf(s_wind_kph_eff, 60.0f) * 0.25f;
            float sway = sinf(t * (0.5f + fabsf(p->spin) * 0.5f) + p->phase) * sway_amp;
            p->x += (p->vx + sway) * dt;
            p->y += p->vy * dt;
            p->phase += p->spin * dt;
            if (p->y > EVA_WEATHER_RENDER_H + 20.0f) {
                spawn_particle(p, P_SNOW, true, i);
            }
            if (p->x < -12.0f) p->x = EVA_WEATHER_RENDER_W + 8.0f;
            if (p->x > EVA_WEATHER_RENDER_W + 12.0f) p->x = -8.0f;
            uint8_t alpha = clamp_u8((int)(p->alpha * 205.0f));
            int x = (int)p->x;
            int y = (int)p->y;
            int r = (int)p->size;
            draw_line(x - r, y, x + r, y, rgb565(255, 255, 255), alpha, 0);
            draw_line(x, y - r, x, y + r, rgb565(255, 255, 255), alpha, 0);
            blend_px(x, y, rgb565(255, 255, 255), alpha);
            break;
        }
        case P_HAIL: {
            p->x += p->vx * dt;
            p->y += p->vy * dt;
            if (p->y > EVA_WEATHER_RENDER_H + 24.0f ||
                p->x < -20.0f || p->x > EVA_WEATHER_RENDER_W + 20.0f) {
                spawn_particle(p, P_HAIL, true, i);
            }
            draw_filled_circle((int)p->x, (int)p->y, (int)p->size,
                               rgb565(238, 248, 255), clamp_u8((int)(p->alpha * 230.0f)));
            break;
        }
        case P_STAR: {
            float tw = 0.45f + 0.55f * sinf(t * 0.8f + p->phase);
            uint8_t alpha = clamp_u8((int)(p->alpha * tw * 220.0f));
            draw_filled_circle((int)p->x, (int)p->y, (int)p->size,
                               rgb565(255, 255, 245), alpha);
            break;
        }
        case P_DUST: {
            p->x += p->vx * dt;
            p->y += p->vy * dt + sinf(t * 0.4f + p->phase) * 0.05f;
            if (p->x < -10.0f || p->x > EVA_WEATHER_RENDER_W + 10.0f ||
                p->y < 120.0f || p->y > EVA_WEATHER_RENDER_H + 10.0f) {
                spawn_particle(p, P_DUST, false, i);
            }
            draw_filled_circle((int)p->x, (int)p->y, (int)p->size,
                               rgb565(255, 235, 180), clamp_u8((int)(p->alpha * 120.0f)));
            break;
        }
        case P_FOG: {
            p->x += p->vx * dt;
            if (p->x < -120.0f) p->x = EVA_WEATHER_RENDER_W + 80.0f;
            if (p->x > EVA_WEATHER_RENDER_W + 120.0f) p->x = -80.0f;
            draw_filled_circle((int)p->x, (int)p->y, (int)p->size,
                               rgb565(220, 222, 216), clamp_u8((int)(p->alpha * 180.0f)));
            break;
        }
        default:
            break;
        }
    }
}

/* Lightning has two phases:
 *   1. update_lightning(t)         — triggers new strikes, picks bolt segment
 *                                    endpoints in working-buffer coordinates,
 *                                    and decays alpha.
 *   2. composite_lightning_on_render() — paints flash + bolt into the working
 *                                    buffer. LVGL scales the final canvas 2x,
 *                                    which is cheaper than rewriting a full
 *                                    800x480 buffer here every frame.
 */
static void update_lightning(float t)
{
    if (s_kind != WEATHER_THUNDERSTORM) {
        s_lightning_alpha = 0.0f;
        s_lightning_cooldown = 0;
        return;
    }
    float p = sinf(t * 2.5f) * sinf(t * 5.3f) * sinf(t * 7.1f);
    if (s_lightning_cooldown > 0) {
        s_lightning_cooldown--;
    } else if (p > 0.58f && s_lightning_alpha < (float)FIB_5) {
        /* Initial lightning brightness: FIB_144 + FIB_34 = 178 baseline plus
         * up to FIB_55 jitter — that gives a flash alpha in 178..233 (uint8
         * range cap is 255, so we stay just below saturation). Decay below
         * uses EVA_INV_PHI² (1/φ² ≈ 0.382) per frame — already Fibonacci. */
        s_lightning_alpha = (float)(FIB_144 + FIB_34) + rndf(0.0f, (float)FIB_55);
        /* Lightning re-trigger cooldown = FIB_8 frames (~104 ms at 77 Hz). */
        s_lightning_cooldown = FIB_8;
        s_lightning_x[0] = (int16_t)(EVA_WEATHER_RENDER_W * rndf(0.38f, 0.68f));
        s_lightning_y[0] = 0;
        for (int i = 1; i < 6; ++i) {
            s_lightning_x[i] = (int16_t)(s_lightning_x[i - 1] + (int)rndf(-42.0f, 42.0f));
            s_lightning_y[i] = (int16_t)(s_lightning_y[i - 1] + (int)rndf(28.0f, 68.0f));
        }
    }
    if (s_lightning_alpha >= 2.0f) {
        s_lightning_alpha *= EVA_INV_PHI * EVA_INV_PHI;
    }
}

static void composite_lightning_on_render(void)
{
    if (s_lightning_alpha < 2.0f) return;
    uint8_t alpha = clamp_u8((int)s_lightning_alpha);
    uint16_t white = rgb565(255, 255, 255);
    for (int y = 0; y < EVA_WEATHER_RENDER_H; ++y) {
        uint16_t *row = &s_buf[y * EVA_WEATHER_RENDER_W];
        for (int x = y & 1; x < EVA_WEATHER_RENDER_W; x += 2) {
            row[x] = blend565(row[x], white, alpha);
        }
    }
    uint16_t bolt = rgb565(238, 246, 255);
    /* Bolt is FIB_5 segments (5 = original 6 segments minus the start vertex
     * that connects to the previous). +FIB_34 extra brightness over the
     * full-screen flash so the bolt itself is always brighter than the
     * background flash, capped at uint8 max via clamp_u8. */
    for (int i = 1; i < FIB_5 + 1; ++i) {
        draw_line(s_lightning_x[i - 1], s_lightning_y[i - 1],
                  s_lightning_x[i], s_lightning_y[i],
                  bolt, clamp_u8(alpha + FIB_34), 1);
    }
}

static uint8_t background_hold_frames(weather_kind_t kind)
{
    /* The cached background contains sky + sun/moon + fog only. Clouds are
     * blended fresh every frame on top of this cached base.
     *
     * Original tuned values from the working backup. Aggressive short
     * holds (FIB_21=270ms) caused render task hangs — likely because
     * the 1.5 MB/frame bg→s_buf memcpy under PSRAM bandwidth contention
     * with the cloud PPA blends pushed past sustained throughput. */
    switch (kind) {
    case WEATHER_THUNDERSTORM:
        return FIB_8;    /* ~104 ms */
    case WEATHER_FOG:
        return FIB_5;    /* ~65 ms */
    case WEATHER_RAIN:
    case WEATHER_HEAVY_RAIN:
    case WEATHER_SNOW:
    case WEATHER_SLEET:
    case WEATHER_HAIL:
        return FIB_13;   /* ~170 ms */
    default:
        return FIB_34;   /* ~440 ms */
    }
}

static uint8_t composite_hold_frames(weather_kind_t kind)
{
    /* Reuse bg+text+clouds for short bursts. Expensive cloud PPA passes are
     * amortized; particles, lightning, and godrays are still redrawn every
     * frame. Longer holds are needed now that full-frame PPA rotation costs a
     * fixed ~15 ms on this panel path. */
    switch (kind) {
    case WEATHER_THUNDERSTORM:
    case WEATHER_HEAVY_RAIN:
        return 8;
    case WEATHER_RAIN:
    case WEATHER_SLEET:
    case WEATHER_HAIL:
    case WEATHER_SNOW:
        return 13;
    case WEATHER_FOG:
        return 13;
    default:
        return 21;
    }
}

static void adapt_budget(int64_t frame_us)
{
    if (frame_us > 25000) {
        s_over_budget++;
        s_under_budget = 0;
        if (s_over_budget >= 3 && s_target > 64) {
            s_target = (uint16_t)((s_target * 9U) / 10U);
            if (s_target < 64) s_target = 64;
            s_over_budget = 0;
        }
        if (frame_us > 30000 && s_clouds3d_active > 10) {
            s_clouds3d_active--;
        }
    } else if (frame_us < 14000) {
        s_under_budget++;
        s_over_budget = 0;
        if (s_under_budget >= 30 && s_target < s_max_target) {
            s_target = (uint16_t)((s_target * 105U) / 100U + 1U);
            if (s_target > s_max_target) s_target = s_max_target;
            s_under_budget = 0;
        }
        if (s_under_budget >= 15 && s_clouds3d_active < CLOUD_3D_MAX) {
            s_clouds3d_active++;
        }
    } else {
        s_over_budget = 0;
        s_under_budget = 0;
    }
}

static uint32_t s_last_tick_hz;
static uint32_t s_last_work_us;
static uint32_t s_last_bg_us;
static uint32_t s_last_cloud_us;
static uint32_t s_last_particle_us;
static uint32_t s_last_lightning_us;
static uint32_t s_last_lvgl_us;
static uint32_t s_last_vsync_us;

uint32_t eva_weather_canvas_last_tick_hz(void) { return s_last_tick_hz; }
uint32_t eva_weather_canvas_last_work_us(void) { return s_last_work_us; }
void eva_weather_canvas_last_breakdown_us(uint32_t *bg_us, uint32_t *cloud_us,
                                          uint32_t *particle_us, uint32_t *lightning_us,
                                          uint32_t *lvgl_us, uint32_t *vsync_us)
{
    if (bg_us) *bg_us = s_last_bg_us;
    if (cloud_us) *cloud_us = s_last_cloud_us;
    if (particle_us) *particle_us = s_last_particle_us;
    if (lightning_us) *lightning_us = s_last_lightning_us;
    if (lvgl_us) *lvgl_us = s_last_lvgl_us;
    if (vsync_us) *vsync_us = s_last_vsync_us;
}

void eva_weather_canvas_cloud_budget(uint16_t *active, uint16_t *max)
{
    if (active) *active = s_clouds3d_active;
    if (max) *max = CLOUD_3D_MAX;
}

/* Cloud composition cache. Cloud blends are the most expensive single phase
 * (~12 ms on storm, ~10 ms otherwise) because each variant runs three PPA
 * passes (shadow + core + light) per layer × 3 layers × ≤2 bands. We cache
 * the result of the sky + sun + clouds + fog stack in s_bg_buf and reuse it
 * across `background_hold_frames(kind)` ticks. Particles and lightning are
 * drawn fresh on top every frame because those move every tick. */
static void render_weather(float dt)
{
    if (s_kind != s_prev_kind) {
        reset_particles_for_kind();
        s_bg_ttl = 0;
        s_bg_dt = 0.0f;
        s_composite_ttl = 0;
        s_composite_dt = 0.0f;
        s_prev_kind = s_kind;
    }

    int64_t now_us = esp_timer_get_time();
    float t = (float)now_us / 1000000.0f;
    s_bg_dt += dt;
    s_composite_dt += dt;

    int64_t tb0 = esp_timer_get_time();
    bool composite_hit = (s_composite_buf && s_composite_ttl > 0);

    if (composite_hit) {
        memcpy(s_buf, s_composite_buf,
               EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t));
        s_composite_ttl--;
        if (s_bg_ttl > 0) s_bg_ttl--;
    } else {
        if (!s_bg_buf || s_bg_ttl == 0) {
            sky_t sky = sky_for_kind(s_kind);
            fill_gradient(sky.top, sky.bottom);
            draw_day_sky_depth();
            draw_sun_or_moon(t);
            draw_sun_sky_glare();
            if (s_fog_pct >= 30) {
                draw_fog_bands(t);
            }
            if (s_bg_buf) {
                memcpy(s_bg_buf, s_buf,
                       EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t));
            }
            s_bg_dt = 0.0f;
            s_bg_ttl = background_hold_frames(s_kind);
        } else {
            memcpy(s_buf, s_bg_buf,
                   EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t));
            s_bg_ttl--;
        }
        int64_t tb_after_bg = esp_timer_get_time();
        s_prof_bg_us += (tb_after_bg - tb0);

        draw_scene_text_overlays();
        compose_clouds_into_working_buffer(dt);
        draw_sun_cloud_lighting();
        draw_sun_god_rays(t);

        int64_t tb_after_clouds = esp_timer_get_time();
        s_prof_clouds_us += (tb_after_clouds - tb_after_bg);

        if (s_composite_buf) {
            memcpy(s_composite_buf, s_buf,
                   EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t));
        }
        s_composite_dt = 0.0f;
        s_composite_ttl = composite_hold_frames(s_kind) - 1;
    }

    int64_t tb_after_static = esp_timer_get_time();
    if (composite_hit) {
        s_prof_bg_us += (tb_after_static - tb0);
    }

    update_and_draw_particles(dt, t);
    int64_t tb3 = esp_timer_get_time();
    s_prof_particles_us += (tb3 - tb_after_static);

    update_lightning(t);
    composite_lightning_on_render();
    int64_t tb4 = esp_timer_get_time();
    s_prof_lightning_us += (tb4 - tb3);
}

/* upscale removed: render is native 800×480 directly into s_display_buf. */

static bool IRAM_ATTR on_ppa_trans_done(ppa_client_handle_t client,
                                        ppa_event_data_t *event_data,
                                        void *user_data)
{
    (void)client;
    (void)event_data;
    (void)user_data;
    BaseType_t hp_wake = pdFALSE;
    if (s_ppa_done_sem) {
        xSemaphoreGiveFromISR(s_ppa_done_sem, &hp_wake);
    }
    return hp_wake == pdTRUE;
}

static bool IRAM_ATTR on_dpi_refresh_done(esp_lcd_panel_handle_t panel,
                                          esp_lcd_dpi_panel_event_data_t *edata,
                                          void *user_ctx)
{
    (void)panel;
    (void)edata;
    (void)user_ctx;
    BaseType_t hp_wake = pdFALSE;
    if (s_vsync_sem) {
        xSemaphoreGiveFromISR(s_vsync_sem, &hp_wake);
    }
    return hp_wake == pdTRUE;
}

static esp_err_t rotate_render_to_dpi_fb(uint16_t *dst)
{
    if (!s_ppa_srm || s_ppa_disabled || !dst) {
        return ESP_ERR_INVALID_STATE;
    }

    ppa_srm_oper_config_t cfg = {
        .in = {
            .buffer = s_render_buf,
            .pic_w = EVA_WEATHER_RENDER_W,
            .pic_h = EVA_WEATHER_RENDER_H,
            .block_w = EVA_WEATHER_RENDER_W,
            .block_h = EVA_WEATHER_RENDER_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = 480 * 800 * sizeof(uint16_t),
            .pic_w = 480,
            .pic_h = 800,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        /* LVGL previously used DISPLAY_ROTATION_270. PPA angles are CCW, so
         * keep the same physical orientation by rotating the landscape scene
         * 270 degrees into the portrait DPI framebuffer. */
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mode = PPA_TRANS_MODE_NON_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_ppa_srm, &cfg);
}

static void native_render_task(void *arg)
{
    (void)arg;
    int64_t prof_rotate_us = 0;

    while (true) {
        if (!s_visible || !s_render_buf || !s_panel || !s_dpi_back_fb) {
            s_last_us = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(TIMER_MS));
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t tick_us = s_last_us ? now - s_last_us : (int64_t)TIMER_MS * 1000;
        float dt = (float)tick_us / 1000000.0f;
        if (dt < 0.0f || dt > 0.10f) dt = (float)TIMER_MS / 1000.0f;
        s_last_us = now;
        s_buf = s_render_buf;

        int64_t t0 = esp_timer_get_time();
        render_weather(dt);
        int64_t t_render = esp_timer_get_time();

        esp_err_t err = rotate_render_to_dpi_fb(s_dpi_back_fb);
        if (err == ESP_OK) {
            if (xSemaphoreTake(s_ppa_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "PPA rotate timeout");
                continue;
            }
        } else {
            ESP_LOGE(TAG, "PPA rotate failed: 0x%x", (unsigned)err);
            vTaskDelay(pdMS_TO_TICKS(TIMER_MS));
            continue;
        }
        int64_t t_rotate = esp_timer_get_time();

        err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, 480, 800, s_dpi_back_fb);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "DPI fb swap failed: 0x%x", (unsigned)err);
            vTaskDelay(pdMS_TO_TICKS(TIMER_MS));
            continue;
        }
        int64_t t_draw = esp_timer_get_time();
        (void)xSemaphoreTake(s_vsync_sem, pdMS_TO_TICKS(40));
        int64_t t_vsync = esp_timer_get_time();

        uint16_t *old_scan = s_dpi_scan_fb;
        s_dpi_scan_fb = s_dpi_back_fb;
        s_dpi_back_fb = old_scan;

        s_last_frame_us = t_rotate - t0;
        adapt_budget(s_last_frame_us);
        prof_rotate_us += (t_rotate - t_render);
        s_accum_lvgl_slot_us += (t_vsync - t_draw);

        s_frames++;
        s_accum_us += s_last_frame_us;
        s_accum_tick_us += tick_us;
        if (s_frames >= LOG_EVERY_FRAMES) {
            uint32_t avg = (uint32_t)(s_accum_us / s_frames);
            uint32_t tick_avg = (uint32_t)(s_accum_tick_us / s_frames);
            uint32_t tick_hz = tick_avg ? (uint32_t)(1000000ULL / tick_avg) : 0;
            uint32_t bg_avg     = (uint32_t)(s_prof_bg_us        / s_frames);
            uint32_t cl_avg     = (uint32_t)(s_prof_clouds_us    / s_frames);
            uint32_t pa_avg     = (uint32_t)(s_prof_particles_us / s_frames);
            uint32_t li_avg     = (uint32_t)(s_prof_lightning_us / s_frames);
            uint32_t rot_avg    = (uint32_t)(prof_rotate_us      / s_frames);
            uint32_t vsync_avg  = (uint32_t)(s_accum_lvgl_slot_us / s_frames);
            ESP_LOGI(TAG, "%s tick=%u Hz, %u/%u particles c3d=%u/%u work_us=%u (bg=%u cl=%u pa=%u li=%u ppa_rot=%u lvgl=0 vsync=%u)",
                     weather_kind_name(s_kind), (unsigned)tick_hz,
                     (unsigned)s_target, (unsigned)s_max_target,
                     (unsigned)s_clouds3d_active, (unsigned)CLOUD_3D_MAX,
                     (unsigned)avg,
                     bg_avg, cl_avg, pa_avg, li_avg, rot_avg, vsync_avg);
            s_last_tick_hz = tick_hz;
            s_last_work_us = avg;
            s_last_bg_us = bg_avg;
            s_last_cloud_us = cl_avg;
            s_last_particle_us = pa_avg;
            s_last_lightning_us = li_avg;
            s_last_lvgl_us = 0;
            s_last_vsync_us = vsync_avg;
            s_frames = 0;
            s_accum_us = 0;
            s_accum_tick_us = 0;
            s_accum_lvgl_slot_us = 0;
            s_prof_bg_us = 0;
            s_prof_clouds_us = 0;
            s_prof_particles_us = 0;
            s_prof_lightning_us = 0;
            prof_rotate_us = 0;
        }

        s_last_tick_exit_us = esp_timer_get_time();
    }
}

static void canvas_tick(lv_timer_t *timer)
{
    (void)timer;
    static int64_t prof_upscale_us = 0; /* Kept for log format compatibility. */

    if (!s_visible || !s_canvas || !s_render_buf) {
        s_last_us = esp_timer_get_time();
        return;
    }

    int64_t now = esp_timer_get_time();
    int64_t lvgl_slot_us = s_last_tick_exit_us ? (now - s_last_tick_exit_us)
                                                : ((int64_t)TIMER_MS * 1000);
    s_accum_lvgl_slot_us += lvgl_slot_us;
    int64_t tick_us = s_last_us ? now - s_last_us : (int64_t)TIMER_MS * 1000;
    float dt = (float)tick_us / 1000000.0f;
    if (dt < 0.0f || dt > 0.10f) dt = (float)TIMER_MS / 1000.0f;
    s_last_us = now;
    s_buf = s_render_buf;

    int64_t t0 = esp_timer_get_time();
    render_weather(dt);
    int64_t t_render = esp_timer_get_time();
    s_last_frame_us = t_render - t0;
    adapt_budget(s_last_frame_us);

    lv_obj_invalidate(s_canvas);

    s_frames++;
    s_accum_us += s_last_frame_us;
    s_accum_tick_us += tick_us;
    if (s_frames >= LOG_EVERY_FRAMES) {
        uint32_t avg = (uint32_t)(s_accum_us / s_frames);
        uint32_t tick_avg = (uint32_t)(s_accum_tick_us / s_frames);
        uint32_t tick_hz = tick_avg ? (uint32_t)(1000000ULL / tick_avg) : 0;
        uint32_t bg_avg     = (uint32_t)(s_prof_bg_us       / s_frames);
        uint32_t cl_avg     = (uint32_t)(s_prof_clouds_us   / s_frames);
        uint32_t pa_avg     = (uint32_t)(s_prof_particles_us/ s_frames);
        uint32_t li_avg     = (uint32_t)(s_prof_lightning_us/ s_frames);
        uint32_t lvgl_avg   = (uint32_t)(s_accum_lvgl_slot_us / s_frames);
        uint32_t vsync_avg  = 0;
        uint32_t up_avg     = (uint32_t)(prof_upscale_us    / s_frames);
        ESP_LOGI(TAG, "%s tick=%u Hz, %u/%u particles c3d=%u/%u work_us=%u (bg=%u cl=%u pa=%u li=%u up=%u lvgl=%u vsync=%u)",
                 weather_kind_name(s_kind), (unsigned)tick_hz,
                 (unsigned)s_target, (unsigned)s_max_target,
                 (unsigned)s_clouds3d_active, (unsigned)CLOUD_3D_MAX,
                 (unsigned)avg,
                 bg_avg, cl_avg, pa_avg, li_avg, up_avg, lvgl_avg, vsync_avg);
        s_last_tick_hz = tick_hz;
        s_last_work_us = avg;
        s_last_bg_us = bg_avg;
        s_last_cloud_us = cl_avg;
        s_last_particle_us = pa_avg;
        s_last_lightning_us = li_avg;
        s_last_lvgl_us = lvgl_avg;
        s_last_vsync_us = vsync_avg;
        s_frames = 0;
        s_accum_us = 0;
        s_accum_tick_us = 0;
        s_accum_lvgl_slot_us = 0;
        s_prof_bg_us = 0;
        s_prof_clouds_us = 0;
        s_prof_particles_us = 0;
        s_prof_lightning_us = 0;
        prof_upscale_us = 0;
    }

    s_last_tick_exit_us = esp_timer_get_time();
}

lv_obj_t *eva_weather_canvas_init(lv_obj_t *parent)
{
    if (s_canvas) return s_canvas;

    s_rng ^= (uint32_t)esp_timer_get_time();
    /* 128-byte alignment required by PPA on ESP32-P4 (L2 cache line = 128 B).
     * heap_caps_aligned_alloc(64,...) is not enough — half the time the address
     * lands at 64-mod-128 and PPA rejects it as "not aligned to cache line size". */
    s_render_buf = heap_caps_aligned_alloc(128,
                                           EVA_WEATHER_CANVAS_W * EVA_WEATHER_CANVAS_H * sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_render_buf) {
        ESP_LOGE(TAG, "render buffer alloc failed");
        abort();
    }
    s_display_buf = s_render_buf;
    s_buf = s_render_buf;
    s_bg_buf = heap_caps_aligned_alloc(128,
                                       EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg_buf) {
        ESP_LOGE(TAG, "background buffer alloc failed");
        abort();
    }
    s_composite_buf = heap_caps_aligned_alloc(128,
                                              EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_composite_buf) {
        ESP_LOGE(TAG, "composite buffer alloc failed");
        abort();
    }
    /* Text-overlay A8 caches (clock/temp/desc). Lifetime = process. */
    s_clock_slot.a8 = heap_caps_malloc(TEXT_SLOT_BUF_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_temp_slot.a8  = heap_caps_malloc(TEXT_SLOT_BUF_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_desc_slot.a8  = heap_caps_malloc(TEXT_SLOT_BUF_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_clock_slot.a8 || !s_temp_slot.a8 || !s_desc_slot.a8) {
        ESP_LOGE(TAG, "text slot alloc failed");
        abort();
    }
    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t ppa_err = ppa_register_client(&ppa_cfg, &s_ppa_srm);
    if (ppa_err != ESP_OK) {
        ESP_LOGW(TAG, "PPA SRM unavailable (err=%d); CPU upscale fallback", (int)ppa_err);
        s_ppa_srm = NULL;
        s_ppa_disabled = true;
    }
    ppa_client_config_t ppa_blend_cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        /* Enough slots for all cloud passes (3 layers × up to 2 bands × up
         * to 2 variants during morph crossfade = 12) plus headroom. */
        .max_pending_trans_num = 16,
    };
    esp_err_t ppa_blend_err = ppa_register_client(&ppa_blend_cfg, &s_ppa_blend);
    if (ppa_blend_err != ESP_OK) {
        ESP_LOGW(TAG, "PPA blend unavailable (err=%d); CPU cloud fallback", (int)ppa_blend_err);
        s_ppa_blend = NULL;
        s_ppa_blend_disabled = true;
    }
    /* Initialise the static glyph draw buffer so draw_text_utf8 can call
     * lv_font_get_glyph_bitmap, which would dereference NULL inside
     * lv_font_get_bitmap_fmt_txt (it does bitmap_out = draw_buf->data with
     * no NULL check). */
    lv_result_t db_init_res = lv_draw_buf_init(&s_glyph_draw_buf,
                                               GLYPH_BUF_W, GLYPH_BUF_H,
                                               LV_COLOR_FORMAT_A8,
                                               GLYPH_BUF_W,
                                               s_glyph_raw, GLYPH_BUF_BYTES);
    if (db_init_res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "glyph draw_buf init failed res=%d (w=%d h=%d size=%d)",
                 (int)db_init_res, GLYPH_BUF_W, GLYPH_BUF_H, GLYPH_BUF_BYTES);
        abort();
    }
    ESP_LOGI(TAG, "glyph draw_buf ready: %dx%d A8, data=%p size=%d",
             GLYPH_BUF_W, GLYPH_BUF_H, (void*)s_glyph_draw_buf.data, GLYPH_BUF_BYTES);

    init_cloud_strips();

    s_canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(s_canvas, s_display_buf, EVA_WEATHER_CANVAS_W, EVA_WEATHER_CANVAS_H,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(s_canvas, EVA_WEATHER_CANVAS_W, EVA_WEATHER_CANVAS_H);
    lv_obj_align(s_canvas, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);

    reset_particles_for_kind();
    s_timer = lv_timer_create(canvas_tick, TIMER_MS, NULL);
    if (!s_timer) {
        ESP_LOGE(TAG, "canvas timer create failed");
        abort();
    }
    lv_timer_pause(s_timer);
    ESP_LOGI(TAG, "allocated %u KB render + %u KB background buffers; %s upscale, %s cloud blend %dx%d -> %dx%d",
             (unsigned)(EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t) / 1024),
             (unsigned)(EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t) / 1024),
             s_ppa_srm && !s_ppa_disabled ? "PPA" : "CPU",
             s_ppa_blend && !s_ppa_blend_disabled ? "PPA" : "CPU",
             EVA_WEATHER_RENDER_W, EVA_WEATHER_RENDER_H,
             EVA_WEATHER_CANVAS_W, EVA_WEATHER_CANVAS_H);
    return s_canvas;
}

void eva_weather_canvas_init_native(esp_lcd_panel_handle_t panel)
{
    if (s_render_task) return;
    s_panel = panel;
    ESP_ERROR_CHECK(s_panel ? ESP_OK : ESP_ERR_INVALID_ARG);

    s_rng ^= (uint32_t)esp_timer_get_time();
    s_render_buf = heap_caps_aligned_alloc(128,
                                           EVA_WEATHER_CANVAS_W * EVA_WEATHER_CANVAS_H * sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_render_buf) {
        ESP_LOGE(TAG, "render buffer alloc failed");
        abort();
    }
    s_display_buf = s_render_buf;
    s_buf = s_render_buf;
    s_bg_buf = heap_caps_aligned_alloc(128,
                                       EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg_buf) {
        ESP_LOGE(TAG, "background buffer alloc failed");
        abort();
    }
    s_composite_buf = heap_caps_aligned_alloc(128,
                                              EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_composite_buf) {
        ESP_LOGE(TAG, "composite buffer alloc failed");
        abort();
    }
    s_clock_slot.a8 = heap_caps_malloc(TEXT_SLOT_BUF_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_temp_slot.a8  = heap_caps_malloc(TEXT_SLOT_BUF_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_desc_slot.a8  = heap_caps_malloc(TEXT_SLOT_BUF_BYTES,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_clock_slot.a8 || !s_temp_slot.a8 || !s_desc_slot.a8) {
        ESP_LOGE(TAG, "text slot alloc failed");
        abort();
    }

    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
    };
    esp_err_t ppa_err = ppa_register_client(&ppa_cfg, &s_ppa_srm);
    if (ppa_err != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM required for native display path (err=%d)", (int)ppa_err);
        abort();
    }
    ppa_event_callbacks_t ppa_cbs = {
        .on_trans_done = on_ppa_trans_done,
    };
    ESP_ERROR_CHECK(ppa_client_register_event_callbacks(s_ppa_srm, &ppa_cbs));

    ppa_client_config_t ppa_blend_cfg = {
        .oper_type = PPA_OPERATION_BLEND,
        .max_pending_trans_num = 16,
    };
    esp_err_t ppa_blend_err = ppa_register_client(&ppa_blend_cfg, &s_ppa_blend);
    if (ppa_blend_err != ESP_OK) {
        ESP_LOGW(TAG, "PPA blend unavailable (err=%d); CPU cloud fallback", (int)ppa_blend_err);
        s_ppa_blend = NULL;
        s_ppa_blend_disabled = true;
    }

    lv_result_t db_init_res = lv_draw_buf_init(&s_glyph_draw_buf,
                                               GLYPH_BUF_W, GLYPH_BUF_H,
                                               LV_COLOR_FORMAT_A8,
                                               GLYPH_BUF_W,
                                               s_glyph_raw, GLYPH_BUF_BYTES);
    if (db_init_res != LV_RESULT_OK) {
        ESP_LOGE(TAG, "glyph draw_buf init failed res=%d", (int)db_init_res);
        abort();
    }
    init_cloud_strips();
    reset_particles_for_kind();

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2,
                                                       (void **)&s_dpi_fb[0],
                                                       (void **)&s_dpi_fb[1]));
    s_dpi_scan_fb = s_dpi_fb[0];
    s_dpi_back_fb = s_dpi_fb[1];
    ESP_LOGI(TAG, "DPI frame buffers: fb0=%p fb1=%p", (void *)s_dpi_fb[0], (void *)s_dpi_fb[1]);

    s_ppa_done_sem = xSemaphoreCreateBinary();
    s_vsync_sem = xSemaphoreCreateBinary();
    if (!s_ppa_done_sem || !s_vsync_sem) {
        ESP_LOGE(TAG, "native sem alloc failed");
        abort();
    }
    esp_lcd_dpi_panel_event_callbacks_t dpi_cbs = {
        .on_refresh_done = on_dpi_refresh_done,
    };
    ESP_ERROR_CHECK(esp_lcd_dpi_panel_register_event_callbacks(s_panel, &dpi_cbs, NULL));

    s_visible = true;
    if (xTaskCreatePinnedToCore(native_render_task, "eva_native_render", 8192,
                                NULL, 5, &s_render_task, 1) != pdTRUE) {
        ESP_LOGE(TAG, "native render task create failed");
        abort();
    }
    ESP_LOGI(TAG, "native render path ready: %ux%u landscape -> PPA rotate -> 480x800 DPI fb",
             (unsigned)EVA_WEATHER_RENDER_W, (unsigned)EVA_WEATHER_RENDER_H);
}

/* Default cloud coverage and fog for each weather kind. Used by
 * eva_weather_canvas_set_kind() (CDC debug path) so switching kinds clears
 * any leftover percentages from a previous live state.
 *
 * Layer values are {low, mid, high, fog}. Total cloud isn't relevant for the
 * canvas — only the per-layer percentages drive rendering. */
static void default_cloud_pct_for_kind(weather_kind_t kind,
                                       uint8_t out[4])
{
    switch (kind) {
    case WEATHER_CLEAR_DAY:
    case WEATHER_CLEAR_NIGHT:
        out[0] = 0;  out[1] = 0;  out[2] = 0;  out[3] = 0;  break;
    case WEATHER_PARTLY_CLOUDY_DAY:
    case WEATHER_PARTLY_CLOUDY_NIGHT:
        out[0] = 55; out[1] = 35; out[2] = 8;  out[3] = 0;  break;
    case WEATHER_CLOUDY:
        out[0] = 80; out[1] = 80; out[2] = 60; out[3] = 0;  break;
    case WEATHER_FOG:
        out[0] = 50; out[1] = 40; out[2] = 20; out[3] = 80; break;
    case WEATHER_RAIN:
    case WEATHER_HEAVY_RAIN:
    case WEATHER_THUNDERSTORM:
    case WEATHER_SLEET:
        out[0] = 85; out[1] = 75; out[2] = 40; out[3] = 0;  break;
    case WEATHER_SNOW:
    case WEATHER_HAIL:
        out[0] = 75; out[1] = 70; out[2] = 40; out[3] = 0;  break;
    default:
        out[0] = 50; out[1] = 50; out[2] = 50; out[3] = 0;  break;
    }
}

void eva_weather_canvas_set_kind(weather_kind_t kind)
{
    if (kind <= WEATHER_UNKNOWN || kind >= WEATHER_KIND_COUNT) {
        kind = WEATHER_CLOUDY;
    }
    if (s_kind != kind || fabsf(s_density_scale - 1.0f) > 0.01f) {
        s_kind = kind;
        s_density_scale = 1.0f;
        /* Reset cloud coverage to the kind's defaults — without this, a
         * weatherdebug switch from "cloudy" to "clear-day" would leave
         * leftover cloud_pct values from the previous live fetch and the
         * "clear" sky would still have visible cloud patches. */
        uint8_t defaults[4];
        default_cloud_pct_for_kind(kind, defaults);
        s_cloud_pct[CLOUD_LAYER_LOW]  = defaults[0];
        s_cloud_pct[CLOUD_LAYER_MID]  = defaults[1];
        s_cloud_pct[CLOUD_LAYER_HIGH] = defaults[2];
        s_fog_pct = defaults[3];
        s_prev_kind = WEATHER_UNKNOWN;
        s_clouds3d_inited = false;
        s_clouds3d_active = CLOUD_3D_MAX;
        s_frames = 0;
        s_accum_us = 0;
        s_accum_tick_us = 0;
        s_over_budget = 0;
        s_under_budget = 0;
        s_bg_ttl = 0;
        s_bg_dt = 0.0f;
        s_composite_ttl = 0;
        s_composite_dt = 0.0f;
    }
}

void eva_weather_canvas_set_weather(const weather_state_t *st)
{
    if (!st) return;
    weather_kind_t kind = st->kind;
    if (kind <= WEATHER_UNKNOWN || kind >= WEATHER_KIND_COUNT) {
        kind = WEATHER_CLOUDY;
    }
    float density = density_scale_from_weather(st);
    s_sunrise_min  = st->sunrise_min;
    s_sunset_min   = st->sunset_min;
    s_moonrise_min = st->moonrise_min;
    s_moonset_min  = st->moonset_min;
    /* Apply test overrides first (sliders). Live values used only if
     * the corresponding override is -1. */
    s_cloud_pct[CLOUD_LAYER_HIGH] = (s_test_cloud_pct_override[CLOUD_LAYER_HIGH] >= 0)
        ? (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_HIGH] : st->cloud_high_pct;
    s_cloud_pct[CLOUD_LAYER_MID]  = (s_test_cloud_pct_override[CLOUD_LAYER_MID] >= 0)
        ? (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_MID]  : st->cloud_mid_pct;
    s_cloud_pct[CLOUD_LAYER_LOW]  = (s_test_cloud_pct_override[CLOUD_LAYER_LOW] >= 0)
        ? (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_LOW]  : st->cloud_low_pct;
    s_cloud_cover_pct = st->cloud_cover_pct;
    s_fog_pct = st->fog_pct;
    s_moon_phase_pct = st->moon_phase_pct;
    s_moon_waning = st->moon_waning;

    /* Adapt Phase 3 cloud count to cover percentage.
     * Minimum 4 clouds even on clear day; maximum CLOUD_3D_MAX on overcast. */
    int desired_active = (int)(((long)s_cloud_cover_pct * CLOUD_3D_MAX + 50) / 100);
    if (desired_active < 4) desired_active = 4;
    if (desired_active > CLOUD_3D_MAX) desired_active = CLOUD_3D_MAX;
    s_clouds3d_active = (uint8_t)desired_active;

    /* Wind bias for rain/snow particle drift. Meteorological direction is
     * the angle the wind is COMING FROM, with 0° = North, 90° = East,
     * 180° = South, 270° = West. Horizontal screen velocity is the sin of
     * that angle, NEGATED (wind from West → blows toward East → particles
     * drift right → positive vx).
     *
     * k_rain = 1.6 px/s per kph: a 30 kph crosswind shifts rain ~48 px/s
     * sideways on top of its base ~70 px/s natural drift. Tuned by eye on
     * the 400x240 working buffer. */
    float new_vx_bias = 0.0f;
    float new_kph = 0.0f;
    if (st->wind_kph >= 0 && st->wind_dir_deg >= 0) {
        float rad = (float)st->wind_dir_deg * (3.1415926f / 180.0f);
        float dir_x = -sinf(rad);    /* "from" -> screen X sign */
        new_vx_bias = dir_x * (float)st->wind_kph * 1.6f;
        new_kph = (float)st->wind_kph;
    }
    /* Let existing particles age out gradually on live wind changes. */
    if (s_test_wind_kph_override < 0) {
        s_wind_vx_bias = new_vx_bias;
        s_wind_kph_eff = new_kph;
    }

    bool kind_changed = (s_kind != kind);
    s_kind = kind;
    s_density_scale = density;

    if (kind_changed) {
        s_prev_kind = WEATHER_UNKNOWN;
        s_clouds3d_inited = false;
        s_frames = 0;
        s_accum_us = 0;
        s_accum_tick_us = 0;
        s_over_budget = 0;
        s_under_budget = 0;
        s_bg_ttl = 0;
        s_bg_dt = 0.0f;
        s_composite_ttl = 0;
        s_composite_dt = 0.0f;
        /* Note: s_clouds3d_active is already set above by cloud_cover_pct adaptation;
         * don't reset it here to preserve the adapted count. */
    }
}

void eva_weather_canvas_show(bool show)
{
    s_visible = show;
    if (!s_canvas) return;
    if (show) {
        lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
        s_last_us = esp_timer_get_time();
        if (s_timer) {
            lv_timer_resume(s_timer);
        }
        lv_obj_invalidate(s_canvas);
    } else {
        if (s_timer) {
            lv_timer_pause(s_timer);
        }
        lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

void eva_weather_canvas_set_time_offset(int hours)
{
    s_time_offset_hours = hours;
}

void eva_weather_canvas_set_clock_text(const char *text)
{
    if (!text) return;
    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_clock_text, text, sizeof(s_clock_text));
    portEXIT_CRITICAL(&s_text_mux);
}

void eva_weather_canvas_set_temp_text(const char *text)
{
    if (!text) return;
    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_temp_text, text, sizeof(s_temp_text));
    portEXIT_CRITICAL(&s_text_mux);
}

void eva_weather_canvas_set_desc_text(const char *text)
{
    if (!text) return;
    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_desc_text, text, sizeof(s_desc_text));
    portEXIT_CRITICAL(&s_text_mux);
}

const uint16_t *eva_weather_canvas_display_buf(void)
{
    return s_display_buf;
}

/* --- Test mode overrides ---------------------------------------------------
 * These let the test-mode UI (sliders in main.c) directly drive cloud
 * coverage and wind without going through the weather provider. Setting
 * a non-negative value pins that channel; setting -1 releases it. */
void eva_weather_canvas_set_test_cloud_pct(int high, int mid, int low)
{
    portENTER_CRITICAL(&s_frame_mux);
    s_test_cloud_pct_override[CLOUD_LAYER_HIGH] = (int16_t)((high < 0) ? -1 : (high > 100 ? 100 : high));
    s_test_cloud_pct_override[CLOUD_LAYER_MID]  = (int16_t)((mid  < 0) ? -1 : (mid  > 100 ? 100 : mid));
    s_test_cloud_pct_override[CLOUD_LAYER_LOW]  = (int16_t)((low  < 0) ? -1 : (low  > 100 ? 100 : low));
    if (s_test_cloud_pct_override[CLOUD_LAYER_HIGH] >= 0)
        s_cloud_pct[CLOUD_LAYER_HIGH] = (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_HIGH];
    if (s_test_cloud_pct_override[CLOUD_LAYER_MID] >= 0)
        s_cloud_pct[CLOUD_LAYER_MID]  = (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_MID];
    if (s_test_cloud_pct_override[CLOUD_LAYER_LOW] >= 0)
        s_cloud_pct[CLOUD_LAYER_LOW]  = (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_LOW];
    /* Force tint refresh on next compose (re-derives alpha_scale from pct). */
    s_bg_ttl = 0;
    s_composite_ttl = 0;
    portEXIT_CRITICAL(&s_frame_mux);
}

void eva_weather_canvas_set_test_wind_kph(int wind_kph)
{
    portENTER_CRITICAL(&s_frame_mux);
    if (wind_kph < 0) {
        s_test_wind_kph_override = -1;
    } else {
        if (wind_kph > 120) wind_kph = 120;
        s_test_wind_kph_override = (int16_t)wind_kph;
        /* Direct east-bound wind for slider control. Positive = right drift.
         * k_rain factor 1.6 matches the live-weather path. */
        s_wind_vx_bias = (float)wind_kph * 1.6f;
        s_wind_kph_eff = (float)wind_kph;
    }
    portEXIT_CRITICAL(&s_frame_mux);
}

void eva_weather_canvas_clear_test_overrides(void)
{
    portENTER_CRITICAL(&s_frame_mux);
    s_test_cloud_pct_override[0] = -1;
    s_test_cloud_pct_override[1] = -1;
    s_test_cloud_pct_override[2] = -1;
    s_test_wind_kph_override = -1;
    portEXIT_CRITICAL(&s_frame_mux);
}
