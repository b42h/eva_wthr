#include "eva_weather_canvas.h"
#include "eva_cloud_assets.h"
#include "eva_clp_toc.h"
#include "eva_wx_transition.h"
#include "eva_dither.h"
#include "eva_sky_palette.h"
#include "eva_text_spans.h"

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

#ifdef EVA_PORTRAIT_NATIVE
#include "eva_orient.h"
#endif

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
#define PPA_CACHE_ALIGN 128   /* ESP32-P4 L2 cache line; required for PPA DMA */
#define EVA_FRAME_BYTES (EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t))

#ifdef EVA_PORTRAIT_NATIVE
#define EVA_FB_PIC_W EVA_PORT_W
#define EVA_FB_PIC_H EVA_PORT_H
static inline void eva_land_rect_to_port(int lx, int ly, int lw, int lh,
                                         int *ox, int *oy, int *ow, int *oh)
{
    *ox = ly;
    *oy = EVA_LAND_W - lx - lw;
    *ow = lh;
    *oh = lw;
}
#else
#define EVA_FB_PIC_W EVA_WEATHER_RENDER_W
#define EVA_FB_PIC_H EVA_WEATHER_RENDER_H
#endif
#define EVA_PHI 1.6180339f
#define EVA_PHI2 (EVA_PHI * EVA_PHI)
#define EVA_INV_PHI 0.61803399f

/* --- PPA-composited clouds ------------------------------------------------
 * Three cloud layers keep two A8 geometry variants each. Every variant has
 * light/shadow/core masks, tinted and alpha-blended over the cached sky with
 * the P4 PPA blend engine. Variants crossfade and rebake over time so clouds
 * appear, dissolve, and reform instead of looping as one static strip. */
#ifdef EVA_PORTRAIT_NATIVE
#define CLOUD_STRIP_W 768
#else
#define CLOUD_STRIP_W 800
#endif
#define CLOUD_LAYER_HIGH 0
#define CLOUD_LAYER_MID  1
#define CLOUD_LAYER_LOW  2
#define CLOUD_LAYER_COUNT 3
#define CLOUD_STRIP_OVERFLOW_Y FIB_144   /* clouds bleed this far above/below 480px */
#define CLOUD_VARIANT_COUNT 2

#define BAKE_IDLE      0
#define BAKE_REQUESTED 1
#define BAKE_RUNNING   2
#define BAKE_DONE      3

#define GLASS_GLINT_MAX  FIB_8
#define GLASS_DROP_MAX   FIB_34

typedef struct {
    float x;
    float y;
    float len;
    float phase;
    float strength;
} glass_glint_t;

typedef enum {
    GLASS_DROP_FORMING = 0,
    GLASS_DROP_SLIDING,
    GLASS_DROP_DRYING,
} glass_drop_state_t;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
    float r;
    float target_r;
    float v_term;
    float phase;
    float alpha;
    float alpha_peak;
    glass_drop_state_t state;
    float timer;        /* forming hold or dry fade remaining (s) */
    float form_total;   /* forming duration at spawn (s) */
    float fade_total;   /* dry fade duration at start */
    float slide_quota;  /* px this bead may travel before stopping */
    float dist_slid;
    bool will_slide;
} glass_drop_t;

/* px/s² — gravity along the glass plane (much weaker effective fall than outdoors). */
#define GLASS_SLIDE_GRAVITY  140.0f
/* Terminal slide speed from bead radius: ~23–67 px/s for r≈1–5. */
#define GLASS_VTERM_BASE     12.0f
#define GLASS_VTERM_PER_R    11.0f

static glass_glint_t s_glass_glints[GLASS_GLINT_MAX];
static glass_drop_t s_glass_drops[GLASS_DROP_MAX];
static bool s_glass_glints_inited;
static bool s_glass_drops_inited;
static uint8_t *s_wet_glass;            /* 800×480 A8 wet-pane accumulation */
static int s_wet_y0 = 1 << 30, s_wet_y1 = -1;
static uint8_t s_wet_decay_tick;

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
    rgb_t top;       /* colour at the very top of the sky */
    rgb_t bottom;    /* colour at the horizon line (bottom of screen) */
} sky_t;

typedef struct {
    uint8_t *a8_light;
    uint8_t *a8_shadow;
    uint8_t *a8_core;
    uint16_t content_y0;   /* first populated row of a8_light */
    uint16_t content_y1;   /* one past last populated row */
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
    volatile uint8_t bake_state;
    uint8_t bake_variant;
    /* Pre-baked pool bookkeeping (indices into the .clm files of this layer). */
    uint8_t pool_kind;    /* CLOUD_POOL_NORMAL or CLOUD_POOL_STORM */
    uint8_t pool_cur;
    uint8_t pool_prev;
    float depth_scale;    /* scale baked into the active variant (cloudinfo) */
    bool mirrored;        /* mirror baked into the active variant (cloudinfo) */
    float drift_y;        /* slow vertical drift accumulator, px */
    float drift_speed;    /* px/s, direction re-rolled per variant swap */
} cloud_strip_t;

static const char *TAG = "eva_canvas";
LV_FONT_DECLARE(eva_font_clock_288_extralight);
LV_FONT_DECLARE(eva_font_uk_22);

static lv_obj_t *s_canvas;
static lv_timer_t *s_timer;
static uint16_t *s_buf;

static inline int eva_sbuf_idx(int xl, int yl);

static uint16_t *s_bg_buf;
/* Amortized background repaint: the sky is painted into s_bg_next in
 * ~64-row slices across frames (params snapshotted at slice 0 so the
 * whole buffer is consistent), then pointer-swapped with s_bg_buf. Keeps
 * the 100–160 ms dithered-sky fill out of any single frame. */
static uint16_t *s_bg_next;
/* "sky+sun+text" composite: bg cache with scene text pre-blitted. Rebuilt
 * when the bg cache rebakes OR the text mask changes. Non-precip kinds only
 * (rain must stay above digits — see scene_text_can_cache). */
static uint16_t *s_scene_base;
static bool s_scene_base_dirty = true;
static bool s_scene_text_done_this_frame;
static bool s_merged_storm_active;
static bool s_storm_lit_active;
static float s_rain_loop_t;
static int s_fog_drift_x;
static int s_bg_paint_row = -1;   /* -1 idle, else next row to paint */
static sky_t s_bg_snap_sky;
static float s_bg_snap_sun_x, s_bg_snap_sun_y, s_bg_snap_warmth;
static float s_bg_snap_t;
#define BG_PAINT_ROWS_PER_FRAME 64
static bool s_blend_from_sky;
static uint16_t *s_display_buf;
static uint16_t *s_render_buf;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_dpi_fb[2];
static uint16_t *s_dpi_scan_fb;
static uint16_t *s_dpi_back_fb;
#ifdef EVA_PORTRAIT_NATIVE
static void copy_landscape_rgb565_to_sbuf(const uint16_t *landscape_src)
{
    for (int y = 0; y < EVA_WEATHER_RENDER_H; ++y) {
        for (int x = 0; x < EVA_WEATHER_RENDER_W; ++x) {
            s_buf[eva_land_to_port_idx(x, y)] = landscape_src[y * EVA_WEATHER_RENDER_W + x];
        }
    }
}

static inline bool sbuf_is_portrait_fb(void)
{
    return s_buf == s_dpi_back_fb || s_buf == s_dpi_scan_fb;
}
#endif

static inline int eva_sbuf_idx(int xl, int yl)
{
#ifdef EVA_PORTRAIT_NATIVE
    if (sbuf_is_portrait_fb()) {
        return eva_land_to_port_idx(xl, yl);
    }
#endif
    return yl * EVA_WEATHER_RENDER_W + xl;
}

static SemaphoreHandle_t s_render_lock;
static SemaphoreHandle_t s_ppa_done_sem;
static SemaphoreHandle_t s_vsync_sem;
static TaskHandle_t s_render_task;
static TaskHandle_t s_bake_task;
static bool s_cloud_assets_ok;   /* mmap'd CLP2 pack discovered at init */
/* 3-plane volume rendering (shadow+core under the lit cap). On = the
 * "beautiful" look; costs 3× PPA bands/layer. Toggle via CDC `cloudvolume`
 * to A/B against light-only on hardware. */
static volatile bool s_cloud_volume = false;
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
static bool weather_kind_has_precip_particles(weather_kind_t kind);
static bool weather_kind_has_precip_particles(weather_kind_t kind);

static inline bool scene_text_can_cache(weather_kind_t kind)
{
    /* Non-precip kinds bake text into s_scene_base. Precip keeps per-frame
     * text so rain stays under the clock digits (z-order). */
    return !weather_kind_has_precip_particles(kind);
}

static float s_density_scale = 1.0f;
/* Smooth weather transition state. Set by eva_weather_canvas_set_weather(),
 * ticked by wx_transition_tick() in render_weather(). See
 * docs/superpowers/specs/2026-07-19-smooth-weather-transitions-design.md */
static eva_wx_transition_t s_wx_trans;
static float s_wx_trans_duration_s = EVA_WX_TRANSITION_DEFAULT_S;
static float s_wx_last_bake_p = -1.0f;   /* eased progress at last sky rebake */
static precip_type_t s_precip_type = PRECIP_NONE;
static particle_t s_particles[PARTICLE_MAX];
static cloud_strip_t s_strip[CLOUD_LAYER_COUNT] = {
    /* Calm-air base drift speeds in px/s — chosen from the Fibonacci ladder so
     * the three layers move at golden-ratio offsets. HIGH cirrus barely moves,
     * MID altocumulus drifts visibly, LOW cumulus is the parallax foreground.
     * Final speed = base * wind_factor(kph) and the LOW layer takes a larger
     * multiplier under heavy wind (cumulus catches gusts more than cirrus). */
    /* Full-sky immersion: strips extend well above and below the 480px frame so
     * cloud mass continues past the rim — viewer feels inside the layer, not
     * below a ceiling. PPA blend clips to the viewport; overflow rows stay in
     * the mask for scroll/bob and soft parallax at the edges. */
    [CLOUD_LAYER_HIGH] = {
        .y_start = -CLOUD_STRIP_OVERFLOW_Y,
        .strip_h = EVA_WEATHER_RENDER_H + 2 * CLOUD_STRIP_OVERFLOW_Y,
        .base_speed = (float)FIB_5,
        .morph_hold_s = (float)FIB_55, .morph_duration_s = (float)FIB_8,
    },
    [CLOUD_LAYER_MID] = {
        .y_start = -CLOUD_STRIP_OVERFLOW_Y,
        .strip_h = EVA_WEATHER_RENDER_H + 2 * CLOUD_STRIP_OVERFLOW_Y,
        .base_speed = (float)FIB_13,
        .morph_hold_s = (float)FIB_34, .morph_duration_s = (float)FIB_8,
    },
    [CLOUD_LAYER_LOW] = {
        .y_start = -CLOUD_STRIP_OVERFLOW_Y,
        .strip_h = EVA_WEATHER_RENDER_H + 2 * CLOUD_STRIP_OVERFLOW_Y,
        .base_speed = (float)FIB_21,
        .morph_hold_s = (float)FIB_21, .morph_duration_s = (float)FIB_13,
    },
};
/* Sun position cache — populated by draw_sun_or_moon() in the bg-cache pass,
 * read by glass glints and cloud tinting. */
static int s_sun_x = -1;
static int s_sun_y = -1;
static int s_sun_r = 0;
static bool s_sun_visible = false;
static float s_sun_strength = 0.0f;
/* Sun elevation 0..1: 0 = at the horizon (sunrise/sunset), 1 = solar noon
 * apex. Set in draw_sun_or_moon(), read by update_cloud_tints() so cloud
 * lighting tracks how high the sun is — warm low-angle light at dawn/dusk,
 * bright top-down light at midday. -1 while the sun is below the horizon. */
static float s_sun_elevation = -1.0f;

/* Normalised luminary positions — sun drives fill_sky()/cloud-tint; moon is
 * drawn independently. Refreshed once per bg-cache pass in render_weather(). */
typedef struct {
    float x_n;
    float y_n;
    float elevation;   /* sin(progress·π): negative = below horizon */
    float warmth;      /* radial sky glow strength, 0 below horizon / at night */
    bool  valid;       /* in visible arc (incl. glide zones) */
} luminary_pos_t;
static luminary_pos_t s_sun_pos;
static luminary_pos_t s_moon_pos;

/* Horizon-band sky colour from the last bg-cache pass. Rain streaks tint
 * themselves from this so the drops read as the actual sky refracted through
 * water — cool grey-blue by day, near-black at night — instead of a fixed
 * bright white that looked unreal against a dark storm sky. */
static rgb_t s_sky_bottom = {96, 108, 118};

#define SUN_HORIZON_Y      0.90f
#define SUN_APEX_Y_SUMMER  0.12f
#define SUN_APEX_Y_WINTER  0.35f
#define SET_GLIDE_MIN      25.0f

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
static float s_lightning_peak;
static float s_lightning_next_strike_at;
/* Set from the CDC task ("lightning" test command); consumed by
 * update_lightning() in the render task on the next frame. */
static volatile bool s_lightning_force;
static bool s_lightning_active;
static bool s_lightning_sheet_only;
static bool s_lightning_has_branch;
static float s_lightning_channel_alpha;
static float s_lightning_flash_alpha;
static float s_lightning_strike_age;
static float s_lightning_afterglow;
static float s_lightning_fade_start;
static float s_lightning_fade_dur;
static bool s_lightning_in_fade;
static uint8_t s_lightning_stroke_total;
static uint8_t s_lightning_stroke_next;
#define LIGHTNING_STROKE_MAX 6
static float s_lightning_stroke_t[LIGHTNING_STROKE_MAX];
static float s_lightning_stroke_k[LIGHTNING_STROKE_MAX];
#define LIGHTNING_PT_MAX     FIB_21
#define LIGHTNING_BRANCH_MAX FIB_5
/* Glow plane rendered dimmer than the core so the halo doesn't oversaturate
 * to near-white against a bright storm sky (core = full bolt alpha).
 * Bolt direction/variant counts live in eva_clp_toc.h (EVA_BOLT_*). */
#define BOLT_GLOW_SCALE_NUM  3
#define BOLT_GLOW_SCALE_DEN  4
static int s_lightning_pt_count;
static int s_lightning_branch_pts;
static int16_t s_lightning_x[LIGHTNING_PT_MAX];
static int16_t s_lightning_y[LIGHTNING_PT_MAX];
static int16_t s_lightning_bx[LIGHTNING_BRANCH_MAX];
static int16_t s_lightning_by[LIGHTNING_BRANCH_MAX];
static int16_t s_lightning_flash_x;
static int16_t s_lightning_flash_y;
/* s_bolt_* = sprite-path state for lightning, grouped with the s_lightning_*
 * statics above (short prefix; s_lightning_bolt_* would be unwieldy). */
static eva_sprite_t s_bolt_sprite;      /* active strike's sprite */
static bool s_bolt_sprite_ok;
static int s_bolt_x, s_bolt_y;          /* top-left blit anchor */
static bool s_bolt_mirror;

typedef struct {
    uint8_t  channel_alpha;
    uint8_t  flash_alpha;
    uint8_t  sheet_only;
    uint16_t flash_x;
    uint16_t flash_y;
    uint8_t  pt_count;
    int16_t  x[LIGHTNING_PT_MAX];
    int16_t  y[LIGHTNING_PT_MAX];
    bool     has_branch;
    int      branch_pts;
    int16_t  bx[LIGHTNING_BRANCH_MAX];
    int16_t  by[LIGHTNING_BRANCH_MAX];
    bool     bolt_sprite_ok;
    int      bolt_x;
    int      bolt_y;
    bool     bolt_mirror;
} lightning_snap_t;

static lightning_snap_t s_li_snap[2];
static volatile uint8_t s_li_front;
static portMUX_TYPE s_li_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t s_bg_ttl;
static float s_bg_dt;
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
static int64_t s_prof_text_us;
static int64_t s_prof_clouds_us;
static int64_t s_prof_particles_us;
static int64_t s_prof_lightning_us;
static int64_t s_prof_glass_us;
static uint32_t s_prof_blend_bands;    /* PPA band blends this log window */
static uint32_t s_prof_morph_frames;   /* frames with ≥1 layer morphing */
static char s_clock_text[16] = "00:00";
static char s_date_text[24] = "";
static char s_temp_text[16] = "+0C";
static char s_desc_text[96] = "";
static portMUX_TYPE s_text_mux = portMUX_INITIALIZER_UNLOCKED;

/* Pre-baked A8 scene text: max-size clock in the centre band; date/temp
 * above and desc below in the remaining vertical space. One full-frame mask
 * is baked when any line changes, then blitted once per frame. */
#define TEXT_SLOT_BUF_W   EVA_WEATHER_RENDER_W
#define TEXT_SLOT_BUF_H   EVA_WEATHER_RENDER_H
#define TEXT_SLOT_BUF_BYTES (TEXT_SLOT_BUF_W * TEXT_SLOT_BUF_H)
#define SCENE_MARGIN        24   /* same gap: screen edge ↔ text ↔ clock ↔ text ↔ edge */
#define SCENE_INFO_LINE_GAP  6   /* gap between date and temp within top block */
#define SCENE_CLOCK_SCALE_Q8 256 /* native eva_font_clock_288_extralight (2× former 144px) */
#define SCENE_EVENING_MIN   105   /* minutes before sunset → evening palette/clock */

typedef enum {
    SCENE_DAYPART_NIGHT = 0,
    SCENE_DAYPART_MORNING,
    SCENE_DAYPART_DAY,
    SCENE_DAYPART_EVENING,
} scene_daypart_t;

typedef struct {
    uint8_t *a8;
    uint16_t mask_w;
    uint16_t mask_h;
    uint16_t bbox_x0;
    uint16_t bbox_y0;
    uint16_t bbox_x1;
    uint16_t bbox_y1;
    char key_clock[16];
    char key_date[24];
    char key_temp[16];
    char key_desc[96];
    uint16_t key_clock_scale_q8;
    uint8_t key_daypart;
    bool valid;
    bool bbox_valid;
    /* Glyph-run spans over rows [bbox_y0, bbox_y1) — built at bake time,
     * walked by the per-frame blit. spans_valid false → bbox fallback. */
    eva_span_t *spans;
    uint32_t *row_start;
    bool spans_valid;
} scene_text_slot_t;

static scene_text_slot_t s_scene_slot;

static scene_daypart_t scene_daypart_now(void);

/* Static glyph draw buffer for draw_text_utf8.
 *
 * Root cause: lv_font_get_bitmap_fmt_txt() does `bitmap_out = draw_buf->data`
 * with no NULL check. Passing NULL as draw_buf dereferences NULL → crash.
 * LVGL labels normally work because LVGL's label widget allocates a draw_buf
 * via the draw layer; our render task lives outside LVGL and must supply one.
 *
 * Size: largest expected glyph is eva_font_clock_288_extralight (~139×217 A8
 * ≈ 30 KB). 256×256 = 65536 bytes gives headroom for clock digits and
 * eva_font_uk_22 accented Cyrillic glyphs. */
#define GLYPH_BUF_W      256
#define GLYPH_BUF_H      256
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
    if (alpha >= 240) return src;

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
    uint16_t *p = &s_buf[eva_sbuf_idx(x, y)];
    if (alpha >= 240) {
        *p = color;
        return;
    }
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

/* Horizontal centre in the full canvas (SCENE_MARGIN is already baked into the
 * symmetric formula via inner_w). */
static int scene_center_x(int line_w)
{
    return (TEXT_SLOT_BUF_W - line_w) / 2;
}

/* Sun halo overlaps the top info band (date/temp) when the luminary sits in the
 * upper sky — shift that block sideways so margins look balanced. Clock stays
 * centred; only the small info lines move. */
static int scene_top_info_x_shift(int band_y, int band_h, int band_w)
{
    if (!s_sun_visible || s_sun_x < 0 || s_sun_y < 0 || band_w <= 0 || band_h <= 0) {
        return 0;
    }

    const int halo_r = FIB_144;
    int sun_l = s_sun_x - halo_r;
    int sun_r = s_sun_x + halo_r;
    int sun_t = s_sun_y - halo_r;
    int sun_b = s_sun_y + halo_r;
    int tx_l = scene_center_x(band_w);
    int tx_r = tx_l + band_w;
    int ty_t = band_y;
    int ty_b = band_y + band_h;

    if (sun_b < ty_t || sun_t > ty_b || sun_r <= tx_l || sun_l >= tx_r) {
        return 0;
    }

    int shift = 0;
    if (s_sun_x <= TEXT_SLOT_BUF_W / 2) {
        shift = sun_r + SCENE_MARGIN - tx_l;
    } else {
        shift = -(tx_r - (sun_l - SCENE_MARGIN));
    }
    if (shift == 0) return 0;

    int nx_l = tx_l + shift;
    if (nx_l < SCENE_MARGIN) {
        shift += SCENE_MARGIN - nx_l;
    }
    int nx_r = tx_l + shift + band_w;
    if (nx_r > TEXT_SLOT_BUF_W - SCENE_MARGIN) {
        shift -= nx_r - (TEXT_SLOT_BUF_W - SCENE_MARGIN);
    }
    return shift;
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

/* Bake one line of `text` at `scale_q8` into canvas `a8` (stride canvas_w),
 * with the line's top-left anchor at (dst_x, dst_y). */
static void bake_line_into_a8(uint8_t *a8, int canvas_w, int canvas_h,
                              int dst_x, int dst_y,
                              const lv_font_t *font, const char *text,
                              uint16_t scale_q8)
{
    if (!a8 || !font || !text || !text[0]) return;

    int line_h_scaled = text_height_utf8_scaled(font, scale_q8);
    int base_line_scaled = (int)(((int32_t)font->base_line * (int32_t)scale_q8 + 128) >> 8);
    int line_top = dst_y + (line_h_scaled - base_line_scaled);

    int pen_x_q8 = (int32_t)dst_x << 8;
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
                if ((unsigned)py >= (unsigned)canvas_h) continue;
                uint32_t sy = ((uint32_t)dy * inv_q16) >> 16;
                if (sy >= g.box_h) continue;
                const uint8_t *src_row = &bitmap[sy * g.box_w];
                uint8_t *dst_row = &a8[py * canvas_w];
                for (int dx = 0; dx < dst_box_w; ++dx) {
                    int px = gx0 + dx;
                    if ((unsigned)px >= (unsigned)canvas_w) continue;
                    uint32_t sx = ((uint32_t)dx * inv_q16) >> 16;
                    if (sx >= g.box_w) continue;
                    uint8_t alpha = src_row[sx];
                    if (alpha == 0) continue;
                    if (alpha > dst_row[px]) dst_row[px] = alpha;
                }
            }
        }
        pen_x_q8 += ((int32_t)g.adv_w * (int32_t)scale_q8);
        lv_font_glyph_release_draw_data(&g);
        i = i_next;
    }
}

static void bake_scene_text_slot(scene_text_slot_t *slot,
                                 const char *clock_txt,
                                 const char *date_txt,
                                 const char *temp_txt,
                                 const char *desc_txt)
{
    if (!slot || !slot->a8) return;

    scene_daypart_t daypart = scene_daypart_now();
    if (slot->valid &&
        slot->key_daypart == (uint8_t)daypart &&
        strncmp(slot->key_clock, clock_txt, sizeof(slot->key_clock)) == 0 &&
        strncmp(slot->key_date, date_txt, sizeof(slot->key_date)) == 0 &&
        strncmp(slot->key_temp, temp_txt, sizeof(slot->key_temp)) == 0 &&
        strncmp(slot->key_desc, desc_txt, sizeof(slot->key_desc)) == 0) {
        return;
    }

    const lv_font_t *clock_font = &eva_font_clock_288_extralight;
    const lv_font_t *info_font = &eva_font_uk_22;
    const uint16_t info_scale_q8 = 256;
    const uint16_t clock_scale_q8 = SCENE_CLOCK_SCALE_Q8;

    memset(slot->a8, 0, TEXT_SLOT_BUF_BYTES);

    const int canvas_w = TEXT_SLOT_BUF_W;
    const int canvas_h = TEXT_SLOT_BUF_H;

    int info_lh = text_height_utf8_scaled(info_font, info_scale_q8);
    int top_h = 0;
    if (date_txt[0]) {
        top_h += info_lh;
        if (temp_txt[0]) top_h += SCENE_INFO_LINE_GAP;
    }
    if (temp_txt[0]) top_h += info_lh;
    int bottom_h = desc_txt[0] ? info_lh : 0;
    int clock_slot_h = clock_txt[0]
        ? text_height_utf8_scaled(clock_font, clock_scale_q8) : 0;

    /* Clock on the vertical centre; date/temp and desc each sit in their own
     * band (top edge↔clock, clock↔bottom edge) with SCENE_MARGIN padding. */
    int clock_y = clock_slot_h > 0 ? (canvas_h - clock_slot_h) / 2 : 0;

    int top_y = SCENE_MARGIN;
    if (top_h > 0 && clock_slot_h > 0) {
        int band_top = SCENE_MARGIN;
        int band_bot = clock_y - SCENE_MARGIN;
        int band_h = band_bot - band_top;
        if (band_h > top_h) {
            top_y = band_top + (band_h - top_h) / 2;
        } else {
            top_y = band_top;
        }
    }

    int desc_y = 0;
    if (bottom_h > 0 && clock_slot_h > 0) {
        int band_top = clock_y + clock_slot_h + SCENE_MARGIN;
        int band_bot = canvas_h - SCENE_MARGIN;
        int band_h = band_bot - band_top;
        if (band_h > bottom_h) {
            desc_y = band_top + (band_h - bottom_h) / 2;
        } else {
            desc_y = band_top;
        }
    }

    int date_w = date_txt[0] ? text_width_utf8_scaled(info_font, date_txt, info_scale_q8) : 0;
    int temp_w = temp_txt[0] ? text_width_utf8_scaled(info_font, temp_txt, info_scale_q8) : 0;
    int top_band_w = date_w > temp_w ? date_w : temp_w;
    int top_x_shift = scene_top_info_x_shift(top_y, top_h, top_band_w);

    int y = top_y;
    if (date_txt[0]) {
        int lw = date_w;
        int x = scene_center_x(lw) + top_x_shift;
        bake_line_into_a8(slot->a8, canvas_w, canvas_h, x, y,
                          info_font, date_txt, info_scale_q8);
        y += info_lh;
        if (temp_txt[0]) y += SCENE_INFO_LINE_GAP;
    }
    if (temp_txt[0]) {
        int lw = temp_w;
        int x = scene_center_x(lw) + top_x_shift;
        bake_line_into_a8(slot->a8, canvas_w, canvas_h, x, y,
                          info_font, temp_txt, info_scale_q8);
    }

    if (clock_slot_h > 0) {
        int line_w = text_width_utf8_scaled(clock_font, clock_txt, clock_scale_q8);
        int clock_x = scene_center_x(line_w);
        bake_line_into_a8(slot->a8, canvas_w, canvas_h, clock_x, clock_y,
                          clock_font, clock_txt, clock_scale_q8);
    }

    if (desc_txt[0]) {
        int lw = text_width_utf8_scaled(info_font, desc_txt, info_scale_q8);
        int x = scene_center_x(lw);
        bake_line_into_a8(slot->a8, canvas_w, canvas_h, x, desc_y,
                          info_font, desc_txt, info_scale_q8);
    }

    slot->mask_w = (uint16_t)canvas_w;
    slot->mask_h = (uint16_t)canvas_h;
    strlcpy(slot->key_clock, clock_txt, sizeof(slot->key_clock));
    strlcpy(slot->key_date, date_txt, sizeof(slot->key_date));
    strlcpy(slot->key_temp, temp_txt, sizeof(slot->key_temp));
    strlcpy(slot->key_desc, desc_txt, sizeof(slot->key_desc));
    slot->key_clock_scale_q8 = clock_scale_q8;
    slot->key_daypart = (uint8_t)daypart;
    slot->valid = true;

    /* Tight bbox for per-frame blit — full 800×480 scan only on text rebake. */
    slot->bbox_valid = false;
    int bx0 = canvas_w, by0 = canvas_h, bx1 = -1, by1 = -1;
    for (int row = 0; row < canvas_h; ++row) {
        const uint8_t *mrow = &slot->a8[row * canvas_w];
        for (int col = 0; col < canvas_w; ++col) {
            if (mrow[col] == 0) continue;
            if (col < bx0) bx0 = col;
            if (col > bx1) bx1 = col;
            if (row < by0) by0 = row;
            if (row > by1) by1 = row;
        }
    }
    if (bx1 >= bx0 && by1 >= by0) {
        slot->bbox_x0 = (uint16_t)bx0;
        slot->bbox_y0 = (uint16_t)by0;
        slot->bbox_x1 = (uint16_t)(bx1 + 1);
        slot->bbox_y1 = (uint16_t)(by1 + 1);
        slot->bbox_valid = true;
    }

    /* Span table for the per-frame blit (17 ms bbox scan → glyph runs only).
     * Capacity: every bbox row at the per-row cap. Allocated once, PSRAM. */
    slot->spans_valid = false;
    if (slot->bbox_valid) {
        int rows = slot->bbox_y1 - slot->bbox_y0;
        int cap = rows * EVA_TEXT_MAX_SPANS_PER_ROW;
        if (!slot->spans) {
            slot->spans = heap_caps_malloc(
                (size_t)canvas_h * EVA_TEXT_MAX_SPANS_PER_ROW * sizeof(eva_span_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            slot->row_start = heap_caps_malloc(
                ((size_t)canvas_h + 1) * sizeof(uint32_t),
                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (slot->spans && slot->row_start) {
            int n = eva_text_build_spans(slot->a8, canvas_w,
                                         slot->bbox_y0, slot->bbox_y1,
                                         slot->spans, cap, slot->row_start);
            slot->spans_valid = (n >= 0);
        }
    }
    s_scene_base_dirty = true;
}

static void blit_text_mask_at(const scene_text_slot_t *slot, int dst_x, int dst_y,
                              int x0, int y0, int x1, int y1,
                              uint16_t color, uint8_t base_alpha)
{
    int w = slot->mask_w;
    if (slot->spans_valid) {
        for (int y = y0; y < y1; ++y) {
            int mrow = y - dst_y;                     /* mask row */
            int srow = mrow - (int)slot->bbox_y0;     /* span-table row */
            if (srow < 0 || mrow >= (int)slot->bbox_y1) continue;
            const uint8_t *mask_row = &slot->a8[mrow * w];
            for (uint32_t s = slot->row_start[srow];
                 s < slot->row_start[srow + 1]; ++s) {
                int mx0 = slot->spans[s].x;
                int mx1 = mx0 + slot->spans[s].len;
                int dx0 = mx0 + dst_x, dx1 = mx1 + dst_x;
                if (dx0 < x0) dx0 = x0;
                if (dx1 > x1) dx1 = x1;
                for (int x = dx0; x < dx1; ++x) {
                    uint8_t m = mask_row[x - dst_x];
                    if (m == 0) continue;
                    uint8_t a = (uint8_t)(((uint16_t)m * (uint16_t)base_alpha) / 255U);
                    if (a == 0) continue;
                    int idx = eva_sbuf_idx(x, y);
                    s_buf[idx] = blend565(s_buf[idx], color, a);
                }
            }
        }
        return;
    }
    /* Fallback: original bbox scan. */
    for (int y = y0; y < y1; ++y) {
        const uint8_t *mask_row = &slot->a8[(y - dst_y) * w + (x0 - dst_x)];
        int run = x1 - x0;
        for (int i = 0; i < run; ++i) {
            uint8_t m = mask_row[i];
            if (m == 0) continue;
            uint8_t a = (uint8_t)(((uint16_t)m * (uint16_t)base_alpha) / 255U);
            if (a == 0) continue;
            int idx = eva_sbuf_idx(x0 + i, y);
            s_buf[idx] = blend565(s_buf[idx], color, a);
        }
    }
}

/* Blit the slot's cached A8 mask onto s_buf at (dst_x, dst_y) using the
 * given colour and base alpha. Hot path: one row of mask = one row of
 * RGB565 writes, only touching pixels with non-zero mask alpha. */
static void blit_text_slot(const scene_text_slot_t *slot, int dst_x, int dst_y,
                           uint16_t color, uint8_t base_alpha)
{
    if (!slot || !slot->valid || !slot->a8 || base_alpha == 0) return;
    if (slot->mask_w == 0 || slot->mask_h == 0) return;

    int w = slot->mask_w;
    int x0 = dst_x, y0 = dst_y;
    int x1 = dst_x + w, y1 = dst_y + (int)slot->mask_h;
    if (slot->bbox_valid) {
        x0 = dst_x + (int)slot->bbox_x0;
        y0 = dst_y + (int)slot->bbox_y0;
        x1 = dst_x + (int)slot->bbox_x1;
        y1 = dst_y + (int)slot->bbox_y1;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > EVA_WEATHER_RENDER_W) x1 = EVA_WEATHER_RENDER_W;
    if (y1 > EVA_WEATHER_RENDER_H) y1 = EVA_WEATHER_RENDER_H;
    if (x0 >= x1 || y0 >= y1) return;

    /* Soft shadow — helps clock/date read through thin cloud edges. */
    int sh_x0 = x0 + 2;
    int sh_y0 = y0 + 2;
    int sh_x1 = x1 + 2;
    int sh_y1 = y1 + 2;
    if (sh_x0 < EVA_WEATHER_RENDER_W && sh_y0 < EVA_WEATHER_RENDER_H &&
        sh_x0 < sh_x1 && sh_y0 < sh_y1) {
        if (sh_x1 > EVA_WEATHER_RENDER_W) sh_x1 = EVA_WEATHER_RENDER_W;
        if (sh_y1 > EVA_WEATHER_RENDER_H) sh_y1 = EVA_WEATHER_RENDER_H;
        blit_text_mask_at(slot, dst_x + 2, dst_y + 2, sh_x0, sh_y0, sh_x1, sh_y1,
                          rgb565(8, 12, 24), (uint8_t)(base_alpha * 3 / 5));
    }
    blit_text_mask_at(slot, dst_x, dst_y, x0, y0, x1, y1, color, base_alpha);
}

static void draw_scene_text_overlays(void)
{
    char clock_txt[sizeof(s_clock_text)];
    char date_txt[sizeof(s_date_text)];
    char temp_txt[sizeof(s_temp_text)];
    char desc_txt[sizeof(s_desc_text)];
    portENTER_CRITICAL(&s_text_mux);
    memcpy(clock_txt, s_clock_text, sizeof(clock_txt));
    memcpy(date_txt, s_date_text, sizeof(date_txt));
    memcpy(temp_txt, s_temp_text, sizeof(temp_txt));
    memcpy(desc_txt, s_desc_text, sizeof(desc_txt));
    portEXIT_CRITICAL(&s_text_mux);
    clock_txt[sizeof(clock_txt) - 1] = '\0';
    date_txt[sizeof(date_txt) - 1] = '\0';
    temp_txt[sizeof(temp_txt) - 1] = '\0';
    desc_txt[sizeof(desc_txt) - 1] = '\0';

    bake_scene_text_slot(&s_scene_slot, clock_txt, date_txt, temp_txt, desc_txt);
    blit_text_slot(&s_scene_slot, 0, 0, rgb565(255, 255, 255), 240);
}

static rgb_t lerp_rgb(rgb_t a, rgb_t b, float t);
static float solar_declination_deg(void);
static float sky_cover_fraction(void);
static float sun_curve(float progress);

/* Paint sky rows [y0, y1) into `dst`. Row-sliceable on purpose: the
 * amortized background repaint spreads the full 480-row fill (dither +
 * radial sun warmth ≈ 100–160 ms) across several frames instead of
 * stalling one frame per rebake (the "FPS dips twice a second in rain"
 * bug, 2026-07-03 — precip kinds rebake every 5–13 frames). */
static void fill_sky_rows(uint16_t *dst, int row0, int row1,
                          rgb_t top, rgb_t bottom,
                          float sun_x_n, float sun_y_n, float warmth)
{
    /* Vertical base (gamma-biased horizon) plus optional radial warm glow
     * centred on the sun. warmth=0 degenerates to the old fill_gradient(). */
    const float horizon_gamma = 2.2f;
    const int W = EVA_WEATHER_RENDER_W;
    const int H = EVA_WEATHER_RENDER_H;
    const int sun_x = (int)(sun_x_n * (float)W);
    const int sun_y = (int)(sun_y_n * (float)H);
    const float R2 = (float)(W * W);
    const rgb_t warm_tint = {
        .r = (uint8_t)(bottom.r < 250 ? bottom.r + (255 - bottom.r) / 3 : 255),
        .g = (uint8_t)(bottom.g * 85 / 100),
        .b = (uint8_t)(bottom.b * 55 / 100),
    };
    const bool use_glow = warmth > 0.01f;

    if (!use_glow) {
        static const int8_t sky_kind_map[WEATHER_KIND_COUNT] = {
            [WEATHER_CLEAR_DAY] = 1,
            [WEATHER_PARTLY_CLOUDY_DAY] = 3,
            [WEATHER_CLOUDY] = 5,
            [WEATHER_FOG] = 6,
            [WEATHER_RAIN] = 7,
            [WEATHER_HEAVY_RAIN] = 8,
            [WEATHER_THUNDERSTORM] = 10,
            [WEATHER_SNOW] = 9,
        };
        scene_daypart_t dp = scene_daypart_now();
        int di = (dp == SCENE_DAYPART_NIGHT) ? 2 :
                 (dp == SCENE_DAYPART_EVENING) ? 1 :
                 (dp == SCENE_DAYPART_MORNING) ? 3 : 0;
        int ki = (s_kind > WEATHER_UNKNOWN && s_kind < WEATHER_KIND_COUNT) ?
                 sky_kind_map[s_kind] : 0;
        eva_sprite_t sp;
        if (ki > 0 && eva_cloud_assets_sprite(EVA_CLP_TYPE_SKY, ki, di, &sp) &&
            sp.plane[0] && sp.h >= (uint16_t)H) {
            const uint16_t *col = (const uint16_t *)sp.plane[0];
            for (int y = row0; y < row1 && y < H; ++y) {
                uint16_t px = col[y];
                uint16_t *row = &dst[y * W];
                for (int x = 0; x < W; ++x) {
                    row[x] = px;
                }
            }
            return;
        }
    }

    for (int y = row0; y < row1 && y < H; ++y) {
        float yn = (float)y / (float)(H - 1);
        float t = powf(yn, horizon_gamma);
        int ti = (int)(t * 255.0f + 0.5f);
        rgb_t base = {
            .r = (uint8_t)(top.r + (((int)bottom.r - top.r) * ti) / 255),
            .g = (uint8_t)(top.g + (((int)bottom.g - top.g) * ti) / 255),
            .b = (uint8_t)(top.b + (((int)bottom.b - top.b) * ti) / 255),
        };
        uint16_t *row = &dst[y * W];
        if (!use_glow) {
            for (int x = 0; x < W; ++x) {
                row[x] = eva_dither565(base.r, base.g, base.b, x, y);
            }
            continue;
        }
        for (int x = 0; x < W; ++x) {
            int dx = x - sun_x;
            int dy = y - sun_y;
            float d2 = (float)(dx * dx + dy * dy);
            float glow = 1.0f - d2 / R2;
            if (glow < 0.0f) glow = 0.0f;
            float blend = glow * warmth;
            rgb_t c = lerp_rgb(base, warm_tint, blend);
            row[x] = eva_dither565(c.r, c.g, c.b, x, y);
        }
    }
}

static void fill_sky(rgb_t top, rgb_t bottom, float sun_x_n, float sun_y_n, float warmth)
{
    fill_sky_rows(s_buf, 0, EVA_WEATHER_RENDER_H,
                  top, bottom, sun_x_n, sun_y_n, warmth);
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

/* Observer latitude in degrees, matching the coordinates baked into the
 * Open-Meteo request URL (weather_fetch_openmeteo.c). Used to compute
 * twilight duration locally since neither provider returns twilight times. */
#define EVA_OBSERVER_LAT_DEG 48.915155f

/* Civil-twilight half-duration in minutes: how long after sunset (or before
 * sunrise) the sun takes to drop from the horizon (0°) to −6°, i.e. from
 * "official sunset" to "full dark enough that the warm glow is gone".
 *
 * Open-Meteo gives only geometric sunrise/sunset (sun at 0°), so we derive
 * the −6° crossing from spherical astronomy:
 *
 *   The hour angle H at which the sun sits at altitude `alt` is
 *     cos(H) = (sin(alt) − sin(φ)·sin(δ)) / (cos(φ)·cos(δ))
 *   with φ = latitude and δ = solar declination for the day of year.
 *   Twilight length = (H(−6°) − H(0°)) converted from degrees to minutes
 *   (Earth turns 360° in 1440 min → 4 min per degree).
 *
 * Real consequence: twilight is short near the equator / equinox and grows
 * toward the poles and the summer solstice — exactly what we want so dusk on
 * a long June evening lingers longer than a crisp winter one. Result is
 * clamped to a sane [20, 180] min so high-latitude edge cases (where the sun
 * never reaches −6°) don't explode. */
static float solar_declination_deg(void)
{
    time_t now = time(NULL);
    int doy = 172;
    if (now > 1700000000) {
        struct tm tm_now = {0};
        localtime_r(&now, &tm_now);
        doy = tm_now.tm_yday + 1;
    }
    const float DEG2RAD = 3.14159265f / 180.0f;
    return 23.44f * sinf(DEG2RAD * (360.0f / 365.0f) * (float)(doy - 81));
}

static float civil_twilight_minutes(void)
{
    const float DEG2RAD = 3.14159265f / 180.0f;
    float decl = solar_declination_deg() * DEG2RAD;
    float lat  = EVA_OBSERVER_LAT_DEG * DEG2RAD;

    float cos_lat = cosf(lat), sin_lat = sinf(lat);
    float cos_decl = cosf(decl), sin_decl = sinf(decl);
    float denom = cos_lat * cos_decl;
    if (denom < 1e-4f) denom = 1e-4f;

    /* Hour angle (radians) at altitude 0° and −6°. */
    float c0 = (0.0f          - sin_lat * sin_decl) / denom;
    float c6 = (sinf(-6.0f * DEG2RAD) - sin_lat * sin_decl) / denom;
    if (c0 < -1.0f) c0 = -1.0f; else if (c0 > 1.0f) c0 = 1.0f;
    if (c6 < -1.0f) c6 = -1.0f; else if (c6 > 1.0f) c6 = 1.0f;
    float h0 = acosf(c0);
    float h6 = acosf(c6);

    /* Convert the hour-angle delta to minutes (4 min per degree). */
    float minutes = (h6 - h0) / DEG2RAD * 4.0f;
    if (minutes < 20.0f) minutes = 20.0f;
    else if (minutes > 180.0f) minutes = 180.0f;
    return minutes;
}

static rgb_t lerp_rgb(rgb_t a, rgb_t b, float t)
{
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    rgb_t out = {
        .r = (uint8_t)(a.r + (int)((b.r - a.r) * t)),
        .g = (uint8_t)(a.g + (int)((b.g - a.g) * t)),
        .b = (uint8_t)(a.b + (int)((b.b - a.b) * t)),
    };
    return out;
}

/* Continuous clear-sky palette as a function of wall-clock minute, anchored
 * to the real sunrise (sr) and sunset (ss) minutes. Returns top + horizon
 * colours that morph smoothly through the whole day — no window snapping.
 *
 * The day is modelled as a normalised "sun elevation phase":
 *   phase < 0          → night
 *   phase in [0, twi)  → dawn/dusk twilight (warm horizon glow)
 *   phase >= twi       → full day
 * The same curve runs forward at sunrise and backward at sunset, so dawn and
 * dusk share the warm-horizon treatment symmetrically. */

static eva_sky_ctx_t sky_ctx_now(void)
{
    int sr, ss;
    sun_events(&sr, &ss);
    return (eva_sky_ctx_t){
        .minute = minutes_now(),
        .sunrise_min = sr,
        .sunset_min = ss,
        .cloud_cover = sky_cover_fraction(),
        .solar_decl_deg = solar_declination_deg(),
        .civil_twilight_min = civil_twilight_minutes(),
        .clock_synced = (time(NULL) > 1700000000),
    };
}

static sky_t sky_from_eva(eva_sky_t s)
{
    return (sky_t){
        s.name,
        { s.top.r, s.top.g, s.top.b },
        { s.bottom.r, s.bottom.g, s.bottom.b },
    };
}

static sky_t sky_for_kind(weather_kind_t kind)
{
    eva_sky_ctx_t ctx = sky_ctx_now();
    return sky_from_eva(eva_sky_for_kind(kind, &ctx));
}

/* Sky to paint this rebake: during a transition, blend the from-kind and
 * to-kind palettes by the eased progress; otherwise just the current kind.
 * Both sky_for_kind() calls fold in the current daypart/nightness, so
 * day/night stays correct throughout. Two rgb lerps, cold path (per rebake).*/
static sky_t wx_current_sky(void)
{
    if (!s_wx_trans.active) {
        return sky_for_kind(s_kind);
    }
    float p = eva_wx_ease(s_wx_trans.progress);
    sky_t a = sky_for_kind((weather_kind_t)s_wx_trans.from_kind);
    sky_t b = sky_for_kind((weather_kind_t)s_wx_trans.to_kind);
    sky_t out = b;                 /* carry b.name */
    out.top    = lerp_rgb(a.top,    b.top,    p);
    out.bottom = lerp_rgb(a.bottom, b.bottom, p);
    return out;
}

/* Visual daypart from wall clock + sun events. Shared by clock scale, sky
 * warmth, and text cache invalidation — independent of forced day/night kinds
 * used only for luminary placement in weatherdebug. */
static scene_daypart_t scene_daypart_now(void)
{
    eva_sky_ctx_t ctx = sky_ctx_now();
    int m = ctx.minute;
    int sr = ctx.sunrise_min;
    int ss = ctx.sunset_min;
    float n = eva_sky_nightness(m, sr, ss, ctx.civil_twilight_min);
    if (n >= 0.92f) {
        return SCENE_DAYPART_NIGHT;
    }
    if (n > 0.08f) {
        return SCENE_DAYPART_EVENING;
    }
    if (m >= sr && m < sr + 120) {
        return SCENE_DAYPART_MORNING;
    }
    if (m >= ss - EVA_SCENE_EVENING_MIN && m <= ss) {
        return SCENE_DAYPART_EVENING;
    }
    return SCENE_DAYPART_DAY;
}

static bool sky_kind_is_clearish(weather_kind_t kind)
{
    return kind == WEATHER_CLEAR_DAY || kind == WEATHER_PARTLY_CLOUDY_DAY ||
           kind == WEATHER_CLEAR_NIGHT || kind == WEATHER_PARTLY_CLOUDY_NIGHT;
}

static void draw_day_sky_depth(void)
{
    if (is_night_kind(s_kind) || !sky_kind_is_clearish(s_kind)) return;

    /* Subtle zenith darkening. Was a full-screen per-pixel radial scan with a
     * smooth_u8() float call per sample — ~80 ms, the dominant cost of the
     * background rebake and the real cause of the once-a-second freeze on
     * clear/sunny skies. The darkening is overwhelmingly vertical (the centre
     * is near the top), so a per-ROW gradient looks the same and costs ~480
     * float ops instead of ~96k. Each row is a single solid-alpha span. */
    const int cy = EVA_WEATHER_RENDER_H / 5;
    const int span = EVA_WEATHER_RENDER_H;          /* vertical falloff scale */
    const uint16_t zenith = rgb565(7, 37, 92);
    for (int y = 0; y < EVA_WEATHER_RENDER_H; ++y) {
        int dy = y - cy;
        float d = (float)(dy * dy) / (float)(span * span);
        if (d > 1.0f) d = 1.0f;
        uint8_t a = smooth_u8(1.0f - d, 42);
        if (!a) continue;
        uint16_t *row = &s_buf[y * EVA_WEATHER_RENDER_W];
        if (s_sun_pos.warmth > 0.01f) {
            for (int x = 0; x < EVA_WEATHER_RENDER_W; ++x) {
                row[x] = blend565(row[x], zenith, a);
            }
        } else {
            uint16_t blended = blend565(row[0], zenith, a);
            for (int x = 0; x < EVA_WEATHER_RENDER_W; ++x) {
                row[x] = blended;
            }
        }
    }
}

static void draw_sun_sky_glare(void)
{
    if (!s_sun_visible || s_sun_strength <= 0.0f || is_night_kind(s_kind)) return;

    /* Glare radius capped on the Fibonacci ladder (was 260-350 px — a
     * 700×700 per-pixel scan that cost ~97 ms and was the single cause of
     * the ~once-a-second freeze whenever the sun was visible). FIB_89 keeps
     * the bright sky-glow tight around the sun; the area shrinks ~15× so the
     * pass drops to a few ms. */
    int r = (int)((float)FIB_89 + 21.0f * s_sun_strength);
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

static void feather_strip_edges(uint8_t *a8, int w, int h, int fade_px)
{
    if (fade_px <= 0 || h < 2 * fade_px) return;
    for (int y = 0; y < fade_px; ++y) {
        float t = (float)y / (float)fade_px;
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

/* Random micro-puffs — breaks up the too-uniform look on the taller strip. */
static void bake_cloud_scatter(uint8_t *a8_light, uint8_t *a8_shadow, uint8_t *a8_core,
                               int w, int h, int count,
                               uint8_t peak_base, float peak_lo, float peak_hi)
{
    for (int i = 0; i < count; ++i) {
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             rndf(0.0f, (float)w),
                             rndf((float)h * 0.03f, (float)h * 0.97f),
                             rndf((float)FIB_8, (float)FIB_34),
                             rndf((float)FIB_3, (float)FIB_13 + 4.0f),
                             scaled_peak(peak_base, peak_lo, peak_hi),
                             rndf(-0.75f, 0.55f), rndf(0.04f, 0.35f));
    }
}

static void bake_strip_fallback_high(uint8_t *a8_light, uint8_t *a8_shadow,
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
    for (int i = 0; i < FIB_8; ++i) {
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             rndf(0.0f, (float)w),
                             rndf((float)h * 0.04f, (float)h * 0.96f),
                             rndf((float)FIB_55, (float)FIB_144),
                             rndf((float)FIB_2, (float)FIB_13),
                             scaled_peak(FIB_89, 0.50f, 0.95f),
                             rndf(-0.65f, 0.15f), rndf(0.06f, 0.22f));
    }
    for (int i = 0; i < FIB_5; ++i) {
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             rndf(0.0f, (float)w),
                             rndf((float)h * 0.18f, (float)h * 0.78f),
                             rndf((float)FIB_89, (float)FIB_144),
                             rndf((float)FIB_2, (float)FIB_8),
                             scaled_peak(FIB_89, 0.45f, 0.85f),
                             -0.55f, 0.10f);
    }
    for (int i = 0; i < FIB_5; ++i) {
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             rndf(0.0f, (float)w),
                             rndf((float)h * 0.02f, (float)h * 0.22f),
                             rndf((float)FIB_55, (float)FIB_89),
                             rndf((float)FIB_5, (float)FIB_13),
                             scaled_peak(FIB_89, 0.55f, 0.95f),
                             -0.45f, 0.12f);
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             rndf(0.0f, (float)w),
                             rndf((float)h * 0.78f, (float)h * 0.98f),
                             rndf((float)FIB_55, (float)FIB_89),
                             rndf((float)FIB_5, (float)FIB_13),
                             scaled_peak(FIB_89, 0.55f, 0.95f),
                             -0.45f, 0.12f);
    }
    bake_cloud_scatter(a8_light, a8_shadow, a8_core, w, h, FIB_13, FIB_55, 0.35f, 0.75f);
}

static void bake_strip_fallback_mid(uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core, int w, int h)
{
    memset(a8_light, 0, w * h);
    memset(a8_shadow, 0, w * h);
    memset(a8_core, 0, w * h);
    /* Mid layer: broken cumulus — denser anchors + random satellites. */
    for (int i = 0; i < FIB_8 + FIB_3; ++i) {
        float cx = rndf(0.0f, (float)w);
        float cy = rndf((float)h * 0.05f, (float)h * 0.92f);
        float base_rx = rndf((float)FIB_34, (float)FIB_55 + rndf(0.0f, 18.0f));
        float mass = rndf(0.82f, 1.22f);
        blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                             cx, cy + rndf(-8.0f, 18.0f),
                             base_rx * mass, rndf((float)FIB_8, (float)FIB_21) * mass,
                             scaled_peak(FIB_144 + FIB_34, 0.66f, 1.08f),
                             rndf(0.35f, 0.85f), rndf(0.55f, 0.95f));
        int sub_lo = (int)rndf(2.0f, (float)(FIB_3 + FIB_1));
        int sub_hi = (int)rndf((float)(FIB_5 + FIB_1), (float)(FIB_8 + FIB_1));
        for (int j = 0; j < sub_lo + (i & 1); ++j) {
            blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                                 cx + rndf(-base_rx * 0.75f, base_rx * 0.75f),
                                 cy + rndf(-(float)FIB_34, (float)FIB_13),
                                 rndf((float)FIB_13, (float)FIB_34 + 10.0f) * mass,
                                 rndf((float)FIB_8, (float)FIB_21) * mass,
                                 scaled_peak(FIB_233 - FIB_21, 0.72f, 1.12f),
                                 rndf(-0.65f, 0.25f), rndf(0.22f, 0.48f));
        }
        for (int j = 0; j < sub_hi - sub_lo; ++j) {
            blob_gaussian_triple(a8_light, a8_shadow, a8_core, w, h,
                                 cx + rndf(-base_rx * 0.85f, base_rx * 0.85f),
                                 cy + rndf(-(float)FIB_55, (float)FIB_8),
                                 rndf((float)FIB_5, (float)FIB_13 + 4.0f),
                                 rndf((float)FIB_5, (float)FIB_13 + 2.0f),
                                 scaled_peak(FIB_89, 0.30f, 0.78f),
                                 rndf(-0.85f, -0.35f), rndf(0.04f, 0.18f));
        }
    }
    bake_cloud_scatter(a8_light, a8_shadow, a8_core, w, h, FIB_21, FIB_89, 0.40f, 0.85f);
}

static void bake_strip_fallback_low(uint8_t *a8_light, uint8_t *a8_shadow,
                           uint8_t *a8_core, int w, int h)
{
    memset(a8_light, 0, w * h);
    memset(a8_shadow, 0, w * h);
    memset(a8_core, 0, w * h);
    /* Low layer: larger cumulus masses with dark bellies and torn bright
     * tops. These are the shapes that should echo the user's real reference
     * photo: strong volume, broken edge, many different scales. */
    for (int i = 0; i < FIB_5 + FIB_3; ++i) {
        float cx = rndf(0.0f, (float)w);
        float cy = rndf((float)h * 0.06f, (float)h * 0.94f);
        float mass = rndf(0.68f, 1.28f);
        float base_rx = rndf((float)FIB_34 + 8.0f, (float)FIB_89 + rndf(0.0f, 16.0f)) * mass;
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
    bake_cloud_scatter(a8_light, a8_shadow, a8_core, w, h, FIB_13, FIB_144, 0.45f, 0.90f);
}

static void bake_strip_fallback_for_layer(int layer, cloud_variant_t *v, int h)
{
    if (!v || !v->a8_light || !v->a8_shadow || !v->a8_core) return;
    if (layer == CLOUD_LAYER_HIGH) {
        bake_strip_fallback_high(v->a8_light, v->a8_shadow, v->a8_core,
                        CLOUD_STRIP_W, h);
    } else if (layer == CLOUD_LAYER_MID) {
        bake_strip_fallback_mid(v->a8_light, v->a8_shadow, v->a8_core,
                       CLOUD_STRIP_W, h);
    } else {
        bake_strip_fallback_low(v->a8_light, v->a8_shadow, v->a8_core,
                       CLOUD_STRIP_W, h);
    }
    /* Softens only the far off-screen strip margins — not the viewport band. */
    feather_strip_edges(v->a8_light,  CLOUD_STRIP_W, h, FIB_8);
    feather_strip_edges(v->a8_shadow, CLOUD_STRIP_W, h, FIB_8);
    feather_strip_edges(v->a8_core,   CLOUD_STRIP_W, h, FIB_8);
}

static void scan_variant_content_rows(cloud_variant_t *v, int strip_h)
{
    v->content_y0 = 0;
    v->content_y1 = (uint16_t)strip_h;
    {
        int y0 = -1, y1 = -1;
        for (int y = 0; y < strip_h; ++y) {
            const uint8_t *row = &v->a8_light[y * CLOUD_STRIP_W];
            bool nz = false;
            for (int x = 0; x < CLOUD_STRIP_W; x += 4) {  /* stride-4 probe */
                if (row[x]) { nz = true; break; }
            }
            if (nz) { if (y0 < 0) y0 = y; y1 = y; }
        }
        if (y0 >= 0) {
            v->content_y0 = (uint16_t)(y0 > 2 ? y0 - 2 : 0);
            v->content_y1 = (uint16_t)((y1 + 3 < strip_h) ? y1 + 3
                                                           : strip_h);
        } else {
            v->content_y1 = v->content_y0;   /* empty mask — skip blends */
        }
    }
}

/* Per-layer depth-breathing scale ranges (spec §5). */
static const float k_depth_lo[CLOUD_LAYER_COUNT] = {
    [CLOUD_LAYER_HIGH] = 0.95f, [CLOUD_LAYER_MID] = 0.90f, [CLOUD_LAYER_LOW] = 0.85f,
};
static const float k_depth_hi[CLOUD_LAYER_COUNT] = {
    [CLOUD_LAYER_HIGH] = 1.05f, [CLOUD_LAYER_MID] = 1.15f, [CLOUD_LAYER_LOW] = 1.25f,
};

/* Pick the next pool variant: never the active one, and avoid the previous
 * one when the pool is big enough (no A→B→A flicker). */
static int pick_pool_variant(const cloud_strip_t *strip, cloud_pool_t pool, int count)
{
    if (count <= 1) return 0;
    for (int guard = 0; guard < 16; ++guard) {
        int idx = (int)(rndf(0.0f, (float)count - 0.001f));
        if (idx == strip->pool_cur && pool == (cloud_pool_t)strip->pool_kind) continue;
        if (count >= 3 && idx == strip->pool_prev &&
            pool == (cloud_pool_t)strip->pool_kind) continue;
        return idx;
    }
    return (strip->pool_cur + 1) % count;
}

static bool merged_storm_available(void)
{
    return s_cloud_assets_ok &&
           eva_cloud_assets_count(0, CLOUD_POOL_STORM_MERGED) > 0;
}

static bool use_merged_storm_layers(void)
{
    return (s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_HEAVY_RAIN) &&
           merged_storm_available();
}

static cloud_pool_t active_cloud_pool(int layer)
{
    if (layer == CLOUD_LAYER_HIGH) return CLOUD_POOL_NORMAL;
    if (use_merged_storm_layers()) {
        return CLOUD_POOL_STORM_MERGED;
    }
    if ((s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_HEAVY_RAIN) &&
        s_cloud_assets_ok &&
        eva_cloud_assets_count(layer, CLOUD_POOL_STORM) > 0) {
        return CLOUD_POOL_STORM;
    }
    return CLOUD_POOL_NORMAL;
}

/* Load the next pre-baked variant into `v`; falls back to the procedural
 * bake when the asset pool is unavailable or the file is bad. */
static void load_or_bake_variant(int layer, cloud_strip_t *strip,
                                 cloud_variant_t *v)
{
    cloud_pool_t pool = active_cloud_pool(layer);
    int asset_layer = layer;
    if (pool == CLOUD_POOL_STORM_MERGED || pool == CLOUD_POOL_STORM_LIT) {
        asset_layer = 0;
    }
    int count = s_cloud_assets_ok ? eva_cloud_assets_count(asset_layer, pool) : 0;
    if (count > 0) {
        int idx = pick_pool_variant(strip, pool, count);
        if (pool == CLOUD_POOL_STORM_LIT && use_merged_storm_layers()) {
            idx += 3;   /* lit merged variants follow base merged (genpool) */
            if (idx >= count) idx = count - 1;
        }
        bool mirror = rndf(0.0f, 1.0f) < 0.5f;
        float scale = rndf(k_depth_lo[layer], k_depth_hi[layer]);
        if (eva_cloud_assets_load(asset_layer, pool, idx,
                                  v->a8_light, v->a8_shadow, v->a8_core,
                                  CLOUD_STRIP_W, strip->strip_h,
                                  mirror, scale)) {
            strip->pool_kind = (uint8_t)pool;
            strip->pool_prev = strip->pool_cur;
            strip->pool_cur = (uint8_t)idx;
            strip->depth_scale = scale;
            strip->mirrored = mirror;
            scan_variant_content_rows(v, strip->strip_h);
            return;
        }
        ESP_LOGW(TAG, "cloud asset L%d %s v%d load failed — procedural fallback",
                 layer, pool == CLOUD_POOL_STORM ? "storm" : "normal", idx);
    }
    bake_strip_fallback_for_layer(layer, v, strip->strip_h);
    strip->pool_kind = (uint8_t)CLOUD_POOL_NORMAL;
    strip->depth_scale = 1.0f;
    strip->mirrored = false;
    scan_variant_content_rows(v, strip->strip_h);
}

static void wait_cloud_bake_idle(void)
{
    if (!s_bake_task) return;
    for (int spin = 0; spin < 5000; ++spin) {
        bool busy = false;
        for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
            if (s_strip[i].bake_state == BAKE_RUNNING) {
                busy = true;
                break;
            }
        }
        if (!busy) return;
        vTaskDelay(1);
    }
    ESP_LOGW(TAG, "cloud bake task still running during strip reinit");
}

static void cloud_bake_task(void *arg)
{
    (void)arg;
    uint32_t notify_bits = 0;
    for (;;) {
        xTaskNotifyWait(0, UINT32_MAX, &notify_bits, portMAX_DELAY);
        for (int layer = 0; layer < CLOUD_LAYER_COUNT; ++layer) {
            if ((notify_bits & (1U << layer)) == 0) continue;
            cloud_strip_t *strip = &s_strip[layer];
            if (strip->bake_state != BAKE_REQUESTED) continue;
            strip->bake_state = BAKE_RUNNING;
            int64_t t0 = esp_timer_get_time();
            load_or_bake_variant(layer, strip, &strip->variant[strip->bake_variant]);
            strip->bake_state = BAKE_DONE;
            ESP_LOGD(TAG, "BAKE layer %d took %lld us", layer,
                     (long long)(esp_timer_get_time() - t0));
        }
    }
}

static void ensure_cloud_bake_task(void)
{
    if (s_bake_task) return;
    if (xTaskCreatePinnedToCore(cloud_bake_task, "cloud_bake", 4096, NULL, 3,
                                &s_bake_task, 0) != pdPASS) {
        ESP_LOGE(TAG, "cloud bake task create failed");
        abort();
    }
}

static void init_cloud_strips(void)
{
    size_t total = 0;
    wait_cloud_bake_idle();
    static bool assets_probed;
    if (!assets_probed) {
        assets_probed = true;
        s_cloud_assets_ok = eva_cloud_assets_init();
        if (!s_cloud_assets_ok) {
            ESP_LOGW(TAG, "no pre-baked cloud assets — using procedural bake");
        }
    }
    /* Re-seed the canvas RNG once so the stable boot-seed doesn't lead to
     * mirror-symmetric cumulus placement (visible bug on first screenshots).
     * Using esp_timer_get_time() gives each boot a fresh distribution. */
    s_rng ^= (uint32_t)esp_timer_get_time();
    for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
        cloud_strip_t *strip = &s_strip[i];
        size_t bytes = (size_t)CLOUD_STRIP_W * strip->strip_h;
        for (int j = 0; j < CLOUD_VARIANT_COUNT; ++j) {
            cloud_variant_t *v = &strip->variant[j];
            if (v->a8_light) {
                heap_caps_free(v->a8_light);
                v->a8_light = NULL;
            }
            if (v->a8_shadow) {
                heap_caps_free(v->a8_shadow);
                v->a8_shadow = NULL;
            }
            if (v->a8_core) {
                heap_caps_free(v->a8_core);
                v->a8_core = NULL;
            }
            v->a8_light = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, bytes,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            v->a8_shadow = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, bytes,
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            v->a8_core = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, bytes,
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!v->a8_light || !v->a8_shadow || !v->a8_core) {
                ESP_LOGE(TAG, "cloud strip %d variant %d alloc failed", i, j);
                abort();
            }
            load_or_bake_variant(i, strip, v);
            total += 3 * bytes;
        }
        strip->active_variant = 0;
        strip->morphing = false;
        strip->morph_t = 0.0f;
        strip->morph_clock = -rndf(0.0f, strip->morph_hold_s * 0.65f);
        strip->bake_state = BAKE_IDLE;
        strip->bake_variant = 0;
        strip->pool_prev = strip->pool_cur;
    }
    ensure_cloud_bake_task();
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

/* Single-pixel Bresenham for rain streaks — skips thickness nesting. */
static void draw_rain_streak(int x0, int y0, int x1, int y1, uint16_t color, uint8_t alpha)
{
    int dx = abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    const int w = EVA_WEATHER_RENDER_W;
    const int h = EVA_WEATHER_RENDER_H;
    for (;;) {
        if ((unsigned)x0 < (unsigned)w && (unsigned)y0 < (unsigned)h) {
            int idx = eva_sbuf_idx(x0, y0);
            if (alpha >= 240) {
                s_buf[idx] = color;
            } else {
                s_buf[idx] = blend565(s_buf[idx], color, alpha);
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
    /* Wind X for particles in VIEWER space. The panel runs through a 270° PPA
     * rotation that mirrors the buffer's horizontal axis, so a positive
     * buffer vx would slant rain the wrong way on screen. Negate so rain/snow
     * slant matches the on-screen cloud drift (which is corrected the same
     * way in advance_cloud_scroll). */
    float wind_vx = -s_wind_vx_bias;
    float size_z = 0.82f + 0.11f * z;
    float alpha_z = 0.80f + 0.08f * z;
    p->x = rndf(0.0f, EVA_WEATHER_RENDER_W - 1.0f);
    p->y = from_top ? rndf(-120.0f, -8.0f) : rndf(0.0f, EVA_WEATHER_RENDER_H - 1.0f);
    p->phase = rndf(0.0f, 6.28f);
    p->spin = rndf(-1.0f, 1.0f);
    switch (kind) {
    case P_RAIN:
        /* Outdoor rain (behind glass): free-fall streaks, much faster than
         * glass-slide droplets. vy ~480–900 px/s ≈ 8–20× glass terminal. */
    {
        float light = (s_density_scale < 0.70f) ? 0.68f : 1.0f;
        float heavy = (s_kind == WEATHER_HEAVY_RAIN || s_kind == WEATHER_THUNDERSTORM)
            ? 1.12f : 1.0f;
        p->vx = (wind_vx + rndf(-24.0f, 24.0f)) * speed_z;
        p->vy = rndf(480.0f, 900.0f) * speed_z * light * heavy;
        p->size = rndf(8.0f, 18.0f) * size_z * light;
        p->alpha = rndf(0.38f, 0.72f) * alpha_z * light;
        break;
    }
    case P_SNOW:
        p->vx = (wind_vx * 0.30f + rndf(-14.0f, 14.0f)) * speed_z;
        p->vy = rndf(22.0f, 68.0f) * speed_z;
        p->size = rndf(2.0f, 5.5f) * size_z;
        p->alpha = rndf(0.32f, 0.72f) * alpha_z;
        break;
    case P_HAIL:
        p->vx = (wind_vx * 0.22f + rndf(-30.0f, 30.0f)) * speed_z;
        p->vy = rndf(420.0f, 720.0f) * speed_z;
        p->size = rndf(3.0f, 6.5f) * size_z;
        p->alpha = rndf(0.82f, 1.0f) * alpha_z;
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
        if (s_precip_type == PRECIP_HAIL) {
            return (slot % 4 == 0) ? P_HAIL : P_RAIN;
        }
        return P_RAIN;
    case WEATHER_SNOW:
        return P_SNOW;
    case WEATHER_SLEET:
        return (slot % 3 == 0) ? P_RAIN : P_SNOW;
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
    if (s_wx_trans.active) {
        bool same_precip = (s_wx_trans.from_precip == s_wx_trans.to_precip);
        float ramp = eva_wx_precip_ramp(s_wx_trans.progress, same_precip);
        s_target = (uint16_t)((float)s_target * ramp + 0.5f);
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

static bool weather_kind_has_precip_particles(weather_kind_t kind)
{
    return kind == WEATHER_RAIN || kind == WEATHER_HEAVY_RAIN ||
           kind == WEATHER_THUNDERSTORM || kind == WEATHER_SLEET ||
           kind == WEATHER_HAIL || kind == WEATHER_SNOW;
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
    /* Continuous sin arc: 0 at sunrise/sunset (progress 0/1), 1 at noon,
     * negative below the horizon when progress is outside [0,1] (glide zones). */
    return sinf(progress * 3.1415926f);
}

static float set_glide_minutes(void)
{
    float half_tw = civil_twilight_minutes() * 0.5f;
    if (half_tw > SET_GLIDE_MIN) half_tw = SET_GLIDE_MIN;
    if (half_tw < 10.0f) half_tw = 10.0f;
    return half_tw;
}

static float sun_apex_y_seasonal(void)
{
    float decl = solar_declination_deg();
    float noon_alt = 90.0f - fabsf(EVA_OBSERVER_LAT_DEG - decl);
    const float ALT_MIN = 17.0f;
    const float ALT_MAX = 65.0f;
    float t = (noon_alt - ALT_MIN) / (ALT_MAX - ALT_MIN);
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    return SUN_APEX_Y_WINTER - t * (SUN_APEX_Y_WINTER - SUN_APEX_Y_SUMMER);
}

/* Arc progress with ±glide extension so the luminary can sink below the
 * screen edge instead of popping off at the horizon. Returns -999 if hidden. */
static float arc_progress_glide(int rise, int set, int now_min, float glide_min)
{
    if (rise < 0 || set < 0) return -999.0f;
    int duration = set - rise;
    if (duration <= 0) duration += 1440;
    int glide_i = (int)(glide_min + 0.5f);

    int elapsed = now_min - rise;
    if (set < rise) {
        if (now_min >= rise) {
            /* evening after rise */
        } else if (now_min <= set) {
            elapsed = now_min - rise + 1440;
        } else {
            int past_set = now_min - set;
            if (past_set <= glide_i) {
                elapsed = duration + past_set;
            } else {
                int before_rise = rise - now_min;
                if (before_rise <= glide_i) {
                    elapsed = -before_rise;
                } else {
                    return -999.0f;
                }
            }
        }
    } else if (elapsed < -glide_i || elapsed > duration + glide_i) {
        return -999.0f;
    }

    if (elapsed < -glide_i || elapsed > duration + glide_i) {
        return -999.0f;
    }
    return (float)elapsed / (float)duration;
}

static void luminary_pos_from_progress(float progress, float apex_y, luminary_pos_t *out)
{
    float arc = sun_curve(progress);
    out->elevation = arc;
    out->x_n = 0.06f + progress * 0.88f;
    out->y_n = SUN_HORIZON_Y - arc * (SUN_HORIZON_Y - apex_y);
    if (arc <= 0.0f) {
        out->warmth = 0.0f;
    } else {
        float horizon = 1.0f - arc;
        out->warmth = horizon * horizon * 0.82f;
    }
}

static void luminary_pos_clear(luminary_pos_t *out)
{
    out->x_n = 0.5f;
    out->y_n = SUN_HORIZON_Y;
    out->elevation = -1.0f;
    out->warmth = 0.0f;
    out->valid = false;
}

static void compute_luminary_positions(int m)
{
    luminary_pos_clear(&s_sun_pos);
    luminary_pos_clear(&s_moon_pos);

    /* Precipitation scenes hide the sun/moon disc entirely (see
     * draw_sun_or_moon), so there is no luminary to place and no warm glow to
     * pool around it — an overcast rainy sky has no visible sun. Leaving
     * warmth=0 here keeps fill_sky() on its cheap flat-fill path instead of the
     * per-pixel radial blend, which was rebaking ~11 ms every background hold
     * and causing the once-a-second hitch in rain-with-clouds. */
    if (weather_kind_has_precip_particles(s_kind)) {
        return;   /* valid=false, warmth=0: flat sky, no disc */
    }

    float glide = set_glide_minutes();
    float apex_y = sun_apex_y_seasonal();

    if (s_moonrise_min >= 0 && s_moonset_min >= 0) {
        float moon_progress = arc_progress_glide(s_moonrise_min, s_moonset_min, m, glide);
        if (moon_progress > -900.0f) {
            s_moon_pos.valid = true;
            luminary_pos_from_progress(moon_progress, apex_y, &s_moon_pos);
            s_moon_pos.warmth = 0.0f;
        }
    }

    int sr, ss;
    sun_events(&sr, &ss);
    float daylight = (float)(ss - sr);
    if (daylight <= 1.0f) daylight = 12.0f * 60.0f;

    /* Only show a fixed "believable daytime sun" when the wall clock is NOT
     * yet synced — otherwise we'd have no real time to place it by. Once the
     * clock is real, a *_DAY scene kind (e.g. open-meteo's is_day lagging the
     * actual sunset) must NOT pin the sun to mid-sky: follow the real time so
     * it sinks below the horizon after sunset like any other day. */
    bool clock_synced = (time(NULL) > 1700000000);
    bool forced_day = (s_kind == WEATHER_CLEAR_DAY || s_kind == WEATHER_PARTLY_CLOUDY_DAY);
    bool show_unsynced_sun = forced_day && !clock_synced;

    int glide_i = (int)(glide + 0.5f);
    bool in_window = (m >= sr - glide_i && m <= ss + glide_i);
    if (!in_window && !show_unsynced_sun) {
        return;   /* sun below horizon; moon may still be valid */
    }

    float progress;
    if (show_unsynced_sun && (m < sr || m >= ss)) {
        progress = 0.46f;   /* clock unsynced: park a plausible daytime sun */
    } else {
        progress = (float)(m - sr) / daylight;
    }

    s_sun_pos.valid = true;
    luminary_pos_from_progress(progress, apex_y, &s_sun_pos);
}

static float moon_sky_factor(int m, int sr, int ss)
{
    float n = eva_sky_nightness(m, sr, ss, civil_twilight_minutes());
    return 0.30f + 0.70f * n;
}

static float moon_phase_factor(void)
{
    return 0.35f + 0.65f * ((float)s_moon_phase_pct * 0.01f);
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
    s_sun_elevation = -1.0f;
    s_sun_x = -1;
    s_sun_y = -1;
    s_sun_r = 0;

    /* Any precipitation kind hides the sun/moon: if it's raining, sleeting,
     * snowing or hailing the sky is overcast enough that no luminary shows
     * through. (Previously only thunderstorm + heavy rain hid it, so plain
     * "rain" still drew a sun in a fully clouded sky — wrong.) Lightning is
     * the only light source during a thunderstorm. */
    bool precip_kind = (s_kind == WEATHER_THUNDERSTORM ||
                        s_kind == WEATHER_HEAVY_RAIN ||
                        s_kind == WEATHER_RAIN ||
                        s_kind == WEATHER_SLEET ||
                        s_kind == WEATHER_HAIL ||
                        s_kind == WEATHER_SNOW);
    if (precip_kind) return;

    /* Compute attenuation from cloud cover.
     *
     * Real-world behaviour we want: on a partly-cloudy day the sun is still
     * the dominant bright object — it shines at near-full strength until the
     * sky is genuinely solid. Clouds passing IN FRONT of it occlude it
     * locally (handled by the cloud layers drawn on top), but the disc's own
     * brightness shouldn't dim just because total cover is 50-70 %.
     *
     * So the disc holds near-full brightness up to ~78 % cover, then ramps
     * down quickly to nothing at ~98 % (true overcast). The soft halo/corona
     * fades earlier and faster (they wash out in hazy skies well before the
     * disc does). */
    float cover = sky_cover_fraction();
    if (cover >= 0.98f) return;            /* near-overcast: hide entirely */

    /* Disc: full strength until DISC_KNEE cover, then linear down to 0 at 98%. */
    const float DISC_KNEE = 0.78f;
    float vis_disc;
    if (cover <= DISC_KNEE) {
        vis_disc = 1.0f;                   /* bright sun through gaps */
    } else {
        vis_disc = 1.0f - (cover - DISC_KNEE) / (0.98f - DISC_KNEE);
        if (vis_disc < 0.0f) vis_disc = 0.0f;
    }
    /* Halo/corona: softer, fades from the very first clouds. */
    float vis_raw = 1.0f - (cover / 0.98f);
    float vis = vis_raw * vis_raw * (3.0f - 2.0f * vis_raw);

    int sr, ss;
    sun_events(&sr, &ss);
    int m = minutes_now();

    /* Below the horizon (glide zone, elevation<=0): the moon has set/not yet
     * risen — draw no disc. Without this, arc_progress_glide keeps the moon
     * "valid" for ~10 min past moonset and its faint daytime disc showed as a
     * ghost circle low in a corner (same bug fixed for the sun below). */
    if (s_moon_pos.valid && s_moon_pos.elevation > 0.0f) {
        int moon_x = (int)(EVA_WEATHER_RENDER_W * s_moon_pos.x_n);
        int moon_y = (int)(EVA_WEATHER_RENDER_H * s_moon_pos.y_n);
        float n = eva_sky_nightness(m, sr, ss, civil_twilight_minutes());
        float sky_factor = moon_sky_factor(m, sr, ss);
        float phase_factor = moon_phase_factor();
        /* Daytime: keep the moon translucent and pale so it blends into the
         * bright sky (faint daytime moon). Nighttime: drive alpha toward fully
         * opaque so the warm disc reads as a real bright moon instead of a
         * washed-out grey — a translucent warm colour blended over the dark
         * night sky comes out muddy. So night opacity wins over the day fade. */
        float day_alpha = (float)FIB_233 * vis * sky_factor * phase_factor;
        float night_alpha = 255.0f * vis;          /* near-opaque lit disc at night */
        float a_f = day_alpha + (night_alpha - day_alpha) * n;
        uint8_t alpha_moon = (uint8_t)(a_f > 255.0f ? 255.0f : a_f);
        if (alpha_moon >= FIB_13) {
            eva_sprite_t moon;
            /* 32 baked phases (was 8): each phase `ph` represents illum
             * fraction (ph+1)/32, so its "center" is (ph+0.5)/32. Round to
             * the NEAREST phase instead of flooring — flooring always picks
             * the phase whose illum is >= pct, systematically overshooting
             * (found 2026-07-18: pct=32 floored to phase 1/8, whose 37.5%
             * illum read as visibly fuller than a real 32% crescent). With
             * 32 phases the round-to-nearest error is at most ~1.6pp. */
            float ph_f = ((float)s_moon_phase_pct / 100.0f) * (float)EVA_MOON_PHASE_COUNT - 0.5f;
            int ph = (int)(ph_f + 0.5f);
            if (ph < 0) ph = 0;
            if (ph > EVA_MOON_PHASE_COUNT - 1) ph = EVA_MOON_PHASE_COUNT - 1;
            if (eva_cloud_assets_sprite(EVA_CLP_TYPE_MOON, ph, 0, &moon)) {
                float night_k = n;
                for (int y = 0; y < moon.h; ++y) {
                    int dy = moon_y - moon.h / 2 + y;
                    if ((unsigned)dy >= EVA_WEATHER_RENDER_H) continue;
                    for (int x = 0; x < moon.w; ++x) {
                        int dx = moon_x - moon.w / 2 + x;
                        if ((unsigned)dx >= EVA_WEATHER_RENDER_W) continue;
                        /* Sprite phases are baked lit-LEFT (see gen_moon in
                         * tools/cloudgen/sprites.py). Northern-hemisphere
                         * convention, same as the draw_moon_phase fallback:
                         * waxing = lit side RIGHT, waning = lit LEFT. So
                         * WAXING mirrors the sprite, waning draws as stored.
                         * (Was inverted -> a young moon rendered as old.) */
                        int sx = s_moon_waning ? x : (moon.w - 1 - x);
                        uint8_t a = moon.plane[0][y * moon.w + sx];
                        if (a < FIB_3) continue;
                        uint8_t l = moon.plane[1][y * moon.w + sx];
                        uint8_t lr = (uint8_t)((l * (236 + (int)(19.0f * night_k))) / 255);
                        uint8_t lg = (uint8_t)((l * (236 + (int)(-1.0f * night_k))) / 255);
                        uint8_t lb = (uint8_t)((l * (226 + (int)(-56.0f * night_k))) / 255);
                        uint16_t c = rgb565(lr, lg, lb);
                        uint8_t blend_a = (uint8_t)(((uint16_t)a * alpha_moon) / 255U);
                        if (blend_a) {
                            int idx = eva_sbuf_idx(dx, dy);
                            s_buf[idx] = blend565(s_buf[idx], c, blend_a);
                        }
                    }
                }
            } else {
                rgb_t day_moon   = {225, 230, 235};
                rgb_t night_moon = {255, 235, 170};
                rgb_t moon_rgb = lerp_rgb(day_moon, night_moon, n);
                uint16_t moon_col = rgb565_from(moon_rgb);
                const int moon_r = (FIB_21 + FIB_8) * 2;
                draw_moon_phase(moon_x, moon_y, moon_r, moon_col, alpha_moon);
            }
        }
    }

    if (!s_sun_pos.valid) return;

    float arc = s_sun_pos.elevation;
    /* Below the horizon (glide zone, arc<=0): the sun has set — draw no disc.
     * The position stays valid so the warm afterglow (warmth, arc>0 only) can
     * still tint the sky during twilight, but the bright disc must not linger
     * on-screen after sunset. Without this the horizon_boost'd disc showed a
     * large low sun for ~10 min past the official sunset. */
    if (arc <= 0.0f) return;
    float horizon_boost = (arc > 0.0f) ? (1.0f - arc) : 0.0f;
    s_sun_elevation = arc;
    int sun_x = (int)(EVA_WEATHER_RENDER_W * s_sun_pos.x_n);
    int sun_y = (int)(EVA_WEATHER_RENDER_H * s_sun_pos.y_n);
    const int r = FIB_34 + FIB_8;
    uint8_t a_outer  = (uint8_t)((float)FIB_34 * vis * (0.65f + 0.35f * horizon_boost));
    uint8_t a_corona = (uint8_t)((float)FIB_55 * vis * (0.75f + 0.25f * horizon_boost));
    uint8_t a_glow   = (uint8_t)((float)FIB_89 * vis_disc * (0.85f + 0.15f * horizon_boost));
    uint8_t a_disc   = (uint8_t)((float)255 * vis_disc);
    if (a_outer)  draw_filled_circle(sun_x, sun_y, FIB_144, rgb565(255, 224, 148), a_outer);
    if (a_corona) draw_filled_circle(sun_x, sun_y, FIB_89,  rgb565(255, 210,  88), a_corona);
    if (a_glow)   draw_filled_circle(sun_x, sun_y, FIB_55,  rgb565(255, 228, 108), a_glow);
    if (a_disc)   draw_filled_circle(sun_x, sun_y, r,       rgb565(255, 236, 120), a_disc);

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
/* Lightweight sun halo: a few concentric Fibonacci-radius rings (8,13,21,34,
 * 55,89 px) whose alpha falls off with distance. Replaces the old god-ray
 * pass (a 216 px filled circle plus 8 marched beams) which scanned a huge
 * screen region every frame — ~60 ms on the CPU, the real FPS killer. The
 * ring set tops out at 89 px so the painted area is tiny and cheap, while
 * still giving the sun a soft warm glow that bleeds onto nearby clouds.
 * Rings go outer→inner so the brighter inner ones overpaint. */
/* Animated outer halo — Fibonacci radii/periods, painted every frame on s_buf
 * (sky cache holds the static disc+glow from draw_sun_or_moon). Cheap filled
 * circles + FIB_8 short rays; no full-frame pixel scan. */
static void blit_bolt_plane(const uint8_t *plane, int w, int h,
                            int x0, int y0, bool mirror,
                            uint16_t tint, uint8_t scale);

static uint16_t s_ray_tint_565(void)
{
    return rgb565(255, 246, 214);
}

static uint8_t s_ray_alpha(void)
{
    float cover = sky_cover_fraction();
    float k = (1.0f - cover * 0.58f) * s_sun_strength;
    if (k < 0.18f) k = 0.18f;
    return (uint8_t)(48.0f * k);
}

static void draw_sun_fib_light(float t)
{
    if (!s_sun_visible || s_sun_r <= 0) return;
    if (is_night_kind(s_kind)) return;
    if (s_kind == WEATHER_THUNDERSTORM || s_kind == WEATHER_HEAVY_RAIN) return;

    /* Smooth ray animation: instead of snapping between the 4 sprite phases
     * (the old `(t*1.6)&3` jumped every ~0.6 s — visibly janky), crossfade
     * between the current phase and the next. `pf` runs continuously; its
     * integer part selects the phase, its fraction blends into the next one,
     * so the fan breathes/rotates smoothly. The 4 phases are near-identical
     * (2° rotation + slight gain), so the blend reads as gentle shimmer. */
    float pf = t * 0.55f;                    /* slow, ~0.09 Hz per full cycle */
    int phase = (int)pf & 3;
    int next = (phase + 1) & 3;
    float f = pf - floorf(pf);               /* 0..1 crossfade weight */
    uint16_t tint = s_ray_tint_565();
    uint8_t base_a = s_ray_alpha();
    eva_sprite_t ray0, ray1;
    if (eva_cloud_assets_sprite(EVA_CLP_TYPE_RAY, phase, 0, &ray0) &&
        eva_cloud_assets_sprite(EVA_CLP_TYPE_RAY, next, 0, &ray1)) {
        int cx = s_sun_x - ray0.w / 2;
        int cy = s_sun_y - ray0.h / 2;
        uint8_t a0 = (uint8_t)((float)base_a * (1.0f - f));
        uint8_t a1 = (uint8_t)((float)base_a * f);
        if (a0) blit_bolt_plane(ray0.plane[0], ray0.w, ray0.h, cx, cy, false, tint, a0);
        if (a1) blit_bolt_plane(ray1.plane[0], ray1.w, ray1.h, cx, cy, false, tint, a1);
        return;
    }

    float cover = sky_cover_fraction();
    float k = (1.0f - cover * 0.58f) * s_sun_strength;
    if (k < 0.18f) k = 0.18f;

    static const struct { int r; float base_a; int freq_n; int freq_d; float phase; } rings[] = {
        { FIB_233, 5.0f,  FIB_2, FIB_144, 0.0f },
        { FIB_144, 8.5f,  FIB_3, FIB_89,  (float)FIB_5 / (float)FIB_3 },
        { FIB_89,  12.0f, FIB_5, FIB_55,  (float)FIB_8 / (float)FIB_5 },
    };
    const uint16_t warm = rgb565(255, 226, 168);
    const uint16_t pale = rgb565(255, 244, 210);
    for (size_t i = 0; i < sizeof(rings) / sizeof(rings[0]); ++i) {
        float pulse = 0.50f + 0.50f * sinf(t * (float)rings[i].freq_n / (float)rings[i].freq_d
                                            + rings[i].phase);
        uint8_t a = (uint8_t)(rings[i].base_a * pulse * k);
        if (a < FIB_3) continue;
        draw_filled_circle(s_sun_x, s_sun_y, rings[i].r,
                           (i == 0) ? pale : warm, a);
    }

    /* FIB_8 soft rays — slow rotation (period ~FIB_377/FIB_2 s), twinkle per ray. */
    const float rot = t * (float)FIB_2 / (float)FIB_377;
    const float step = 6.2831853f / (float)FIB_8;
    for (int i = 0; i < FIB_8; ++i) {
        float ray_pulse = 0.38f + 0.62f * sinf(t * (float)FIB_3 / (float)FIB_89
                                               + (float)i * step);
        if (ray_pulse < 0.40f) continue;
        float ang = rot + (float)i * step;
        int len = FIB_34 + (int)((float)FIB_21 * ray_pulse);
        int ex = s_sun_x + (int)(cosf(ang) * (float)len);
        int ey = s_sun_y + (int)(sinf(ang) * (float)len);
        uint8_t a = (uint8_t)((float)FIB_13 * ray_pulse * k);
        if (a < FIB_2) continue;
        draw_rain_streak(s_sun_x, s_sun_y, ex, ey, warm, a);
    }
}

static void draw_sun_god_rays(float t)
{
    draw_sun_fib_light(t);
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
        s_cloud_atlas[i].a8_data = (uint8_t *)heap_caps_aligned_alloc(PPA_CACHE_ALIGN, CLOUD_SPRITE_BYTES,
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
        if (night) {
            set_tint(&s_strip[CLOUD_LAYER_HIGH], 118, 120, 124,  72,  74,  78,  58,  60,  64);
            set_tint(&s_strip[CLOUD_LAYER_MID],  104, 106, 110,  58,  60,  64,  46,  48,  52);
            set_tint(&s_strip[CLOUD_LAYER_LOW],   92,  94,  98,  46,  48,  52,  36,  38,  42);
        } else {
            /* Fog: low contrast between light/shadow — fog is omnidirectional
             * scattering, no clear sun direction. Both tints are warm-grey. */
            set_tint(&s_strip[CLOUD_LAYER_HIGH], 232, 232, 226, 195, 195, 190, 178, 178, 172);
            set_tint(&s_strip[CLOUD_LAYER_MID],  220, 218, 212, 175, 175, 170, 155, 155, 150);
            set_tint(&s_strip[CLOUD_LAYER_LOW],  208, 208, 202, 158, 158, 154, 136, 136, 132);
        }
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
        /* Day: cloud lighting tracks the sun's elevation.
         *
         * High sun (midday): light hits the cloud tops straight down, so the
         * lit pass is near-white and the undersides catch cool sky-blue
         * scatter — bright, high-contrast cumulus.
         *
         * Low sun (sunrise/sunset): the sun grazes the clouds from the side,
         * so the "lit" face is a warm muted orange/pink rather than white,
         * the light is dimmer overall, and shadows go warmer-dark. This is
         * why clouds at dawn/dusk are never bright white — there is no
         * top-down light reaching them.
         *
         * elev in [0,1]: 0 = horizon (warm low light), 1 = apex (white top
         * light). Below the horizon s_sun_elevation is -1 → treat as 0. */
        float elev = s_sun_elevation;
        if (elev < 0.0f) elev = 0.0f;
        /* Ease so most of the day looks "high sun" and the warm low-angle
         * look only takes over in the last/first ~hour near the horizon. */
        float hi = elev * elev * (3.0f - 2.0f * elev);   /* smoothstep */

        /* Per-layer endpoint tints: {light}, {shadow}, {core}.
         * _lo = grazing low-sun (warm, dim), _hi = overhead (white, cool). */
        /* HIGH cirrus */
        rgb_t h_l_lo = {255, 196, 150}, h_l_hi = {255, 255, 252};
        rgb_t h_s_lo = {150, 120, 130}, h_s_hi = {205, 216, 232};
        rgb_t h_c_lo = {120,  92, 104}, h_c_hi = {170, 184, 204};
        /* MID altocumulus */
        rgb_t m_l_lo = {255, 180, 132}, m_l_hi = {255, 255, 252};
        rgb_t m_s_lo = {120,  92, 104}, m_s_hi = {160, 176, 198};
        rgb_t m_c_lo = { 92,  68,  82}, m_c_hi = {112, 130, 158};
        /* LOW cumulus */
        rgb_t l_l_lo = {255, 168, 120}, l_l_hi = {255, 255, 252};
        rgb_t l_s_lo = { 96,  72,  86}, l_s_hi = {118, 132, 156};
        rgb_t l_c_lo = { 64,  48,  62}, l_c_hi = { 70,  84, 108};

        rgb_t hL = lerp_rgb(h_l_lo, h_l_hi, hi), hS = lerp_rgb(h_s_lo, h_s_hi, hi), hC = lerp_rgb(h_c_lo, h_c_hi, hi);
        rgb_t mL = lerp_rgb(m_l_lo, m_l_hi, hi), mS = lerp_rgb(m_s_lo, m_s_hi, hi), mC = lerp_rgb(m_c_lo, m_c_hi, hi);
        rgb_t lL = lerp_rgb(l_l_lo, l_l_hi, hi), lS = lerp_rgb(l_s_lo, l_s_hi, hi), lC = lerp_rgb(l_c_lo, l_c_hi, hi);

        set_tint(&s_strip[CLOUD_LAYER_HIGH], hL.r, hL.g, hL.b, hS.r, hS.g, hS.b, hC.r, hC.g, hC.b);
        set_tint(&s_strip[CLOUD_LAYER_MID],  mL.r, mL.g, mL.b, mS.r, mS.g, mS.b, mC.r, mC.g, mC.b);
        set_tint(&s_strip[CLOUD_LAYER_LOW],  lL.r, lL.g, lL.b, lS.r, lS.g, lS.b, lC.r, lC.g, lC.b);
    }

    for (int i = 0; i < CLOUD_LAYER_COUNT; ++i) {
        uint8_t pct = s_cloud_pct[i];
        /* Map cloud_pct (0..100) to alpha_scale (0..254). 254 = FIB_233 + FIB_21
         * keeps the alpha ladder on the Fibonacci grid. The +50 in the
         * numerator is rounding (half of 100). */
        uint16_t max_alpha = (uint16_t)(FIB_233 + FIB_21);
        if (!stormy && !foggy && !night && pct > 0) {
            max_alpha = 255;
        } else if (night && !stormy && !foggy && pct > 0) {
            max_alpha = (uint16_t)((max_alpha * 72) / 100);
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
    /* Direction of horizontal scroll follows the wind.
     *
     * IMPORTANT sign note: the blend samples mask[scroll + x] for screen
     * column x, so increasing scroll_x makes the cloud texture appear to
     * move LEFT (west). To make the on-screen drift match the wind, the
     * scroll increment must be the OPPOSITE sign of the wind's screen-x:
     * Sign verified on hardware: the panel is driven through a 270° PPA
     * rotation, which mirrors the buffer's horizontal axis on the viewer's
     * screen. So to make the VIEWER see clouds drift with the wind:
     *   wind blows east (vx_bias > 0) → viewer sees rightward drift → scroll_x ↑
     *   wind blows west (vx_bias < 0) → viewer sees leftward  drift → scroll_x ↓
     * The ±4 px/s (~2 kph) dead-band avoids jitter when live wind dithers
     * around zero; in calm air we keep a gentle default drift. */
    float dir_sign = 1.0f;                        /* calm default: gentle eastward drift */
    if (s_wind_vx_bias > 4.0f) dir_sign = 1.0f;   /* east wind → viewer right */
    else if (s_wind_vx_bias < -4.0f) dir_sign = -1.0f; /* west wind → viewer left */

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
        float bob_amp = 0.50f + s_wind_kph_eff * 0.04f;
        if (bob_amp > (float)FIB_13) bob_amp = (float)FIB_13;
        float bob_freq = 0.32f + 0.07f * (float)i;
        s_strip[i].drift_y += dt * s_strip[i].drift_speed;
        /* Bounce softly inside the overflow margin so clouds never expose
         * the strip edge (±CLOUD_STRIP_OVERFLOW_Y/2 with feather margin). */
        float drift_lim = (float)CLOUD_STRIP_OVERFLOW_Y * 0.5f;
        if (s_strip[i].drift_y > drift_lim) {
            s_strip[i].drift_y = drift_lim;
            s_strip[i].drift_speed = -fabsf(s_strip[i].drift_speed);
        } else if (s_strip[i].drift_y < -drift_lim) {
            s_strip[i].drift_y = -drift_lim;
            s_strip[i].drift_speed = fabsf(s_strip[i].drift_speed);
        }
        s_strip[i].scroll_y_off = s_strip[i].drift_y +
            bob_amp * sinf(t_now * bob_freq + (float)i * EVA_PHI + s_strip[i].scroll_x * 0.002f);
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
                           int eff_y, int src_row0, int src_row1,
                           int src_x, int dst_x, int width,
                           bool bg_from_sky)
{
    if (!mask || width <= 0 || alpha_scale == 0) return;
    if (src_row0 >= src_row1) return;
    s_prof_blend_bands++;
    uint16_t tint = rgb565(tr, tg, tb);
    for (int y = src_row0; y < src_row1; ++y) {
        const uint8_t *src = &mask[y * CLOUD_STRIP_W + src_x];
        int dst_y = eff_y + y;
        if ((unsigned)dst_y >= EVA_WEATHER_RENDER_H) continue;
        const uint16_t *sky_row = (bg_from_sky && s_bg_buf)
            ? &s_bg_buf[dst_y * EVA_WEATHER_RENDER_W + dst_x] : NULL;
        for (int x = 0; x < width; ++x) {
            uint8_t a = (uint8_t)(((uint16_t)src[x] * alpha_scale) / 255);
            if (a) {
                int idx = eva_sbuf_idx(dst_x + x, dst_y);
                uint16_t base = sky_row ? sky_row[x] : s_buf[idx];
                if (a >= 240) {
                    s_buf[idx] = tint;
                } else {
                    s_buf[idx] = blend565(base, tint, a);
                }
            } else if (sky_row) {
                s_buf[eva_sbuf_idx(dst_x + x, dst_y)] = sky_row[x];
            }
        }
    }
}

static esp_err_t blend_mask_ppa_one_band(const cloud_strip_t *strip,
                                         const uint8_t *mask,
                                         uint8_t tr, uint8_t tg, uint8_t tb,
                                         uint8_t alpha_scale,
                                         int eff_y, int src_row0, int src_row1,
                                         int src_x, int dst_x, int width,
                                         bool bg_from_sky)
{
    if (!s_ppa_blend || s_ppa_blend_disabled || !mask || alpha_scale == 0 || width <= 0) {
        return ESP_FAIL;
    }
    if (src_row0 >= src_row1) return ESP_OK;
    s_prof_blend_bands++;
    /* PPA needs non-negative offsets and the band must fit in the working
     * buffer. If wind bob pushed us partially outside, clip the strip height
     * to the visible region. */
    int strip_top = eff_y + src_row0;
    int strip_h = src_row1 - src_row0;
    int mask_row_off = src_row0;
    if (strip_top < 0) {
        mask_row_off += -strip_top;
        strip_h -= -strip_top;
        strip_top = 0;
    }
    if (strip_top + strip_h > EVA_WEATHER_RENDER_H) {
        strip_h = EVA_WEATHER_RENDER_H - strip_top;
    }
    if (strip_h <= 0) return ESP_OK;   /* fully off-screen: skip silently */

    const bool use_sky_bg = bg_from_sky && s_bg_buf;
#ifdef EVA_PORTRAIT_NATIVE
    if (use_sky_bg) {
        return ESP_FAIL;   /* landscape sky cache → CPU path with idx transform */
    }
    int bg_ox, bg_oy, bg_bw, bg_bh;
    int fg_ox, fg_oy, fg_bw, fg_bh;
    eva_land_rect_to_port(dst_x, strip_top, width, strip_h,
                          &bg_ox, &bg_oy, &bg_bw, &bg_bh);
    eva_land_rect_to_port(src_x, mask_row_off, width, strip_h,
                          &fg_ox, &fg_oy, &fg_bw, &fg_bh);
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = (void *)s_buf,
            .pic_w = EVA_FB_PIC_W,
            .pic_h = EVA_FB_PIC_H,
            .block_w = (uint32_t)bg_bw,
            .block_h = (uint32_t)bg_bh,
            .block_offset_x = (uint32_t)bg_ox,
            .block_offset_y = (uint32_t)bg_oy,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = (void *)mask,
            .pic_w = CLOUD_STRIP_W,
            .pic_h = (uint32_t)strip->strip_h,
            .block_w = (uint32_t)fg_bw,
            .block_h = (uint32_t)fg_bh,
            .block_offset_x = (uint32_t)fg_ox,
            .block_offset_y = (uint32_t)fg_oy,
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer = s_buf,
            .buffer_size = EVA_FB_PIC_W * EVA_FB_PIC_H * sizeof(uint16_t),
            .pic_w = EVA_FB_PIC_W,
            .pic_h = EVA_FB_PIC_H,
            .block_offset_x = (uint32_t)bg_ox,
            .block_offset_y = (uint32_t)bg_oy,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .fg_alpha_update_mode = PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = (float)alpha_scale / 256.0f,
        .fg_fix_rgb_val = { .b = tb, .g = tg, .r = tr },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
#else
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = use_sky_bg ? (void *)s_bg_buf : (void *)s_buf,
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
            .block_offset_y = (uint32_t)mask_row_off,
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
#endif
    esp_err_t err = ppa_do_blend(s_ppa_blend, &cfg);
    if (err == ESP_OK && bg_from_sky && s_blend_from_sky) {
        s_blend_from_sky = false;
    }
    return err;
}

/* Composite one layer: shadow mask first (under the sky), light mask over.
 * Wraps the strip across the seam if scroll lands close to the edge.
 * `eff_y` accounts for the layer's wind-driven vertical bob — small offset
 * but stops the layer feeling locked into a fixed band. */
static int strip_eff_y(const cloud_strip_t *strip)
{
    return strip->y_start + (int)strip->scroll_y_off;
}

static void blend_layer_variant(cloud_strip_t *strip,
                                const cloud_variant_t *v,
                                uint8_t alpha_scale,
                                bool sky_wrap)
{
    if (!v || !v->a8_light || !v->a8_shadow || !v->a8_core || alpha_scale == 0) return;

    int src_row0 = (int)v->content_y0;
    int src_row1 = (int)v->content_y1;
    if (src_row0 >= src_row1) return;

    int scroll = (int)strip->scroll_x;
    if (scroll >= CLOUD_STRIP_W) scroll = 0;
    int max_src_w = CLOUD_STRIP_W - scroll;
    int first_w  = max_src_w >= EVA_WEATHER_RENDER_W ? EVA_WEATHER_RENDER_W : max_src_w;
    int second_w = EVA_WEATHER_RENDER_W - first_w;
    int eff_y = strip_eff_y(strip);

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

    /* 3-plane volume rendering (restored 2026-07-03): shadow (belly) →
     * core (dense centre) → light (sun-lit top), so the darker planes lie
     * UNDER the lit cap and clouds read as volumes, not flat white tufts.
     * Gated by s_cloud_volume so it can be A/B'd against light-only on the
     * device — 3× the PPA bands per layer, measured on hardware. */
    esp_err_t err = ESP_OK;

    /* --- shadow + core, only when volume rendering is on --------------- */
    if (s_cloud_volume) {
        if (shadow_alpha) {
            err = blend_mask_ppa_one_band(strip, v->a8_shadow,
                                           strip->tint_shadow_r,
                                           strip->tint_shadow_g,
                                           strip->tint_shadow_b,
                                           shadow_alpha,
                                           eff_y, src_row0, src_row1,
                                           scroll, 0, first_w,
                                           s_blend_from_sky);
            if (err == ESP_OK && second_w > 0) {
                err = blend_mask_ppa_one_band(strip, v->a8_shadow,
                                               strip->tint_shadow_r,
                                               strip->tint_shadow_g,
                                               strip->tint_shadow_b,
                                               shadow_alpha,
                                               eff_y, src_row0, src_row1,
                                               0, first_w, second_w, sky_wrap);
            }
        }
        if (err == ESP_OK && core_alpha) {
            err = blend_mask_ppa_one_band(strip, v->a8_core,
                                           strip->tint_core_r,
                                           strip->tint_core_g,
                                           strip->tint_core_b,
                                           core_alpha,
                                           eff_y, src_row0, src_row1,
                                           scroll, 0, first_w, false);
            if (err == ESP_OK && second_w > 0) {
                err = blend_mask_ppa_one_band(strip, v->a8_core,
                                               strip->tint_core_r,
                                               strip->tint_core_g,
                                               strip->tint_core_b,
                                               core_alpha,
                                               eff_y, src_row0, src_row1,
                                               0, first_w, second_w, false);
            }
        }
    } else {
        (void)shadow_alpha;
        (void)core_alpha;
    }

    /* --- light cap (always drawn, painted last so it sits on top) ------ */
    /* Cloud blend via PPA (hardware A8-over-RGB565). CPU blend was tried but
     * a full-width light mask is ~60 ms on the CPU — far worse than PPA even
     * with some engine contention against the SRM rotation. Falls back to CPU
     * only if the PPA call errors out. */
    if (err == ESP_OK && light_alpha) {
        err = blend_mask_ppa_one_band(strip, v->a8_light,
                                       strip->tint_light_r,
                                       strip->tint_light_g,
                                       strip->tint_light_b,
                                       light_alpha,
                                       eff_y, src_row0, src_row1,
                                       scroll, 0, first_w,
                                       s_cloud_volume ? false : s_blend_from_sky);
        if (err == ESP_OK && second_w > 0) {
            /* Wrap band sits right of the seam; s_buf there still holds last
             * frame's rain/text unless we read the cached sky as bg. */
            err = blend_mask_ppa_one_band(strip, v->a8_light,
                                           strip->tint_light_r,
                                           strip->tint_light_g,
                                           strip->tint_light_b,
                                           light_alpha,
                                           eff_y, src_row0, src_row1,
                                           0, first_w, second_w,
                                           s_cloud_volume ? false : sky_wrap);
        }
    }
    if (err == ESP_OK) return;

    if (s_ppa_blend && !s_ppa_blend_disabled) {
        ESP_LOGW(TAG, "PPA cloud blend failed once (err=%d), using CPU fallback", (int)err);
        s_ppa_blend_disabled = true;
    }
    if (light_alpha) {
        blend_mask_cpu(strip, v->a8_light,
                       strip->tint_light_r, strip->tint_light_g, strip->tint_light_b,
                       light_alpha,
                       eff_y, src_row0, src_row1,
                       scroll, 0, first_w, s_blend_from_sky);
        if (second_w > 0) {
            blend_mask_cpu(strip, v->a8_light,
                           strip->tint_light_r, strip->tint_light_g, strip->tint_light_b,
                           light_alpha,
                           eff_y, src_row0, src_row1,
                           0, first_w, second_w, sky_wrap);
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

        if (strip->bake_state == BAKE_DONE) {
            strip->bake_state = BAKE_IDLE;
        }

        strip->morph_clock += dt;
        if (!strip->morphing) {
            if (strip->bake_state != BAKE_IDLE) {
                continue;
            }
            bool another_morphing = false;
            for (int j = 0; j < CLOUD_LAYER_COUNT; ++j) {
                if (j != i && s_strip[j].morphing) { another_morphing = true; break; }
            }
            if (another_morphing) {
                /* Hold this layer's clock at the threshold — it starts as
                 * soon as the current crossfade finishes. */
                strip->morph_clock = strip->morph_hold_s;
            } else if (strip->morph_clock >= strip->morph_hold_s) {
                strip->morphing = true;
                strip->morph_t = 0.0f;
                strip->morph_clock = 0.0f;
            }
            continue;
        }

        float duration = strip->morph_duration_s > 1.0f ? strip->morph_duration_s : 1.0f;
        strip->morph_t += dt / duration;
        if (strip->morph_t >= 1.0f) {
            uint8_t hidden = strip->active_variant ^ 1U;
            strip->active_variant = hidden;
            strip->morphing = false;
            strip->morph_t = 0.0f;
            strip->morph_clock = 0.0f;
            s_rng ^= (uint32_t)esp_timer_get_time() ^ ((uint32_t)i << 24);
            strip->bake_variant = strip->active_variant ^ 1U;
            strip->bake_state = BAKE_REQUESTED;
            /* New variant, new gentle vertical drift (spec §5): ±8–20 px
             * over a 34–89 s lifecycle ≈ 0.10–0.35 px/s. */
            strip->drift_speed = rndf(0.10f, 0.35f) *
                                 (rndf(0.0f, 1.0f) < 0.5f ? -1.0f : 1.0f);
            if (s_bake_task) {
                xTaskNotify(s_bake_task, (uint32_t)(1U << i), eSetBits);
            }
        }
    }
}

static bool blend_layer(cloud_strip_t *strip, bool sky_wrap)
{
    /* Below this threshold the layer would only contribute imperceptible
     * pixels (alpha < FIB_8 ≈ 3 %) but still trigger 3 PPA blends. Skip it
     * so clear-day (cloud_pct~5) doesn't pay the full cloudy cost. */
    if (strip->alpha_scale < FIB_8) return false;

    uint8_t active = strip->active_variant;
    uint8_t next = active ^ 1U;
    if (!strip->morphing) {
        blend_layer_variant(strip, &strip->variant[active], strip->alpha_scale, sky_wrap);
        return true;
    }

    float t = strip->morph_t;
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    float eased = t * t * (3.0f - 2.0f * t);
    uint8_t a0 = alpha_scaled_by_float(strip->alpha_scale, 1.0f - eased);
    uint8_t a1 = alpha_scaled_by_float(strip->alpha_scale, eased);
    blend_layer_variant(strip, &strip->variant[active], a0, sky_wrap);
    blend_layer_variant(strip, &strip->variant[next], a1, sky_wrap);
    return true;
}

static void draw_sun_cloud_light_variant(const cloud_strip_t *strip,
                                         const cloud_variant_t *v,
                                         uint8_t alpha_scale)
{
    if (!s_sun_visible || s_sun_strength <= 0.0f || is_night_kind(s_kind)) return;
    if (!strip || !v || !v->a8_light || alpha_scale == 0) return;

    int eff_y = strip_eff_y(strip);
    int scroll = (int)strip->scroll_x;
    if (scroll >= CLOUD_STRIP_W) scroll = 0;

    /* Cloud-edge sunlight reaches only the clouds near the sun. The old
     * radius (260-340 px) swept almost half the screen — ~115k float-math
     * iterations per frame, one of the two big CPU costs. FIB_144 (+ a small
     * sun-strength term) keeps the lit halo tight around the sun where it's
     * actually visible, cutting the scanned area ~5×. */
    int r = (int)((float)FIB_233 + (float)FIB_34 * s_sun_strength);
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
        /* Skip the lit-edge pass for thin layers. Below this cover there are
         * too few cloud pixels for the rim light to read, yet the variant
         * scan still sweeps the whole sun region (Fib-144²) — pure waste that
         * dropped clear-day (cover ~8 %) to ~23 FPS. FIB_34 ≈ 13 % cover. */
        if (strip->alpha_scale < FIB_34) continue;
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

static void advance_cloud_frame(float dt)
{
    update_cloud_tints();
    update_cloud_lifecycle(dt);
    advance_cloud_scroll(dt);
}

static void compose_clouds_into_working_buffer(float dt)
{
    advance_cloud_frame(dt);
    bool sky_wrap = true;
    if (blend_layer(&s_strip[CLOUD_LAYER_HIGH], sky_wrap)) sky_wrap = false;
    if (blend_layer(&s_strip[CLOUD_LAYER_MID], sky_wrap)) sky_wrap = false;
    (void)blend_layer(&s_strip[CLOUD_LAYER_LOW], sky_wrap);
}

static bool blit_offline_a8_full(const uint8_t *mask, int mw, int mh,
                                 uint16_t tint, uint8_t alpha_scale,
                                 int scroll_x, int scroll_y)
{
    if (!mask || !s_ppa_blend || s_ppa_blend_disabled || alpha_scale == 0) {
        return false;
    }
    s_prof_blend_bands++;
    ppa_blend_oper_config_t cfg = {
        .in_bg = {
            .buffer = s_buf,
            .pic_w = EVA_WEATHER_RENDER_W,
            .pic_h = EVA_WEATHER_RENDER_H,
            .block_w = EVA_WEATHER_RENDER_W,
            .block_h = EVA_WEATHER_RENDER_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .in_fg = {
            .buffer = (void *)mask,
            .pic_w = (uint32_t)mw,
            .pic_h = (uint32_t)mh,
            .block_w = EVA_WEATHER_RENDER_W,
            .block_h = EVA_WEATHER_RENDER_H,
            .block_offset_x = (uint32_t)((scroll_x % mw + mw) % mw),
            .block_offset_y = (uint32_t)((scroll_y % mh + mh) % mh),
            .blend_cm = PPA_BLEND_COLOR_MODE_A8,
        },
        .out = {
            .buffer = s_buf,
            .buffer_size = EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
            .pic_w = EVA_WEATHER_RENDER_W,
            .pic_h = EVA_WEATHER_RENDER_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .blend_cm = PPA_BLEND_COLOR_MODE_RGB565,
        },
        .fg_alpha_update_mode = PPA_ALPHA_SCALE,
        .fg_alpha_scale_ratio = (float)alpha_scale / 256.0f,
        .fg_fix_rgb_val = { .b = tint & 0x1f, .g = (tint >> 5) & 0x3f, .r = (tint >> 11) & 0x1f },
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    uint8_t tb = (uint8_t)(tint & 0x1f);
    uint8_t tg = (uint8_t)((tint >> 5) & 0x3f);
    uint8_t tr = (uint8_t)((tint >> 11) & 0x1f);
    cfg.fg_fix_rgb_val.r = tr;
    cfg.fg_fix_rgb_val.g = tg;
    cfg.fg_fix_rgb_val.b = tb;
    return ppa_do_blend(s_ppa_blend, &cfg) == ESP_OK;
}

static void update_and_draw_particles(float dt, float t)
{
    ensure_particle_count();

    if (s_kind == WEATHER_FOG) {
        eva_sprite_t sp;
        if (eva_cloud_assets_sprite(EVA_CLP_TYPE_FOG, 0, 0, &sp) && sp.plane[0]) {
            s_fog_drift_x += (int)(dt * 18.0f);
            if (s_fog_drift_x >= (int)sp.w) s_fog_drift_x -= (int)sp.w;
            uint16_t col = is_night_kind(s_kind) ? rgb565(148, 152, 160)
                                                 : rgb565(220, 222, 216);
            (void)blit_offline_a8_full(sp.plane[0], sp.w, sp.h, col, 200,
                                       s_fog_drift_x, 0);
            return;
        }
    }

    if (weather_kind_has_precip_particles(s_kind) &&
        s_precip_type != PRECIP_SNOW && s_kind != WEATHER_SNOW &&
        s_kind != WEATHER_HAIL) {
        eva_sprite_t sp;
        int intensity = (s_kind == WEATHER_HEAVY_RAIN || s_kind == WEATHER_THUNDERSTORM) ? 1 : 0;
        int tilt = (s_wind_kph_eff > 24.0f) ? 1 : 0;
        s_rain_loop_t += dt;
        int frame = ((int)(s_rain_loop_t * 10.0f)) & 7;
        rgb_t rain_rgb = {
            .r = (uint8_t)(s_sky_bottom.r + (255 - s_sky_bottom.r) * 40 / 100),
            .g = (uint8_t)(s_sky_bottom.g + (255 - s_sky_bottom.g) * 40 / 100),
            .b = (uint8_t)(s_sky_bottom.b + (255 - s_sky_bottom.b) * 40 / 100),
        };
        uint16_t rain_col = rgb565_from(rain_rgb);
        if (eva_cloud_assets_sprite(EVA_CLP_TYPE_RAIN, intensity, tilt * 8 + frame, &sp) &&
            sp.plane[0]) {
            (void)blit_offline_a8_full(sp.plane[0], sp.w, sp.h, rain_col, 220, 0, frame * 6);
            return;
        }
    }

    /* Rain streak colour = the horizon sky lifted ~40 % toward white. A drop is
     * a lens showing a brighter slice of the sky behind it, so it tracks the
     * scene: pale steel-blue under a grey storm, near-charcoal at night. Cheaper
     * and more believable than the old fixed rgb565(214,234,255) that glowed
     * white against a dark sky. Computed once per frame, not per particle. */
    rgb_t rain_rgb = {
        .r = (uint8_t)(s_sky_bottom.r + (255 - s_sky_bottom.r) * 40 / 100),
        .g = (uint8_t)(s_sky_bottom.g + (255 - s_sky_bottom.g) * 40 / 100),
        .b = (uint8_t)(s_sky_bottom.b + (255 - s_sky_bottom.b) * 40 / 100),
    };
    uint16_t rain_col = rgb565_from(rain_rgb);

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
            /* Lower peak alpha (was 200) so streaks stay translucent — real
             * rain is a faint veil, not opaque white lines. */
            uint8_t alpha = clamp_u8((int)(p->alpha * 150.0f));
            int x0 = (int)p->x;
            int y0 = (int)p->y;
            /* Streak direction follows actual velocity, so wind from the
             * east tilts streaks /, west tilts them \. Streak length is
             * p->size; horizontal extent is size * (vx/vy) ratio. vy is
             * always > 0 for rain so no divide-by-zero. */
            float vy_safe = (p->vy > 1.0f) ? p->vy : 1.0f;
            float slen = p->size * (0.85f + fminf(p->vy / 900.0f, 0.35f));
            int x1 = x0 - (int)(slen * (p->vx / vy_safe));
            int y1 = y0 - (int)slen;
            if (p->size > 14.0f) {
                draw_line(x1, y1, x0, y0, rain_col, alpha, 1);
            } else {
                draw_rain_streak(x1, y1, x0, y0, rain_col, alpha);
            }
            break;
        }
        case P_SNOW: {
            float sway_amp = 20.0f + fminf(s_wind_kph_eff, 60.0f) * 0.30f;
            float sway = sinf(t * (0.35f + fabsf(p->spin) * 0.35f) + p->phase) * sway_amp;
            p->x += (p->vx + sway) * dt;
            p->y += p->vy * dt;
            p->phase += p->spin * dt;
            if (p->y > EVA_WEATHER_RENDER_H + 20.0f) {
                spawn_particle(p, P_SNOW, true, i);
            }
            if (p->x < -12.0f) p->x = EVA_WEATHER_RENDER_W + 8.0f;
            if (p->x > EVA_WEATHER_RENDER_W + 12.0f) p->x = -8.0f;
            uint8_t alpha = clamp_u8((int)(p->alpha * 180.0f));
            int x = (int)p->x;
            int y = (int)p->y;
            int r = (int)p->size;
            uint16_t col = rgb565(240, 246, 255);
            draw_filled_circle(x, y, r, col, alpha);
            if (r > 2) {
                draw_filled_circle(x, y, r - 1, rgb565(255, 255, 255),
                                   (uint8_t)(alpha * 3 / 5));
            }
            break;
        }
        case P_HAIL: {
            p->x += p->vx * dt;
            p->y += p->vy * dt;
            if (p->y > EVA_WEATHER_RENDER_H + 24.0f ||
                p->x < -20.0f || p->x > EVA_WEATHER_RENDER_W + 20.0f) {
                spawn_particle(p, P_HAIL, true, i);
            }
            int x = (int)p->x;
            int y = (int)p->y;
            int r = (int)p->size;
            uint8_t alpha = clamp_u8((int)(p->alpha * 240.0f));
            draw_filled_circle(x, y, r, rgb565(210, 222, 238), alpha);
            draw_filled_circle(x - 1, y - 1, r / 2 + 1, rgb565(255, 255, 255),
                               (uint8_t)(alpha * 4 / 5));
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
            uint16_t col = is_night_kind(s_kind) ? rgb565(148, 152, 160)
                                                 : rgb565(220, 222, 216);
            uint8_t alpha = clamp_u8((int)(p->alpha * (is_night_kind(s_kind) ? 120.0f : 180.0f)));
            draw_filled_circle((int)p->x, (int)p->y, (int)p->size, col, alpha);
            break;
        }
        default:
            break;
        }
    }
}

/* Real CG lightning: stepped leader channel + 3–4 return-stroke flickers (~40 ms
 * apart). Strike geometry is picked at random each flash — diagonal, vertical,
 * upward, intracloud — so bolts can cross the frame in many directions. */
static void clamp_lightning_xy(float *px, float *py, int w, int h)
{
    if (*px < 2.0f) *px = 2.0f;
    if (*px > (float)(w - 3)) *px = (float)(w - 3);
    if (*py < 2.0f) *py = 2.0f;
    if (*py > (float)(h - 3)) *py = (float)(h - 3);
}

static void generate_lightning_bolt(void)
{
    const int w = EVA_WEATHER_RENDER_W;
    const int h = EVA_WEATHER_RENDER_H;

    /* Rolled once per strike, before the sprite/polyline split, so ~10 % of
     * strikes are flash-only (no visible channel) on EITHER draw path. */
    s_lightning_sheet_only = (rndf(0.0f, 1.0f) < 0.10f);

    /* --- sprite path: pre-baked DIRECTIONAL branched bolt (preferred) ---
     * Direction is baked into the sprite (subtype). Pick a direction, a random
     * variant within it, then anchor the sprite so its strike origin lands in a
     * plausible screen region for that direction. No geometry is computed here;
     * the whole point of the offline bake is that the device only picks + blits.
     * Direction indices match BOLT_DIR_NAMES / EVA_BOLT_* constants in
     * eva_clp_toc.h: 0 down,1 down-left,2 down-right,3 up,4 up-left,
     * 5 up-right,6 intracloud. */
    int direction = (int)rndf(0.0f, (float)EVA_BOLT_DIRECTION_COUNT - 0.001f);
    int variant   = (int)rndf(0.0f, (float)EVA_BOLT_VARIANT_COUNT - 0.001f);
    s_bolt_sprite_ok = eva_cloud_assets_sprite(EVA_CLP_TYPE_BOLT, direction,
                                               variant, &s_bolt_sprite);
    if (s_bolt_sprite_ok) {
        /* Direction is already in the pixels — do NOT horizontal-mirror
         * (that would turn a down-left into a down-right and break the axis). */
        s_bolt_mirror = false;
        const int rw = EVA_WEATHER_RENDER_W;
        const int rh = EVA_WEATHER_RENDER_H;
        int sw = s_bolt_sprite.w, sh = s_bolt_sprite.h;
        /* Horizontal anchor: allow the sprite to span most of the frame width. */
        s_bolt_x = (int)rndf(-sw * 0.15f, (float)rw - sw * 0.85f);
        /* Vertical anchor by direction: 'up*' bolts (indices 3,4,5) originate
         * low, so pull the sprite DOWN so its bright base sits near the deck;
         * 'intracloud' (6) sits high; others (down*) hang from the top. */
        if (direction >= 3 && direction <= 5) {
            s_bolt_y = (int)rndf((float)rh - sh, (float)rh - sh * 0.75f);
        } else if (direction == 6) {
            s_bolt_y = (int)rndf(-20.0f, (float)rh * 0.10f);
        } else {
            s_bolt_y = (int)rndf(-40.0f, 20.0f);
        }
        /* Flash origin = the strike end of the channel for that direction.
         * For down* / intracloud the strike is near the sprite top; for up* the
         * strike (ground contact) is near the sprite bottom. */
        s_lightning_flash_x = s_bolt_x + sw / 2;
        s_lightning_flash_y = (direction >= 3 && direction <= 5)
                            ? s_bolt_y + (sh * 2) / 3
                            : s_bolt_y + sh / 3;
        return;
    }

    /* --- polyline fallback: ONLY when the pack has no bolt sprite ---
     * Minimal single top-down jittered channel; the rich directional look
     * lives entirely in the baked sprites now. Kept so a corrupt/missing pack
     * still renders a bolt instead of nothing. */
    float sx = w * rndf(0.30f, 0.70f);
    float sy = h * rndf(0.04f, 0.16f);
    float ex = sx + w * rndf(-0.18f, 0.18f);
    float ey = h * rndf(0.72f, 0.94f);
    float dx = ex - sx, dy = ey - sy;
    float seg_len = sqrtf(dx * dx + dy * dy);
    if (seg_len < 1.0f) seg_len = 1.0f;
    s_lightning_pt_count = LIGHTNING_PT_MAX;
    for (int i = 0; i < LIGHTNING_PT_MAX; ++i) {
        float u = (float)i / (float)(LIGHTNING_PT_MAX - 1);
        float px = sx + dx * u;
        float py = sy + dy * u;
        if (i > 0 && i < LIGHTNING_PT_MAX - 1) {
            float amp = ((1.0f - u) * (float)FIB_55 + (float)FIB_13) * 0.85f;
            px += rndf(-amp, amp);
            py += rndf(-amp * 0.35f, amp * 0.35f);
        }
        clamp_lightning_xy(&px, &py, w, h);
        s_lightning_x[i] = (int16_t)px;
        s_lightning_y[i] = (int16_t)py;
    }
    s_lightning_flash_x = (int16_t)sx;
    s_lightning_flash_y = (int16_t)sy;
    s_lightning_has_branch = false;
    s_lightning_branch_pts = 0;
}

/* Dart leaders reuse the channel — nudge interior points slightly on later strokes. */
static void nudge_lightning_channel(uint8_t stroke_idx)
{
    if (stroke_idx == 0 || s_lightning_sheet_only) return;

    const int w = EVA_WEATHER_RENDER_W;
    const int h = EVA_WEATHER_RENDER_H;
    for (int i = 1; i < LIGHTNING_PT_MAX - 1; ++i) {
        float px = (float)s_lightning_x[i] + rndf(-3.0f, 3.0f);
        float py = (float)s_lightning_y[i] + rndf(-1.8f, 1.8f);
        clamp_lightning_xy(&px, &py, w, h);
        s_lightning_x[i] = (int16_t)px;
        s_lightning_y[i] = (int16_t)py;
    }
    if (!s_lightning_has_branch) return;
    for (int j = 0; j < s_lightning_branch_pts; ++j) {
        float px = (float)s_lightning_bx[j] + rndf(-2.0f, 2.0f);
        float py = (float)s_lightning_by[j] + rndf(-1.2f, 1.2f);
        clamp_lightning_xy(&px, &py, w, h);
        s_lightning_bx[j] = (int16_t)px;
        s_lightning_by[j] = (int16_t)py;
    }
}

static float lightning_stroke_envelope(float age_s)
{
    if (age_s < 0.0f) return 0.0f;
    if (age_s < 0.006f) return age_s / 0.006f;
    if (age_s > 0.14f) return 0.0f;
    return expf(-(age_s - 0.006f) * 34.0f);
}

static void start_lightning_strike(float t)
{
    (void)t;
    generate_lightning_bolt();
    s_lightning_active = true;
    s_lightning_in_fade = false;
    s_lightning_strike_age = 0.0f;
    s_lightning_stroke_next = 0;
    s_lightning_stroke_total = (uint8_t)rndf(2.0f, 6.0f);
    if (s_lightning_stroke_total > LIGHTNING_STROKE_MAX) {
        s_lightning_stroke_total = LIGHTNING_STROKE_MAX;
    }

    float acc = rndf(0.015f, 0.045f);
    for (int i = 0; i < (int)s_lightning_stroke_total; ++i) {
        s_lightning_stroke_t[i] = acc;
        s_lightning_stroke_k[i] = rndf(0.68f, 1.0f)
            * (1.0f - (float)i * rndf(0.06f, 0.12f));
        if (i > 0) {
            acc += rndf(0.032f, 0.088f);
        }
    }

    s_lightning_peak = (float)(FIB_144 + FIB_13) + rndf(0.0f, (float)FIB_21);
    s_lightning_afterglow = s_lightning_peak * rndf(0.46f, 0.64f);   /* bolt lingers */
    s_lightning_channel_alpha = s_lightning_afterglow * 0.35f;
    s_lightning_flash_alpha = s_lightning_afterglow * 0.22f;
    s_lightning_alpha = s_lightning_channel_alpha;
}

static void schedule_next_lightning_strike(float t)
{
    /* 2026-07-03: tightened for on-screen drama (was 3.5–12 s storm). */
    float lo = (s_kind == WEATHER_HAIL) ? 2.2f : 2.5f;
    float hi = (s_kind == WEATHER_HAIL) ? 7.5f : 7.0f;
    s_lightning_next_strike_at = t + rndf(lo, hi);
}

static void draw_lightning_path(const int16_t *xs, const int16_t *ys, int pts,
                                uint16_t core, uint16_t glow, uint8_t alpha)
{
    if (pts < 2 || alpha < FIB_2) return;

    /* Bug (2026-07-03): the day thunderstorm sky was lifted to a light grey
     * and clouds now paint dense/bright light-plane cover on top of it — a
     * near-white 1 px core at any alpha reads as invisible against that
     * near-white deck. Fix: draw the core 2 px wide and always emit a soft
     * glow pass (previously gated to alpha>=FIB_89, so most strokes had no
     * glow at all) so the bolt has enough width/spread to stay legible on a
     * bright storm sky, not just a fully-black one. */
    uint8_t core_a = clamp_u8((int)alpha);
    for (int i = 1; i < pts; ++i) {
        draw_rain_streak(xs[i - 1], ys[i - 1], xs[i], ys[i], core, core_a);
        draw_rain_streak(xs[i - 1] + 1, ys[i - 1], xs[i] + 1, ys[i], core, core_a);
    }

    uint8_t glow_a = clamp_u8((int)((alpha * (int)FIB_55) / 255));
    if (glow_a >= FIB_2) {
        for (int i = 1; i < pts; i += 2) {
            int ox = (i & 2) ? 2 : -2;
            draw_rain_streak(xs[i - 1] + ox, ys[i - 1], xs[i] + ox, ys[i],
                             glow, glow_a);
        }
    }
}

static void composite_lightning_flash(uint8_t alpha)
{
    if (alpha < FIB_2) return;

    uint16_t cool = rgb565(196, 214, 255);
    uint16_t warm = rgb565(255, 250, 242);
    int cx = s_lightning_flash_x;
    int cy = s_lightning_flash_y;

    /* Regional cloud illumination — no fat blobs along the channel.
     * Alphas boosted 2026-07-03: the old FIB_21/FIB_34 fractions (~8/13 %)
     * were invisible on the storm deck; a real flash lights the whole sky. */
    draw_filled_circle(cx, cy, FIB_144, cool, clamp_u8((alpha * FIB_34) / 255));
    draw_filled_circle(cx, cy, FIB_89, warm, clamp_u8((alpha * FIB_55) / 255));

    /* Sky wash — mimics cloud volume lighting. Denser grid + stronger
     * alpha; runs only on flash frames so the cost stays bounded. */
    uint8_t sky_a = clamp_u8((alpha * FIB_34) / 255);
    if (sky_a >= FIB_2) {
        int y1 = (int)((float)EVA_WEATHER_RENDER_H * 0.70f);
        for (int y = 0; y < y1; y += 2) {
            uint8_t row_a = (uint8_t)((sky_a * (y1 - y)) / (y1 + 1));
            if (row_a < FIB_2) continue;
            for (int x = 0; x < EVA_WEATHER_RENDER_W; x += 2) {
                int idx = eva_sbuf_idx(x, y);
                s_buf[idx] = blend565(s_buf[idx], warm, row_a);
            }
        }
    }
}

/* Lightning lifecycle (PyLightning / return-stroke model):
 *   1. update_lightning(dt,t) — one fixed channel, 2–5 return strokes with
 *      irregular spacing; channel never drops to zero between strokes.
 *   2. composite_lightning_on_render() — thin bolt + soft regional flash.
 */
static void update_lightning(float dt, float t)
{
    if (s_kind != WEATHER_THUNDERSTORM && s_kind != WEATHER_HAIL) {
        s_lightning_alpha = 0.0f;
        s_lightning_channel_alpha = 0.0f;
        s_lightning_flash_alpha = 0.0f;
        s_lightning_active = false;
        s_lightning_next_strike_at = 0.0f;
        return;
    }

    if (s_lightning_next_strike_at <= 0.0f) {
        schedule_next_lightning_strike(t);
    }

    if (s_lightning_active) {
        s_lightning_strike_age += dt;

        while (s_lightning_stroke_next < s_lightning_stroke_total
               && s_lightning_strike_age >= s_lightning_stroke_t[s_lightning_stroke_next]) {
            nudge_lightning_channel(s_lightning_stroke_next);
            s_lightning_stroke_next++;
        }

        float channel = s_lightning_afterglow;
        float flash = s_lightning_afterglow * 0.62f;

        for (int i = 0; i < (int)s_lightning_stroke_total; ++i) {
            float env = lightning_stroke_envelope(
                s_lightning_strike_age - s_lightning_stroke_t[i]);
            if (env <= 0.001f) continue;
            float stroke = s_lightning_peak * s_lightning_stroke_k[i] * env;
            if (stroke > channel) channel = stroke;
            if (stroke * 0.72f > flash) flash = stroke * 0.72f;
        }

        if (channel > s_lightning_afterglow * 1.35f) {
            channel *= 0.93f + 0.07f * sinf(s_lightning_strike_age * 380.0f + t * 2.7f);
        }

        float last_end = s_lightning_stroke_t[s_lightning_stroke_total - 1] + 0.10f;
        if (s_lightning_strike_age > last_end) {
            if (!s_lightning_in_fade) {
                s_lightning_in_fade = true;
                s_lightning_fade_start = s_lightning_strike_age;
                s_lightning_fade_dur = rndf(0.38f, 0.65f);   /* slower die-off */
            }
            float fade_u = (s_lightning_strike_age - s_lightning_fade_start)
                / s_lightning_fade_dur;
            if (fade_u >= 1.0f) {
                s_lightning_active = false;
                s_lightning_channel_alpha = 0.0f;
                s_lightning_flash_alpha = 0.0f;
                s_lightning_alpha = 0.0f;
                schedule_next_lightning_strike(t);
                return;
            }
            float fade_k = (1.0f - fade_u) * (1.0f - fade_u);
            channel *= fade_k;
            flash *= fade_k;
        }

        s_lightning_channel_alpha = channel;
        s_lightning_flash_alpha = flash;
        s_lightning_alpha = channel;
        return;
    }

    if (s_lightning_force || t >= s_lightning_next_strike_at) {
        s_lightning_force = false;
        start_lightning_strike(t);
    }
}

static void lightning_publish_snapshot(void)
{
    uint8_t back = (uint8_t)(s_li_front ^ 1u);
    lightning_snap_t *snap = &s_li_snap[back];
    snap->channel_alpha = clamp_u8((int)s_lightning_channel_alpha);
    snap->flash_alpha = clamp_u8((int)s_lightning_flash_alpha);
    snap->sheet_only = (uint8_t)s_lightning_sheet_only;
    snap->flash_x = (uint16_t)s_lightning_flash_x;
    snap->flash_y = (uint16_t)s_lightning_flash_y;
    snap->pt_count = (uint8_t)s_lightning_pt_count;
    memcpy(snap->x, s_lightning_x, sizeof snap->x);
    memcpy(snap->y, s_lightning_y, sizeof snap->y);
    snap->has_branch = s_lightning_has_branch;
    snap->branch_pts = s_lightning_branch_pts;
    memcpy(snap->bx, s_lightning_bx, sizeof snap->bx);
    memcpy(snap->by, s_lightning_by, sizeof snap->by);
    snap->bolt_sprite_ok = s_bolt_sprite_ok;
    snap->bolt_x = s_bolt_x;
    snap->bolt_y = s_bolt_y;
    snap->bolt_mirror = s_bolt_mirror;
    portENTER_CRITICAL(&s_li_mux);
    s_li_front = back;
    portEXIT_CRITICAL(&s_li_mux);
}

static void lightning_task(void *arg)
{
    (void)arg;
    const TickType_t period = pdMS_TO_TICKS(8);
    for (;;) {
        float t = (float)esp_timer_get_time() * 1e-6f;
        update_lightning(1.0f / 120.0f, t);
        lightning_publish_snapshot();
        vTaskDelay(period);
    }
}

/* Additive-ish A8 sprite blit: dst brightened toward tint by plane alpha × scale. */
static void blit_bolt_plane(const uint8_t *plane, int w, int h,
                            int x0, int y0, bool mirror,
                            uint16_t tint, uint8_t scale)
{
    if (!plane || scale == 0) return;
    for (int y = 0; y < h; ++y) {
        int dy = y0 + y;
        if ((unsigned)dy >= EVA_WEATHER_RENDER_H) continue;
        const uint8_t *src = &plane[y * w];
        for (int x = 0; x < w; ++x) {
            uint8_t m = src[mirror ? (w - 1 - x) : x];
            if (m < FIB_2) continue;
            int dx = x0 + x;
            if ((unsigned)dx >= EVA_WEATHER_RENDER_W) continue;
            uint8_t a = (uint8_t)(((uint16_t)m * scale) / 255U);
            if (a) {
                int idx = eva_sbuf_idx(dx, dy);
                s_buf[idx] = blend565(s_buf[idx], tint, a);
            }
        }
    }
}

static void composite_lightning_on_render(void)
{
    lightning_snap_t snap;
    portENTER_CRITICAL(&s_li_mux);
    snap = s_li_snap[s_li_front];
    portEXIT_CRITICAL(&s_li_mux);

    uint8_t flash_a = snap.flash_alpha;
    uint8_t bolt_a = snap.channel_alpha;
    if (flash_a < FIB_2 && bolt_a < FIB_2) return;

    if (flash_a >= FIB_2 && !s_storm_lit_active) {
        int save_x = s_lightning_flash_x;
        int save_y = s_lightning_flash_y;
        s_lightning_flash_x = (int16_t)snap.flash_x;
        s_lightning_flash_y = (int16_t)snap.flash_y;
        composite_lightning_flash(flash_a);
        s_lightning_flash_x = save_x;
        s_lightning_flash_y = save_y;
    }

    if (snap.sheet_only || bolt_a < FIB_2) return;

    /* Brighter, cooler-white core reads as a hot channel; the glow keeps a
     * bluish halo. Core is pushed to near-pure white so a thin 1px baked
     * channel still pops against a bright storm deck (the sprite is thin now,
     * so brightness—not width—carries legibility). */
    uint16_t core = rgb565(255, 255, 255);
    uint16_t glow = rgb565(176, 202, 255);
    if (snap.bolt_sprite_ok) {
        blit_bolt_plane(s_bolt_sprite.plane[1], s_bolt_sprite.w, s_bolt_sprite.h,
                        snap.bolt_x, snap.bolt_y, snap.bolt_mirror,
                        glow, (uint8_t)((bolt_a * BOLT_GLOW_SCALE_NUM) / BOLT_GLOW_SCALE_DEN));
        blit_bolt_plane(s_bolt_sprite.plane[0], s_bolt_sprite.w, s_bolt_sprite.h,
                        snap.bolt_x, snap.bolt_y, snap.bolt_mirror,
                        core, bolt_a);
    } else {
        draw_lightning_path(snap.x, snap.y, snap.pt_count, core, glow, bolt_a);
        if (snap.has_branch) {
            draw_lightning_path(snap.bx, snap.by, snap.branch_pts,
                                core, glow, clamp_u8((bolt_a * FIB_89) / 255));
        }
    }
}

/* --- Glass overlay (topmost layer) ----------------------------------------
 * Simulates a protective glass panel: sun specular glints when the disc is
 * up, and slow sliding droplets during rain. Drawn after lightning so it
 * always reads as foreground. */
static void reset_glass_overlay_for_kind(void)
{
    s_glass_drops_inited = false;
    s_wet_y0 = 1 << 30;
    s_wet_y1 = -1;
}

static void ensure_wet_glass(void)
{
    if (s_wet_glass) return;
    s_wet_glass = heap_caps_calloc((size_t)EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H,
                                   1, MALLOC_CAP_SPIRAM);
    s_wet_y0 = 1 << 30;
    s_wet_y1 = -1;
}

static void stamp_trail_sprite(int x0, int y0, int variant)
{
    eva_sprite_t tr;
    if (!s_wet_glass ||
        !eva_cloud_assets_sprite(EVA_CLP_TYPE_TRAIL, 0, variant, &tr)) {
        return;
    }
    int tx = x0 - tr.w / 2;
    int ty = y0 - tr.h + 4;
    for (int y = 0; y < tr.h; ++y) {
        int dy = ty + y;
        if ((unsigned)dy >= EVA_WEATHER_RENDER_H) continue;
        const uint8_t *src = &tr.plane[0][y * tr.w];
        uint8_t *row = &s_wet_glass[dy * EVA_WEATHER_RENDER_W];
        for (int x = 0; x < tr.w; ++x) {
            int dx = tx + x;
            if ((unsigned)dx >= EVA_WEATHER_RENDER_W) continue;
            uint8_t a = src[x];
            if (a < FIB_2) continue;
            if (row[dx] < a) row[dx] = a;
            if (dy < s_wet_y0) s_wet_y0 = dy;
            if (dy > s_wet_y1) s_wet_y1 = dy;
        }
    }
}

static void decay_wet_glass_band(void)
{
    if (!s_wet_glass || s_wet_y0 > s_wet_y1) return;
    int ny0 = 1 << 30;
    int ny1 = -1;
    for (int y = s_wet_y0; y <= s_wet_y1; ++y) {
        uint8_t *row = &s_wet_glass[y * EVA_WEATHER_RENDER_W];
        for (int x = 0; x < EVA_WEATHER_RENDER_W; ++x) {
            row[x] = (uint8_t)((row[x] * 247) >> 8);
            if (row[x] >= FIB_2) {
                if (y < ny0) ny0 = y;
                if (y > ny1) ny1 = y;
            }
        }
    }
    s_wet_y0 = ny0;
    s_wet_y1 = ny1;
}

static void composite_wet_glass_band(void)
{
    if (!s_wet_glass || s_wet_y0 > s_wet_y1) return;
    const uint16_t tint = rgb565(196, 206, 220);
    for (int y = s_wet_y0; y <= s_wet_y1; ++y) {
        const uint8_t *src = &s_wet_glass[y * EVA_WEATHER_RENDER_W];
        for (int x = 0; x < EVA_WEATHER_RENDER_W; ++x) {
            uint8_t wv = src[x];
            if (wv < FIB_2) continue;
            uint8_t a = (uint8_t)(wv >> 1);
            if (a) {
                int idx = eva_sbuf_idx(x, y);
                s_buf[idx] = blend565(s_buf[idx], tint, a);
            }
        }
    }
}

static void ensure_glass_glints(void)
{
    if (s_glass_glints_inited) return;
    for (int i = 0; i < GLASS_GLINT_MAX; ++i) {
        glass_glint_t *g = &s_glass_glints[i];
        g->x = rndf(48.0f, (float)EVA_WEATHER_RENDER_W - 48.0f);
        g->y = rndf(24.0f, (float)EVA_WEATHER_RENDER_H * 0.58f);
        g->len = rndf(16.0f, 44.0f);
        g->phase = rndf(0.0f, 6.2831853f);
        g->strength = rndf(0.45f, 1.0f);
    }
    s_glass_glints_inited = true;
}

static uint16_t glass_drop_target_for_kind(weather_kind_t kind)
{
    switch (kind) {
    case WEATHER_HEAVY_RAIN:
    case WEATHER_THUNDERSTORM:
        return GLASS_DROP_MAX;
    case WEATHER_RAIN:
    case WEATHER_SLEET:
        return FIB_21;
    default:
        return 0;
    }
}

static float glass_slide_quota_px(float y)
{
    float room = (float)EVA_WEATHER_RENDER_H - y - 12.0f;
    if (room < 20.0f) room = 20.0f;
    float q = room * rndf(0.07f, 0.38f);
    if (q < 18.0f) q = 18.0f;
    if (q > 145.0f) q = 145.0f;
    return q;
}

static float glass_ease_smooth(float t)
{
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

static float glass_drop_v_term(float r)
{
    float vt = GLASS_VTERM_BASE + r * GLASS_VTERM_PER_R;
    if (s_kind == WEATHER_HEAVY_RAIN || s_kind == WEATHER_THUNDERSTORM) {
        vt *= 1.14f;
    }
    return vt;
}

static void glass_drop_begin_dry(glass_drop_t *d)
{
    d->state = GLASS_DROP_DRYING;
    d->vy = 0.0f;
    d->vx *= 0.35f;
    d->fade_total = rndf(0.07f, 0.38f);
    d->timer = d->fade_total;
}

static void respawn_glass_drop(glass_drop_t *d)
{
    d->target_r = rndf(2.4f, 5.0f);
    d->r = d->target_r * rndf(0.30f, 0.48f);
    d->v_term = glass_drop_v_term(d->r) + rndf(0.0f, 6.0f);
    d->phase = rndf(0.0f, 6.2831853f);
    d->alpha_peak = rndf(0.44f, 0.90f);
    d->alpha = d->alpha_peak;
    d->vx = -s_wind_vx_bias * 0.07f + rndf(-2.0f, 2.0f);
    d->vy = 0.0f;
    d->dist_slid = 0.0f;
    d->form_total = 0.0f;
    d->fade_total = 0.0f;
    d->x = rndf(28.0f, (float)EVA_WEATHER_RENDER_W - 28.0f);

    float roll = rndf(0.0f, 1.0f);
    if (roll < 0.50f) {
        /* Condensation bead: appears on the pane, grows, then slides or dries. */
        d->state = GLASS_DROP_FORMING;
        d->y = rndf((float)EVA_WEATHER_RENDER_H * 0.05f,
                    (float)EVA_WEATHER_RENDER_H * 0.90f);
        d->form_total = rndf(0.30f, 1.50f);
        d->timer = d->form_total;
        d->r = d->target_r * rndf(0.22f, 0.40f);
        d->will_slide = rndf(0.0f, 1.0f) > 0.14f;
        d->slide_quota = glass_slide_quota_px(d->y);
    } else if (roll < 0.80f) {
        /* Already sliding mid-pane — not spawned from the top edge. */
        d->r = d->target_r;
        d->v_term = glass_drop_v_term(d->r);
        d->state = GLASS_DROP_SLIDING;
        d->y = rndf((float)EVA_WEATHER_RENDER_H * 0.04f,
                    (float)EVA_WEATHER_RENDER_H * 0.76f);
        d->vy = rndf(0.0f, d->v_term * 0.55f);
        d->slide_quota = glass_slide_quota_px(d->y);
        d->will_slide = true;
    } else {
        /* Upper band entry: only a short run, rarely the full pane height. */
        d->r = d->target_r;
        d->v_term = glass_drop_v_term(d->r);
        d->state = GLASS_DROP_SLIDING;
        d->y = rndf(-18.0f, (float)EVA_WEATHER_RENDER_H * 0.14f);
        d->vy = rndf(0.0f, d->v_term * 0.22f);
        d->slide_quota = rndf(28.0f, 118.0f);
        d->will_slide = true;
    }
}

static void ensure_glass_drops(void)
{
    uint16_t target = glass_drop_target_for_kind(s_kind);
    if (target == 0) {
        s_glass_drops_inited = false;
        return;
    }
    if (s_glass_drops_inited) return;
    memset(s_glass_drops, 0, sizeof(s_glass_drops));
    for (uint16_t i = 0; i < target && i < GLASS_DROP_MAX; ++i) {
        respawn_glass_drop(&s_glass_drops[i]);
    }
    s_glass_drops_inited = true;
}

static void draw_glass_sun_glints(float t)
{
    if (!s_sun_visible || s_sun_strength <= 0.0f || is_night_kind(s_kind)) return;
    if (s_sun_x < 0 || s_sun_y < 0) return;

    ensure_glass_glints();
    uint16_t cool = rgb565(214, 232, 255);
    float sun_k = s_sun_strength;

    /* Soft fixed sparkles on the pane — no orbiting streak/beam near the sun. */
    for (int i = 0; i < GLASS_GLINT_MAX; ++i) {
        const glass_glint_t *g = &s_glass_glints[i];
        float tw = 0.30f + 0.70f * sinf(t * (1.15f + 0.07f * (float)i) + g->phase);
        if (tw < 0.28f) continue;

        int ix = (int)g->x;
        int iy = (int)g->y;
        uint8_t a = smooth_u8(tw, (uint8_t)(28.0f * sun_k * g->strength));
        draw_filled_circle(ix, iy, 2, cool, a);
    }
}

static void draw_glass_drop_bead(int x, int y, int r, uint16_t col_hi, uint8_t a)
{
    if (a < 4) return;
    if (r < 1) r = 1;
    draw_filled_circle(x, y, r, col_hi, a);
    if (r > 2) {
        draw_filled_circle(x - 1, y - 1, 1, col_hi, (uint8_t)(a * 4 / 5));
    }
}

static void update_and_draw_glass_drops(float dt, float t)
{
    (void)t;
    uint16_t target = glass_drop_target_for_kind(s_kind);
    if (target == 0) return;

    ensure_glass_drops();
    ensure_wet_glass();
    if (++s_wet_decay_tick >= FIB_5) {
        s_wet_decay_tick = 0;
        decay_wet_glass_band();
    }

    uint16_t col_drop = rgb565(188, 210, 232);
    uint16_t col_hi = rgb565(250, 252, 255);

    for (uint16_t i = 0; i < target && i < GLASS_DROP_MAX; ++i) {
        glass_drop_t *d = &s_glass_drops[i];
        float prev_x = d->x;
        float prev_y = d->y;

        switch (d->state) {
        case GLASS_DROP_FORMING:
            d->timer -= dt;
            if (d->form_total > 0.01f) {
                float prog = 1.0f - (d->timer / d->form_total);
                if (prog < 0.0f) prog = 0.0f;
                if (prog > 1.0f) prog = 1.0f;
                float start_r = d->target_r * 0.32f;
                d->r = start_r + (d->target_r - start_r) * glass_ease_smooth(prog);
            }
            if (d->timer <= 0.0f) {
                d->r = d->target_r;
                d->v_term = glass_drop_v_term(d->r);
                if (d->will_slide) {
                    d->state = GLASS_DROP_SLIDING;
                } else {
                    glass_drop_begin_dry(d);
                }
            }
            break;

        case GLASS_DROP_SLIDING: {
            /* Bead may swell slightly while sliding (collecting runoff). */
            if (d->r < d->target_r) {
                d->r += dt * 0.05f;
                if (d->r > d->target_r) {
                    d->r = d->target_r;
                }
            }
            d->v_term = glass_drop_v_term(d->r);

            /* Quadratic drag → slow start, ease into terminal speed. */
            float speed_ratio = (d->v_term > 0.5f) ? (d->vy / d->v_term) : 0.0f;
            float drag = speed_ratio * speed_ratio;
            if (drag > 1.0f) {
                drag = 1.0f;
            }

            /* Brief runaway near quota end: bigger beads overshoot v_term. */
            float quota_used = (d->slide_quota > 1.0f) ? (d->dist_slid / d->slide_quota) : 0.0f;
            float runaway = 0.0f;
            if (quota_used > 0.78f && d->target_r > 2.6f) {
                runaway = (quota_used - 0.78f) / 0.22f;
                if (runaway > 1.0f) {
                    runaway = 1.0f;
                }
            }

            float accel = GLASS_SLIDE_GRAVITY * (1.0f - drag * (1.0f - runaway * 0.62f));
            if (runaway > 0.0f) {
                accel += GLASS_SLIDE_GRAVITY * 0.18f * runaway;
            }
            d->vy += accel * dt;

            float v_cap = d->v_term * (1.0f + runaway * 0.34f);
            if (d->vy > v_cap) {
                d->vy = v_cap;
            }

            float wind_goal = -s_wind_vx_bias * 0.07f;
            d->vx += (wind_goal - d->vx) * fminf(1.0f, dt * 1.8f);
            d->x += d->vx * dt;
            d->y += d->vy * dt;
            d->dist_slid += d->vy * dt;

            if (d->dist_slid >= d->slide_quota) {
                glass_drop_begin_dry(d);
            } else if (d->y > (float)EVA_WEATHER_RENDER_H + 18.0f ||
                       d->x < -20.0f || d->x > (float)EVA_WEATHER_RENDER_W + 20.0f) {
                respawn_glass_drop(d);
                continue;
            }
            break;
        }

        case GLASS_DROP_DRYING:
            d->timer -= dt;
            if (d->fade_total > 0.01f) {
                d->alpha = d->alpha_peak * (d->timer / d->fade_total);
            } else {
                d->alpha = 0.0f;
            }
            if (d->timer <= 0.0f || d->alpha < 0.04f) {
                respawn_glass_drop(d);
                continue;
            }
            break;
        }

        int x = (int)d->x;
        int y = (int)d->y;
        int r = (int)d->r;
        uint8_t a = clamp_u8((int)(d->alpha * 200.0f));
        if (a < FIB_2) continue;

        if (d->state == GLASS_DROP_SLIDING && d->vy > 1.0f) {
            stamp_trail_sprite((int)prev_x, (int)prev_y, (int)i & 1);
        }

        eva_sprite_t dsp;
        int size_c = r < FIB_5 ? 0 : (r < FIB_13 ? 1 : 2);
        int shape = (int)(d->phase * 3.0f) % 3;
        if (eva_cloud_assets_sprite(EVA_CLP_TYPE_DROP, size_c, shape, &dsp)) {
            int x0 = x - dsp.w / 2;
            int y0 = y - dsp.h / 2;
            blit_bolt_plane(dsp.plane[0], dsp.w, dsp.h, x0, y0, false,
                            rgb565(210, 220, 232), (uint8_t)((a * 3) / 5));
            blit_bolt_plane(dsp.plane[1], dsp.w, dsp.h, x0, y0, false,
                            rgb565(255, 255, 255), a);
            continue;
        }

        if (d->state == GLASS_DROP_SLIDING && d->vy > 1.0f) {
            int trail = (int)(8.0f + d->vy * 0.11f);
            if (trail < FIB_5) trail = FIB_5;
            if (trail > FIB_21) trail = FIB_21;
            float speed_k = (d->v_term > 1.0f) ? (d->vy / d->v_term) : 0.0f;
            if (speed_k > 1.0f) {
                speed_k = 1.0f;
            }
            uint8_t ta = (uint8_t)(a * (0.20f + 0.42f * speed_k));
            if (ta < FIB_3) ta = FIB_3;
            int tx = x - (int)(d->vx * 0.06f);
            draw_rain_streak(tx, y - trail, x, y, col_drop, ta);
        }

        draw_glass_drop_bead(x, y, r, col_hi, a);
    }
}

static void composite_glass_overlay(float dt, float t)
{
    (void)dt;
    /* Glass rain drops + wet-pane accumulation REMOVED 2026-07-04: the
     * per-drop sprite blit + full-screen wet-glass composite cost ~40 ms/
     * frame (gl bucket), dropping storm/rain to ~11 Hz — the single most
     * expensive thing in the scene. Sun glints are kept (a few tiny blits,
     * negligible). The drop FSM (respawn/update/draw) and the wet-glass
     * accumulation buffer are now dead code, left in place but never
     * called; s_wet_glass is never allocated. */
    draw_glass_sun_glints(t);
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

static esp_err_t ppa_copy_rgb565(uint16_t *dst, const uint16_t *src)
{
    if (!dst || !src) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef EVA_PORTRAIT_NATIVE
    if (dst == s_dpi_back_fb || dst == s_dpi_scan_fb) {
        copy_landscape_rgb565_to_sbuf(src);
        return ESP_OK;
    }
#endif
    memcpy(dst, src, EVA_FRAME_BYTES);
    return ESP_OK;
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

void eva_weather_canvas_cloud_info(char *buf, size_t buf_len)
{
    static const char *names[CLOUD_LAYER_COUNT] = { "HIGH", "MID", "LOW" };
    size_t off = 0;
    off += (size_t)snprintf(buf + off, buf_len - off,
                            "cloud assets: %s, last load %lld us\r\n",
                            s_cloud_assets_ok ? "mmap CLP2" : "procedural fallback",
                            (long long)eva_cloud_assets_last_load_us());
    for (int i = 0; i < CLOUD_LAYER_COUNT && off < buf_len; ++i) {
        const cloud_strip_t *s = &s_strip[i];
        cloud_pool_t pool = (cloud_pool_t)s->pool_kind;
        const char *pool_tag = (pool == CLOUD_POOL_STORM) ? "storm" : "normal";
        off += (size_t)snprintf(buf + off, buf_len - off,
                                "%s: %s v%d/%d prev %d scale %.2f%s%s\r\n",
                                names[i], pool_tag, s->pool_cur,
                                eva_cloud_assets_count(i, pool), s->pool_prev,
                                (double)s->depth_scale,
                                s->mirrored ? " mirrored" : "",
                                s->morphing ? " (morphing)" : "");
    }
}

/* Drive the live render fields from the active transition. Called once per
 * frame at the very top of render_weather(). Returns true if the transition
 * finalized on this call (so render_weather can run the deferred kind reset).*/
static bool wx_transition_tick(float dt)
{
    if (!s_wx_trans.active) return false;

    float p = eva_wx_ease(s_wx_trans.progress);

    s_cloud_cover_pct = eva_wx_lerp_u8(s_wx_trans.start_cover,
                                       s_wx_trans.target_cover, p);
    for (int i = 0; i < 3; ++i) {
        s_cloud_pct[i] = eva_wx_lerp_u8(s_wx_trans.start_cloud_pct[i],
                                        s_wx_trans.target_cloud_pct[i], p);
    }
    s_fog_pct = eva_wx_lerp_u8(s_wx_trans.start_fog, s_wx_trans.target_fog, p);
    s_density_scale = eva_wx_lerp_f(s_wx_trans.start_density,
                                    s_wx_trans.target_density, p);

    /* Re-derive the active 3D cloud count from the interpolated cover, exactly
     * as eva_weather_canvas_set_weather does. */
    int desired_active = (int)(((long)s_cloud_cover_pct * CLOUD_3D_MAX + 50) / 100);
    if (desired_active < 4) desired_active = 4;
    if (desired_active > CLOUD_3D_MAX) desired_active = CLOUD_3D_MAX;
    s_clouds3d_active = (uint8_t)desired_active;

    /* Precip identity flips at the midpoint; s_kind flips with it so the sky
     * blend (Task 4) and particle reset (below) use the right target. */
    int precip_now = eva_wx_precip_at(&s_wx_trans, s_wx_trans.progress);
    s_precip_type = (precip_type_t)precip_now;
    s_kind = (s_wx_trans.progress < 0.5f)
             ? (weather_kind_t)s_wx_trans.from_kind
             : (weather_kind_t)s_wx_trans.to_kind;

    /* Recompute particle target each frame so the precip ramp animates
     * (target_for_kind is otherwise only called on kind reset). */
    target_for_kind(s_kind);

    bool finished = eva_wx_advance(&s_wx_trans, dt);
    if (finished) {
        s_kind = (weather_kind_t)s_wx_trans.to_kind;
        s_precip_type = (precip_type_t)s_wx_trans.to_precip;
        target_for_kind(s_kind);   /* full intensity, ramp inactive */
    }
    return finished;
}

static void render_weather(float dt)
{
    /* Advance any active smooth transition first — it may flip s_kind at the
     * midpoint, which the reset block below then reacts to (once). */
    wx_transition_tick(dt);

    if (s_kind != s_prev_kind) {
        reset_particles_for_kind();
        reset_glass_overlay_for_kind();
        s_lightning_active = false;
        s_lightning_alpha = 0.0f;
        s_lightning_channel_alpha = 0.0f;
        s_lightning_flash_alpha = 0.0f;
        s_lightning_next_strike_at = 0.0f;
        s_bg_ttl = 0;
        s_bg_dt = 0.0f;
        s_scene_base_dirty = true;
        /* Abort any in-flight amortized repaint — its snapshot belongs to
         * the previous kind; a fresh one starts next frame. */
        s_bg_paint_row = -1;
        s_prev_kind = s_kind;
    }

    int64_t now_us = esp_timer_get_time();
    float t = (float)now_us / 1000000.0f;
    s_bg_dt += dt;

    int64_t tb0 = esp_timer_get_time();
    bool sky_refreshed = false;
    if (!s_bg_buf || (!s_bg_next && s_bg_ttl == 0)) {
        /* Fallback path (bg buffers unavailable): synchronous full paint
         * into the frame, exactly the old behaviour. */
        sky_t sky = wx_current_sky();
        s_sky_bottom = sky.bottom;   /* cache for sky-tinted rain streaks */
        int m = minutes_now();
        compute_luminary_positions(m);
        fill_sky(sky.top, sky.bottom,
                 s_sun_pos.x_n, s_sun_pos.y_n, s_sun_pos.warmth);
        draw_day_sky_depth();
        draw_sun_or_moon(t);
        if (s_bg_buf) {
            (void)ppa_copy_rgb565(s_bg_buf, s_buf);
        }
        s_bg_dt = 0.0f;
        s_bg_ttl = background_hold_frames(s_kind);
        sky_refreshed = true;
    } else {
        if (s_bg_ttl > 0) s_bg_ttl--;
        /* During a transition, advance the rebake whenever eased progress has
         * moved ~0.09 since the last sky bake — ~11 rebakes across the whole
         * transition, so the gradient visibly eases without per-frame cost. */
        if (s_wx_trans.active) {
            float pe = eva_wx_ease(s_wx_trans.progress);
            if (pe - s_wx_last_bake_p >= 0.09f) {
                s_bg_ttl = 0;
            }
        }
        if (s_bg_ttl == 0 && s_bg_paint_row < 0) {
            /* Snapshot everything the repaint needs so the buffer stays
             * consistent while slices land across several frames. */
            s_wx_last_bake_p = eva_wx_ease(s_wx_trans.progress);
            s_bg_snap_sky = wx_current_sky();
            s_sky_bottom = s_bg_snap_sky.bottom;
            compute_luminary_positions(minutes_now());
            s_bg_snap_sun_x = s_sun_pos.x_n;
            s_bg_snap_sun_y = s_sun_pos.y_n;
            s_bg_snap_warmth = s_sun_pos.warmth;
            s_bg_snap_t = t;
            s_bg_paint_row = 0;
        }
        if (s_bg_paint_row >= 0) {
            int y0 = s_bg_paint_row;
            int y1 = y0 + BG_PAINT_ROWS_PER_FRAME;
            if (y1 > EVA_WEATHER_RENDER_H) y1 = EVA_WEATHER_RENDER_H;
            fill_sky_rows(s_bg_next, y0, y1,
                          s_bg_snap_sky.top, s_bg_snap_sky.bottom,
                          s_bg_snap_sun_x, s_bg_snap_sun_y, s_bg_snap_warmth);
            s_bg_paint_row = y1;
            if (y1 >= EVA_WEATHER_RENDER_H) {
                /* Final slice: overlays paint via s_buf-based helpers —
                 * retarget them at the back buffer (same task, no race). */
                uint16_t *save = s_buf;
                s_buf = s_bg_next;
                draw_day_sky_depth();
                draw_sun_or_moon(s_bg_snap_t);
                s_buf = save;
                uint16_t *old_bg = s_bg_buf;
                s_bg_buf = s_bg_next;
                s_bg_next = old_bg;
                s_bg_paint_row = -1;
                s_bg_dt = 0.0f;
                s_bg_ttl = background_hold_frames(s_kind);
                s_scene_base_dirty = true;
            }
        }
    }
    int64_t tb_after_bg = esp_timer_get_time();
    s_prof_bg_us += (tb_after_bg - tb0);

    s_merged_storm_active = use_merged_storm_layers();

    /* Z-order (bottom → top): sky+sun → outdoor rain/lightning → text →
     * clouds → glass. s_bg_buf holds sky+sun only; copy scene base or bg. */
    if (s_scene_base && scene_text_can_cache(s_kind)) {
        if (s_scene_base_dirty && s_bg_buf) {
            (void)ppa_copy_rgb565(s_scene_base, s_bg_buf);
            uint16_t *save = s_buf;
            s_buf = s_scene_base;
            draw_scene_text_overlays();
            s_buf = save;
            s_scene_base_dirty = false;
        }
        (void)ppa_copy_rgb565(s_buf, s_scene_base);
        s_scene_text_done_this_frame = true;
    } else if (s_bg_buf && !sky_refreshed) {
        (void)ppa_copy_rgb565(s_buf, s_bg_buf);
        s_scene_text_done_this_frame = false;
    } else {
        s_scene_text_done_this_frame = false;
    }

    draw_sun_fib_light(t);

    int64_t tb_overlay0 = esp_timer_get_time();
    update_and_draw_particles(dt, t);
    int64_t tb3 = esp_timer_get_time();
    s_prof_particles_us += (tb3 - tb_overlay0);

    s_storm_lit_active = false;

    int64_t tb_text0 = esp_timer_get_time();
    if (!s_scene_text_done_this_frame) {
        draw_scene_text_overlays();
    }
    int64_t tb_after_text = esp_timer_get_time();
    s_prof_text_us += (tb_after_text - tb_text0);

    advance_cloud_frame(dt);
    int64_t tb_cloud0 = esp_timer_get_time();
    {
        /* s_blend_from_sky stays false: clouds blend over s_buf (sky+rain+
         * lightning+text), not the sky-only cache. Wrap bands use s_buf too. */
        if (s_strip[0].morphing || s_strip[1].morphing || s_strip[2].morphing) {
            s_prof_morph_frames++;
        }
        bool sky_wrap = false;
        if (s_merged_storm_active) {
            (void)blend_layer(&s_strip[CLOUD_LAYER_LOW], false);
        } else {
            if (blend_layer(&s_strip[CLOUD_LAYER_HIGH], sky_wrap)) sky_wrap = false;
            if (blend_layer(&s_strip[CLOUD_LAYER_MID], sky_wrap)) sky_wrap = false;
            (void)blend_layer(&s_strip[CLOUD_LAYER_LOW], sky_wrap);
        }
    }
    s_blend_from_sky = false;
    int64_t tb_after_clouds = esp_timer_get_time();
    s_prof_clouds_us += (tb_after_clouds - tb_cloud0);

    /* Bolt + regional flash over the cloud deck (state updated above). */
    composite_lightning_on_render();
    int64_t tb_li2 = esp_timer_get_time();
    s_prof_lightning_us += (tb_li2 - tb_after_clouds);

    int64_t tb_glass0 = esp_timer_get_time();
    composite_glass_overlay(dt, t);
    int64_t tb_glass1 = esp_timer_get_time();
    s_prof_glass_us += (tb_glass1 - tb_glass0);
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
    int64_t tick_min = 1000000, tick_max = 0;   /* DIAG: frame interval jitter */

    while (true) {
#ifdef EVA_PORTRAIT_NATIVE
        if (!s_visible || !s_panel || !s_dpi_back_fb) {
#else
        if (!s_visible || !s_render_buf || !s_panel || !s_dpi_back_fb) {
#endif
            s_last_us = esp_timer_get_time();
            vTaskDelay(pdMS_TO_TICKS(TIMER_MS));
            continue;
        }

        int64_t now = esp_timer_get_time();
        int64_t tick_us = s_last_us ? now - s_last_us : (int64_t)TIMER_MS * 1000;
        float dt = (float)tick_us / 1000000.0f;
        if (dt < 0.0f || dt > 0.10f) dt = (float)TIMER_MS / 1000.0f;
        s_last_us = now;
        if (tick_us < tick_min) tick_min = tick_us;
        if (tick_us > tick_max) tick_max = tick_us;
        int64_t t0 = esp_timer_get_time();
        if (s_render_lock) {
            xSemaphoreTake(s_render_lock, portMAX_DELAY);
        }
#ifdef EVA_PORTRAIT_NATIVE
        s_buf = s_dpi_back_fb;
#else
        s_buf = s_render_buf;
#endif
        render_weather(dt);
        if (s_render_lock) {
            xSemaphoreGive(s_render_lock);
        }
        int64_t t_render = esp_timer_get_time();

#ifndef EVA_PORTRAIT_NATIVE
        esp_err_t err = rotate_render_to_dpi_fb(s_dpi_back_fb);
        if (err == ESP_OK) {
            if (xSemaphoreTake(s_ppa_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
                ESP_LOGW(TAG, "PPA rotate timeout");
                vTaskDelay(pdMS_TO_TICKS(TIMER_MS));
                continue;
            }
        } else {
            ESP_LOGE(TAG, "PPA rotate failed: 0x%x", (unsigned)err);
            vTaskDelay(pdMS_TO_TICKS(TIMER_MS));
            continue;
        }
#else
        esp_err_t err = ESP_OK;
#endif
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
        (void)t_draw;

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
            uint32_t tx_avg     = (uint32_t)(s_prof_text_us      / s_frames);
            uint32_t cl_avg     = (uint32_t)(s_prof_clouds_us    / s_frames);
            uint32_t pa_avg     = (uint32_t)(s_prof_particles_us / s_frames);
            uint32_t li_avg     = (uint32_t)(s_prof_lightning_us / s_frames);
            uint32_t gl_avg     = (uint32_t)(s_prof_glass_us      / s_frames);
            uint32_t rot_avg    = (uint32_t)(prof_rotate_us      / s_frames);
            uint32_t vsync_avg  = (uint32_t)(s_accum_lvgl_slot_us / s_frames);
            uint32_t clb_avg    = (uint32_t)(s_prof_blend_bands  / s_frames);
            ESP_LOGI(TAG, "%s tick=%u Hz, %u/%u particles c3d=%u/%u work_us=%u (bg=%u tx=%u cl=%u pa=%u li=%u gl=%u ppa_rot=%u lvgl=0 vsync=%u clb=%u mfr=%u) jitter=%u..%u",
                     weather_kind_name(s_kind), (unsigned)tick_hz,
                     (unsigned)s_target, (unsigned)s_max_target,
                     (unsigned)s_clouds3d_active, (unsigned)CLOUD_3D_MAX,
                     (unsigned)avg,
                     bg_avg, tx_avg, cl_avg, pa_avg, li_avg, gl_avg, rot_avg, vsync_avg,
                     clb_avg, (unsigned)s_prof_morph_frames,
                     (unsigned)tick_min, (unsigned)tick_max);
            tick_min = 1000000; tick_max = 0;
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
            s_prof_text_us = 0;
            s_prof_clouds_us = 0;
            s_prof_particles_us = 0;
            s_prof_lightning_us = 0;
            s_prof_glass_us = 0;
            s_prof_blend_bands = 0;
            s_prof_morph_frames = 0;
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
        uint32_t tx_avg     = (uint32_t)(s_prof_text_us     / s_frames);
        uint32_t cl_avg     = (uint32_t)(s_prof_clouds_us   / s_frames);
        uint32_t pa_avg     = (uint32_t)(s_prof_particles_us/ s_frames);
        uint32_t li_avg     = (uint32_t)(s_prof_lightning_us/ s_frames);
        uint32_t gl_avg     = (uint32_t)(s_prof_glass_us     / s_frames);
        uint32_t lvgl_avg   = (uint32_t)(s_accum_lvgl_slot_us / s_frames);
        uint32_t vsync_avg  = 0;
        uint32_t up_avg     = (uint32_t)(prof_upscale_us    / s_frames);
        ESP_LOGI(TAG, "%s tick=%u Hz, %u/%u particles c3d=%u/%u work_us=%u (bg=%u tx=%u cl=%u pa=%u li=%u gl=%u up=%u lvgl=%u vsync=%u)",
                 weather_kind_name(s_kind), (unsigned)tick_hz,
                 (unsigned)s_target, (unsigned)s_max_target,
                 (unsigned)s_clouds3d_active, (unsigned)CLOUD_3D_MAX,
                 (unsigned)avg,
                 bg_avg, tx_avg, cl_avg, pa_avg, li_avg, gl_avg, up_avg, lvgl_avg, vsync_avg);
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
        s_prof_text_us = 0;
        s_prof_clouds_us = 0;
        s_prof_particles_us = 0;
        s_prof_lightning_us = 0;
        s_prof_glass_us = 0;
        prof_upscale_us = 0;
    }

    s_last_tick_exit_us = esp_timer_get_time();
}

lv_obj_t *eva_weather_canvas_init(lv_obj_t *parent)
{
    if (s_canvas) return s_canvas;

    s_rng ^= (uint32_t)esp_timer_get_time();
    s_render_buf = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                           EVA_WEATHER_CANVAS_W * EVA_WEATHER_CANVAS_H * sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_render_buf) {
        ESP_LOGE(TAG, "render buffer alloc failed");
        abort();
    }
    s_display_buf = s_render_buf;
    s_buf = s_render_buf;
    if (!s_render_lock) {
        s_render_lock = xSemaphoreCreateMutex();
        if (!s_render_lock) {
            ESP_LOGE(TAG, "render lock alloc failed");
            abort();
        }
    }
    s_bg_buf = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                       EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg_buf) {
        ESP_LOGE(TAG, "background buffer alloc failed");
        abort();
    }
    /* Back buffer for the amortized sky repaint; if it fails we just fall
     * back to the old synchronous rebake (s_bg_next stays NULL). */
    s_bg_next = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                        EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg_next) {
        ESP_LOGW(TAG, "bg back buffer alloc failed — synchronous sky rebake");
    }
    s_scene_base = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                             EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scene_base) {
        ESP_LOGW(TAG, "scene-base buffer alloc failed — per-frame text fallback");
    }
    /* Scene text A8 cache (clock + date + temp + desc). Lifetime = process. */
    s_scene_slot.a8 = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, TEXT_SLOT_BUF_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scene_slot.a8) {
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
    s_render_buf = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                           EVA_WEATHER_CANVAS_W * EVA_WEATHER_CANVAS_H * sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_render_buf) {
        ESP_LOGE(TAG, "render buffer alloc failed");
        abort();
    }
    s_display_buf = s_render_buf;
    s_buf = s_render_buf;
    if (!s_render_lock) {
        s_render_lock = xSemaphoreCreateMutex();
        if (!s_render_lock) {
            ESP_LOGE(TAG, "render lock alloc failed");
            abort();
        }
    }
    s_bg_buf = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                       EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg_buf) {
        ESP_LOGE(TAG, "background buffer alloc failed");
        abort();
    }
    /* Back buffer for the amortized sky repaint; if it fails we just fall
     * back to the old synchronous rebake (s_bg_next stays NULL). */
    s_bg_next = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                        EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bg_next) {
        ESP_LOGW(TAG, "bg back buffer alloc failed — synchronous sky rebake");
    }
    s_scene_base = heap_caps_aligned_alloc(PPA_CACHE_ALIGN,
                                           EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H * sizeof(uint16_t),
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scene_base) {
        ESP_LOGW(TAG, "scene-base buffer alloc failed — per-frame text fallback");
    }
    s_scene_slot.a8 = heap_caps_aligned_alloc(PPA_CACHE_ALIGN, TEXT_SLOT_BUF_BYTES,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_scene_slot.a8) {
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
    if (xTaskCreatePinnedToCore(lightning_task, "eva_lightning", 4096,
                                NULL, 4, NULL, 0) != pdTRUE) {
        ESP_LOGW(TAG, "lightning task create failed");
    }
#ifdef EVA_PORTRAIT_NATIVE
    ESP_LOGI(TAG, "native render path ready: portrait %ux%u direct to DPI fb (no PPA rotate)",
             (unsigned)EVA_FB_PIC_W, (unsigned)EVA_FB_PIC_H);
#else
    ESP_LOGI(TAG, "native render path ready: %ux%u landscape -> PPA rotate -> 480x800 DPI fb",
             (unsigned)EVA_WEATHER_RENDER_W, (unsigned)EVA_WEATHER_RENDER_H);
#endif
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
    }
}

void eva_weather_canvas_set_weather(const weather_state_t *st)
{
    if (!st) return;
    /* Snapshot current LIVE render values before we overwrite them — these
     * become the transition's start point (or, if a transition is already
     * running, they already hold the interpolated mid-values, so restarting
     * from them is seamless). */
    uint8_t wx_prev_cover        = s_cloud_cover_pct;
    uint8_t wx_prev_cloud_pct[3] = { s_cloud_pct[0], s_cloud_pct[1], s_cloud_pct[2] };
    uint8_t wx_prev_fog          = s_fog_pct;
    float   wx_prev_density      = s_density_scale;
    precip_type_t wx_prev_precip = s_precip_type;
    weather_kind_t kind = st->kind;
    if (kind <= WEATHER_UNKNOWN || kind >= WEATHER_KIND_COUNT) {
        kind = WEATHER_CLOUDY;
    }
    s_precip_type = st->precip_type;

    /* Nudge visual kind when WMO label and measured cloud cover disagree. */
    {
        int sr = (st->sunrise_min >= 0 && st->sunrise_min < 24 * 60) ? st->sunrise_min : 360;
        int ss = (st->sunset_min  >= 0 && st->sunset_min  < 24 * 60) ? st->sunset_min  : 1080;
        int m = minutes_now();
        bool clock_night = (m < sr || m >= ss);
        uint8_t cover = st->cloud_cover_pct;

        if (kind == WEATHER_PARTLY_CLOUDY_DAY || kind == WEATHER_PARTLY_CLOUDY_NIGHT) {
            if (cover >= 92) {
                kind = WEATHER_CLOUDY;
            }
        } else if (kind == WEATHER_CLOUDY && cover < 48) {
            kind = clock_night ? WEATHER_PARTLY_CLOUDY_NIGHT : WEATHER_PARTLY_CLOUDY_DAY;
        } else if ((kind == WEATHER_CLEAR_DAY || kind == WEATHER_CLEAR_NIGHT) && cover >= 72) {
            kind = clock_night ? WEATHER_PARTLY_CLOUDY_NIGHT : WEATHER_PARTLY_CLOUDY_DAY;
        }
    }

    float density = density_scale_from_weather(st);
    s_sunrise_min  = st->sunrise_min;
    s_sunset_min   = st->sunset_min;
    s_moonrise_min = st->moonrise_min;
    s_moonset_min  = st->moonset_min;
    /* Apply test overrides first (sliders). Live values used only if
     * the corresponding override is -1. */
    uint8_t live_high = (s_test_cloud_pct_override[CLOUD_LAYER_HIGH] >= 0)
        ? (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_HIGH] : st->cloud_high_pct;
    uint8_t live_mid  = (s_test_cloud_pct_override[CLOUD_LAYER_MID] >= 0)
        ? (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_MID]  : st->cloud_mid_pct;
    uint8_t live_low  = (s_test_cloud_pct_override[CLOUD_LAYER_LOW] >= 0)
        ? (uint8_t)s_test_cloud_pct_override[CLOUD_LAYER_LOW]  : st->cloud_low_pct;
    s_cloud_pct[CLOUD_LAYER_HIGH] = live_high;
    s_cloud_pct[CLOUD_LAYER_MID]  = live_mid;
    /* Only the LOW layer is actually rendered (single-layer pipeline), so it
     * must carry the *overall* cloudiness — otherwise overcast skies, whose
     * cover lives almost entirely in the MID/HIGH bands, would render as an
     * empty sky. Feed LOW the strongest of the three bands so the visible
     * cloud amount always matches how cloudy it really is. */
    uint8_t strongest = live_low;
    if (live_mid  > strongest) strongest = live_mid;
    if (live_high > strongest) strongest = live_high;
    s_cloud_pct[CLOUD_LAYER_LOW]  = strongest;
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

    /* --- Smooth transition decision ------------------------------------
     * At this point s_cloud_cover_pct, s_cloud_pct[], s_fog_pct, etc. have
     * ALREADY been overwritten above with the new target values, and the
     * live-vs-target lerp below re-derives the visible values each frame.
     * We captured the OLD live values into locals before the overwrite (see
     * the snapshot added just below the s_* assignments). */
    bool significant = eva_wx_change_is_significant(
        (int)s_kind, wx_prev_cover, (int)wx_prev_precip,
        (int)kind, s_cloud_cover_pct, (int)st->precip_type);

    bool first_state = (s_kind == WEATHER_UNKNOWN);

    if (!significant || first_state) {
        /* Instant apply (first state / NVS restore / temp-text-only change). */
        s_wx_trans.active = false;
        bool kind_changed = (s_kind != kind);
        s_kind = kind;
        s_density_scale = density;
        if (kind_changed) {
            s_prev_kind = WEATHER_UNKNOWN;
            s_scene_slot.valid = false;
            s_clouds3d_inited = false;
            s_frames = 0;
            s_accum_us = 0;
            s_accum_tick_us = 0;
            s_over_budget = 0;
            s_under_budget = 0;
            s_bg_ttl = 0;
            s_bg_dt = 0.0f;
        }
        return;
    }

    /* Begin (or restart) a transition. start_* = the OLD live values we
     * snapshotted; target_* = the new values already in s_*. If a transition
     * was already running, start from the CURRENT interpolated values so we
     * never snap. */
    s_wx_trans.start_cover        = wx_prev_cover;
    s_wx_trans.start_cloud_pct[0] = wx_prev_cloud_pct[0];
    s_wx_trans.start_cloud_pct[1] = wx_prev_cloud_pct[1];
    s_wx_trans.start_cloud_pct[2] = wx_prev_cloud_pct[2];
    s_wx_trans.start_fog          = wx_prev_fog;
    s_wx_trans.start_density      = wx_prev_density;

    s_wx_trans.target_cover        = s_cloud_cover_pct;
    s_wx_trans.target_cloud_pct[0] = s_cloud_pct[0];
    s_wx_trans.target_cloud_pct[1] = s_cloud_pct[1];
    s_wx_trans.target_cloud_pct[2] = s_cloud_pct[2];
    s_wx_trans.target_fog          = s_fog_pct;
    s_wx_trans.target_density      = density;

    s_wx_trans.from_kind   = (int)s_kind;
    s_wx_trans.to_kind     = (int)kind;
    s_wx_trans.from_precip = (int)wx_prev_precip;
    s_wx_trans.to_precip   = (int)st->precip_type;

    s_wx_trans.duration_s = s_wx_trans_duration_s;
    s_wx_trans.progress   = 0.0f;
    s_wx_trans.active     = true;
    s_wx_last_bake_p = -1.0f;   /* force a rebake on the first transitioning frame */

    /* Keep the render on the OLD kind/precip until the tick finalizes or
     * crosses the midpoint. Do NOT overwrite s_kind / s_precip_type here. */
    s_cloud_cover_pct = wx_prev_cover;
    s_cloud_pct[0] = wx_prev_cloud_pct[0];
    s_cloud_pct[1] = wx_prev_cloud_pct[1];
    s_cloud_pct[2] = wx_prev_cloud_pct[2];
    s_fog_pct = wx_prev_fog;
    s_density_scale = wx_prev_density;   /* start from old; tick will lerp */
    s_kind = s_kind;                     /* explicit no-op: stays old kind */
    s_precip_type = (precip_type_t)s_wx_trans.from_precip;
    {
        int desired = (int)(((long)s_cloud_cover_pct * CLOUD_3D_MAX + 50) / 100);
        if (desired < 4) desired = 4;
        if (desired > CLOUD_3D_MAX) desired = CLOUD_3D_MAX;
        s_clouds3d_active = (uint8_t)desired;
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

void eva_weather_canvas_set_transition_ms(int ms)
{
    if (ms < 0) ms = 0;
    s_wx_trans_duration_s = (float)ms / 1000.0f;
}

void eva_weather_canvas_set_clock_text(const char *text)
{
    if (!text) return;
    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_clock_text, text, sizeof(s_clock_text));
    portEXIT_CRITICAL(&s_text_mux);
}

void eva_weather_canvas_set_date_text(const char *text)
{
    if (!text) return;
    portENTER_CRITICAL(&s_text_mux);
    strlcpy(s_date_text, text, sizeof(s_date_text));
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

void eva_weather_canvas_trigger_lightning(void)
{
    s_lightning_force = true;
}

bool eva_weather_canvas_toggle_volume(void)
{
    s_cloud_volume = !s_cloud_volume;
    return s_cloud_volume;
}

bool eva_weather_canvas_copy_display(uint16_t *dst, size_t dst_bytes)
{
    size_t need = (size_t)EVA_WEATHER_RENDER_W * EVA_WEATHER_RENDER_H
                  * sizeof(uint16_t);
    if (!dst || dst_bytes < need || !s_display_buf) return false;
    /* s_display_buf aliases the live render buffer; without the render lock
     * a copy can catch the frame mid-pipeline (sky painted, clouds/text not
     * yet) — screenshots then show "empty" scenes that never hit the panel. */
    if (s_render_lock) xSemaphoreTake(s_render_lock, portMAX_DELAY);
    memcpy(dst, s_display_buf, need);
    if (s_render_lock) xSemaphoreGive(s_render_lock);
    return true;
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
    portEXIT_CRITICAL(&s_frame_mux);
}

/* Test wind override. wind_kph encodes BOTH magnitude and direction:
 *   > 0  → wind from the west, blowing east  (clouds/rain drift right)
 *   < 0  → wind from the east, blowing west  (clouds/rain drift left)
 *   special sentinel -1000 (or any value via the CDC "windclear") clears it.
 * The CDC `wind <signed_kph>` command uses this so both drift directions can
 * be exercised on demand regardless of the live weather. */
void eva_weather_canvas_set_test_wind_kph(int wind_kph)
{
    portENTER_CRITICAL(&s_frame_mux);
    if (wind_kph <= -1000) {
        s_test_wind_kph_override = -1;
    } else {
        int mag = wind_kph < 0 ? -wind_kph : wind_kph;
        if (mag > 120) mag = 120;
        float signed_kph = (wind_kph < 0) ? -(float)mag : (float)mag;
        s_test_wind_kph_override = (int16_t)mag;
        /* k_rain factor 1.6 matches the live-weather path; sign carries
         * the drift direction. */
        s_wind_vx_bias = signed_kph * 1.6f;
        s_wind_kph_eff = (float)mag;
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
