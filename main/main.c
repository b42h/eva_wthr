/*
 * Eva WEATHER firmware: weather canvas + Wi-Fi + NVS + CDC + screenshots.
 */
#include <ctype.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

#include "eva_cdc.h"
#include "eva_clock.h"
#include "eva_fps_overlay.h"
#include "eva_settings.h"
#include "eva_screenshot.h"
#include "eva_weather.h"
#include "eva_weather_canvas.h"
#include "eva_wifi.h"
#include "eva_wifi_status.h"
#include "weather_fetch.h"

#define WEATHER_KIND_HELP \
    "clear-day|clear-night|partly-cloudy-day|partly-cloudy-night|cloudy|fog|rain|heavy-rain|snow|thunderstorm|sleet|hail"

LV_FONT_DECLARE(eva_font_clock_144);
LV_FONT_DECLARE(eva_font_uk_22);
LV_FONT_DECLARE(eva_font_uk_14);

static const char *TAG = "eva";
static const char *EVA_FIRMWARE_VERSION = "1";

static eva_cdc_t *s_cdc;
static eva_settings_t *s_settings;
static eva_wifi_status_t *s_wifi_status;
static eva_fps_overlay_t *s_fps_overlay;
static eva_clock_t *s_clock;

/* Test mode: corner checkbox toggles a debug overlay where horizontal swipes
 * cycle through weather kinds and vertical swipes shift the clock by ±1 hour.
 * Off-screen the device behaves normally (real weather, real NTP time).
 *
 * s_test_hour_offset is signed minutes (multiples of 60) so we can reuse the
 * same offset path for future ±30 min increments if needed.
 *
 * s_test_weather_idx is -1 for live weather, -2 for a custom manual scene,
 * or 0..N for one of the quick demo presets. */
static lv_obj_t *s_test_checkbox;
static lv_obj_t *s_test_banner;
static bool s_test_mode;
static int s_test_hour_offset;
static int s_test_weather_idx = -1;

#define TEST_WEATHER_PRESET_LIVE   (-1)
#define TEST_WEATHER_PRESET_CUSTOM (-2)

/* Test mode control deck — scrollable touch-first panel in the top-left.
 * It drives the full scene model: raw cloud bands, wind, precipitation,
 * astronomy, clock offset, and the kind/description selectors. */
static lv_obj_t *s_test_slider_panel;
static lv_obj_t *s_test_slider_high;
static lv_obj_t *s_test_slider_mid;
static lv_obj_t *s_test_slider_low;
static lv_obj_t *s_test_slider_wind;
static lv_obj_t *s_test_slider_hour;
static lv_obj_t *s_test_slider_high_label;
static lv_obj_t *s_test_slider_mid_label;
static lv_obj_t *s_test_slider_low_label;
static lv_obj_t *s_test_slider_wind_label;
static lv_obj_t *s_test_slider_hour_label;
static lv_obj_t *s_test_kind_dropdown;
static lv_obj_t *s_test_desc_dropdown;
static lv_obj_t *s_test_temp_slider;
static lv_obj_t *s_test_feels_slider;
static lv_obj_t *s_test_cloud_cover_slider;
static lv_obj_t *s_test_cloud_total_slider;
static lv_obj_t *s_test_fog_slider;
static lv_obj_t *s_test_wind_dir_slider;
static lv_obj_t *s_test_precip_dropdown;
static lv_obj_t *s_test_precip_slider;
static lv_obj_t *s_test_sunrise_slider;
static lv_obj_t *s_test_sunset_slider;
static lv_obj_t *s_test_moonrise_slider;
static lv_obj_t *s_test_moonset_slider;
static lv_obj_t *s_test_moon_phase_slider;
static lv_obj_t *s_test_moon_waning_switch;
static lv_obj_t *s_test_preset_dropdown;
static lv_obj_t *s_test_advanced_toggle;
static lv_obj_t *s_test_advanced_panel;
static lv_obj_t *s_test_advanced_toggle_label;
static lv_obj_t *s_test_temp_label;
static lv_obj_t *s_test_feels_label;
static lv_obj_t *s_test_cloud_cover_label;
static lv_obj_t *s_test_cloud_total_label;
static lv_obj_t *s_test_fog_label;
static lv_obj_t *s_test_wind_dir_label;
static lv_obj_t *s_test_precip_mm_label;
static lv_obj_t *s_test_sunrise_label;
static lv_obj_t *s_test_sunset_label;
static lv_obj_t *s_test_moonrise_label;
static lv_obj_t *s_test_moonset_label;
static lv_obj_t *s_test_moon_phase_label;
static lv_obj_t *s_test_moon_waning_label;

static const weather_kind_t s_test_weather_cycle[] = {
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
#define TEST_WEATHER_CYCLE_LEN ((int)(sizeof(s_test_weather_cycle) / sizeof(s_test_weather_cycle[0])))

typedef enum {
    TEST_DESC_AUTO = 0,
    TEST_DESC_KIND,
    TEST_DESC_CLEAR,
    TEST_DESC_PARTLY,
    TEST_DESC_CLOUDY,
    TEST_DESC_FOG,
    TEST_DESC_RAIN,
    TEST_DESC_HEAVY_RAIN,
    TEST_DESC_SNOW,
    TEST_DESC_THUNDER,
    TEST_DESC_SLEET,
    TEST_DESC_HAIL,
} test_desc_mode_t;

static const char *const s_kind_options =
    "Авто з raw\n"
    "Ясно день\n"
    "Ясна ніч\n"
    "Мінлива день\n"
    "Мінлива ніч\n"
    "Хмарно\n"
    "Туман\n"
    "Дощ\n"
    "Злива\n"
    "Сніг\n"
    "Гроза\n"
    "Мокрий сніг\n"
    "Град";

/* Kept for the simplified test panel — no longer referenced after removing
 * the desc / precip / preset dropdowns, but useful for re-enabling them
 * later. Mark as __attribute__((unused)) to keep -Werror=unused-const happy. */
__attribute__((unused))
static const char *const s_desc_options =
    "Авто з kind\n"
    "За kind\n"
    "Ясно\n"
    "Мінлива хмарність\n"
    "Хмарно\n"
    "Туман\n"
    "Дощ\n"
    "Злива\n"
    "Сніг\n"
    "Гроза\n"
    "Мокрий сніг\n"
    "Град";

__attribute__((unused))
static const char *const s_precip_options =
    "Немає\n"
    "Дрібний дощ\n"
    "Малий дощ\n"
    "Дощ\n"
    "Злива\n"
    "Сніг\n"
    "Мокрий сніг\n"
    "Град\n"
    "Гроза";

__attribute__((unused))
static const char *const s_preset_options =
    "Живе\n"
    "Свій\n"
    "Ясно день\n"
    "Мінлива день\n"
    "Хмарно\n"
    "Туман\n"
    "Дощ\n"
    "Злива\n"
    "Гроза\n"
    "Сніг\n"
    "Мокрий сніг\n"
    "Град\n"
    "Ясно ніч\n"
    "Мінлива ніч";

static weather_state_t s_test_state;
static bool s_test_kind_auto;
static test_desc_mode_t s_test_desc_mode = TEST_DESC_AUTO;
static bool s_test_ui_syncing;
static weather_kind_t s_test_manual_kind = WEATHER_CLEAR_DAY;
static bool s_test_advanced_visible;

#define CLOCK_SOLAR_FALLBACK_SUNRISE_MIN  (6 * 60)
#define CLOCK_SOLAR_FALLBACK_SUNSET_MIN   (20 * 60)
#define CLOCK_SOLAR_PI 3.1415926f
#define CLOCK_SCALE_MIN 154
#define CLOCK_SCALE_NIGHT 150
#define CLOCK_SCALE_MAX 390

typedef struct {
    bool day;
    int sunrise_min;
    int sunset_min;
    int apex_min;
    float progress;
    float elevation;
    float sun_x_norm;
    float clear_sky;  /* 1.0 = totally clear, 0.0 = overcast. Scales the halo. */
} clock_solar_t;

static void trim_trailing(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

static char *trim_in_place(char *s)
{
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    trim_trailing(s);
    return s;
}

static void apply_timezone(void)
{
    setenv("TZ", eva_settings_get_tz(s_settings), 1);
    tzset();
}

static void cdc_send(const char *text)
{
    eva_cdc_send(s_cdc, text);
}

static void cdc_sendf(const char *fmt, ...)
{
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cdc_send(buf);
}

static void cdc_send_binary(const uint8_t *data, size_t size)
{
    eva_cdc_send_binary(s_cdc, data, size);
}

/* Defined further down; the CDC handler dispatches through lv_async_call so
 * it must see the prototype. */
static void set_test_mode(bool on);

/* Called via lv_async_call from CDC so set_test_mode() runs on the LVGL
 * thread. user is encoded as 1 = enable, 0 = disable. */
static void test_mode_set_async(void *user)
{
    bool on = ((uintptr_t)user) != 0;
    if (s_test_checkbox) {
        if (on) lv_obj_add_state(s_test_checkbox, LV_STATE_CHECKED);
        else    lv_obj_remove_state(s_test_checkbox, LV_STATE_CHECKED);
    }
    set_test_mode(on);
}

static void cdc_handle_screenshot(void)
{
    const uint8_t *jpg = NULL;
    size_t jpg_size = 0;
    esp_err_t err = eva_screenshot_capture(&jpg, &jpg_size);
    if (err != ESP_OK || !jpg || jpg_size == 0) {
        cdc_sendf("ERR screenshot capture_failed err=0x%x size=%u\r\n",
                  (unsigned)err, (unsigned)jpg_size);
        return;
    }
    cdc_sendf("BEGIN_SCREENSHOT %u\r\n", (unsigned)jpg_size);
    cdc_send_binary(jpg, jpg_size);
    cdc_send("\r\nEND_SCREENSHOT\r\n");
}

static const char *log_level_to_str(esp_log_level_t level)
{
    switch (level) {
    case ESP_LOG_NONE:    return "none";
    case ESP_LOG_ERROR:   return "error";
    case ESP_LOG_WARN:    return "warn";
    case ESP_LOG_INFO:    return "info";
    case ESP_LOG_DEBUG:   return "debug";
    case ESP_LOG_VERBOSE: return "verbose";
    default:              return "unknown";
    }
}

static bool parse_log_level(const char *s, esp_log_level_t *out)
{
    if (strcmp(s, "none") == 0 || strcmp(s, "off") == 0) {
        *out = ESP_LOG_NONE;
        return true;
    }
    if (strcmp(s, "error") == 0 || strcmp(s, "e") == 0) {
        *out = ESP_LOG_ERROR;
        return true;
    }
    if (strcmp(s, "warn") == 0 || strcmp(s, "warning") == 0 || strcmp(s, "w") == 0) {
        *out = ESP_LOG_WARN;
        return true;
    }
    if (strcmp(s, "info") == 0 || strcmp(s, "i") == 0) {
        *out = ESP_LOG_INFO;
        return true;
    }
    if (strcmp(s, "debug") == 0 || strcmp(s, "d") == 0) {
        *out = ESP_LOG_DEBUG;
        return true;
    }
    if (strcmp(s, "verbose") == 0 || strcmp(s, "v") == 0) {
        *out = ESP_LOG_VERBOSE;
        return true;
    }
    return false;
}

static bool parse_precip_type(const char *s, precip_type_t *out)
{
    if (!s || !out) return false;
    for (int i = PRECIP_NONE; i <= PRECIP_THUNDER; ++i) {
        precip_type_t p = (precip_type_t)i;
        if (strcmp(s, precip_type_name(p)) == 0) {
            *out = p;
            return true;
        }
    }
    return false;
}

static uint8_t clamp_pct_i(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return (uint8_t)v;
}

static void unquote_desc(char *s)
{
    s = trim_in_place(s);
    size_t n = strlen(s);
    if (n >= 2 && s[0] == '"' && s[n - 1] == '"') {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

static int minutes_from_clock_now(const struct tm *tm_now)
{
    return tm_now->tm_hour * 60 + tm_now->tm_min;
}

static void clock_solar_state(int now_min, const weather_state_t *st, clock_solar_t *out)
{
    int sr = CLOCK_SOLAR_FALLBACK_SUNRISE_MIN;
    int ss = CLOCK_SOLAR_FALLBACK_SUNSET_MIN;
    if (st && st->sunrise_min >= 0 && st->sunset_min > st->sunrise_min) {
        sr = st->sunrise_min;
        ss = st->sunset_min;
    }

    int daylight = ss - sr;
    if (daylight < 60) {
        sr = CLOCK_SOLAR_FALLBACK_SUNRISE_MIN;
        ss = CLOCK_SOLAR_FALLBACK_SUNSET_MIN;
        daylight = ss - sr;
    }

    memset(out, 0, sizeof(*out));
    out->sunrise_min = sr;
    out->sunset_min = ss;
    /* Solar apex/noon for the clock is the midpoint between sunrise and
     * sunset. With the fallback 06:00-20:00 window this lands exactly at
     * 13:00; live weather usually nudges it around 13:00-13:20 in summer. */
    out->apex_min = sr + daylight / 2;
    out->sun_x_norm = now_min < out->apex_min ? 0.08f : 0.92f;

    if (now_min < sr || now_min >= ss) {
        return;
    }

    out->day = true;
    out->progress = (float)(now_min - sr) / (float)daylight;
    if (out->progress < 0.0f) out->progress = 0.0f;
    if (out->progress > 1.0f) out->progress = 1.0f;
    out->elevation = sinf(out->progress * CLOCK_SOLAR_PI);
    if (out->elevation < 0.0f) out->elevation = 0.0f;
    out->sun_x_norm = 0.08f + out->progress * 0.84f;

    /* clear_sky: 1.0 when sky is totally clear, fading to 0.0 at overcast.
     * The halo is only meaningful when the sun is actually shining through. */
    int cloud_pct = st ? (int)st->cloud_cover_pct : 0;
    if (cloud_pct < 0) cloud_pct = 0;
    if (cloud_pct > 100) cloud_pct = 100;
    out->clear_sky = 1.0f - (float)cloud_pct / 100.0f;
}

static void restore_scene_depth_order(void)
{
    eva_wifi_status_move_foreground(s_wifi_status);
}

static void update_weather_labels(const weather_state_t *st)
{
    if (!st) return;
    char temp_buf[16];
    snprintf(temp_buf, sizeof(temp_buf), "%+dC", (int)st->temp_c);
    eva_weather_canvas_set_temp_text(temp_buf);
    eva_weather_canvas_set_desc_text(st->desc);
    restore_scene_depth_order();
}

static void weather_update_cb(const weather_state_t *st, void *user)
{
    (void)user;
    if (!st) return;
    /* While test mode is on we ignore live fetches so the user's selection
     * isn't clobbered the next time the fetcher ticks. The fetcher keeps
     * running in the background and will resume driving the UI as soon as
     * test mode is turned off. */
    if (s_test_mode) return;
    update_weather_labels(st);
    eva_weather_canvas_set_weather(st);
    eva_weather_canvas_show(true);
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int clamp_minute_of_day(int v)
{
    if (v < -1) return -1;
    if (v > 1439) return 1439;
    return v;
}

static void minutes_to_text(int minutes, char *buf, size_t size)
{
    if (minutes < 0) {
        snprintf(buf, size, "--:--");
        return;
    }
    snprintf(buf, size, "%02d:%02d", minutes / 60, minutes % 60);
}

static precip_type_t precip_from_index(uint32_t idx)
{
    switch (idx) {
    case 1: return PRECIP_DRIZZLE;
    case 2: return PRECIP_LIGHT_RAIN;
    case 3: return PRECIP_RAIN;
    case 4: return PRECIP_HEAVY_RAIN;
    case 5: return PRECIP_SNOW;
    case 6: return PRECIP_SLEET;
    case 7: return PRECIP_HAIL;
    case 8: return PRECIP_THUNDER;
    default: return PRECIP_NONE;
    }
}

static const char *precip_label_uk(precip_type_t p)
{
    switch (p) {
    case PRECIP_NONE:        return "Немає";
    case PRECIP_DRIZZLE:     return "Мряка";
    case PRECIP_LIGHT_RAIN:  return "Малий дощ";
    case PRECIP_RAIN:        return "Дощ";
    case PRECIP_HEAVY_RAIN:  return "Злива";
    case PRECIP_SNOW:        return "Сніг";
    case PRECIP_SLEET:       return "Мокрий сніг";
    case PRECIP_HAIL:        return "Град";
    case PRECIP_THUNDER:     return "Гроза";
    default:                 return "Невідомо";
    }
}

static const char *desc_mode_label(test_desc_mode_t mode)
{
    switch (mode) {
    case TEST_DESC_AUTO:       return "авто raw";
    case TEST_DESC_KIND:       return "за kind";
    case TEST_DESC_CLEAR:      return "Ясно";
    case TEST_DESC_PARTLY:     return "Мінлива хмарність";
    case TEST_DESC_CLOUDY:     return "Хмарно";
    case TEST_DESC_FOG:        return "Туман";
    case TEST_DESC_RAIN:       return "Дощ";
    case TEST_DESC_HEAVY_RAIN: return "Злива";
    case TEST_DESC_SNOW:       return "Сніг";
    case TEST_DESC_THUNDER:    return "Гроза";
    case TEST_DESC_SLEET:      return "Мокрий сніг";
    case TEST_DESC_HAIL:       return "Град";
    default:                   return "невідомо";
    }
}

static uint32_t kind_dropdown_index_from_kind(weather_kind_t kind)
{
    switch (kind) {
    case WEATHER_CLEAR_DAY:           return 1;
    case WEATHER_CLEAR_NIGHT:         return 2;
    case WEATHER_PARTLY_CLOUDY_DAY:   return 3;
    case WEATHER_PARTLY_CLOUDY_NIGHT: return 4;
    case WEATHER_CLOUDY:              return 5;
    case WEATHER_FOG:                 return 6;
    case WEATHER_RAIN:                return 7;
    case WEATHER_HEAVY_RAIN:         return 8;
    case WEATHER_SNOW:                return 9;
    case WEATHER_THUNDERSTORM:       return 10;
    case WEATHER_SLEET:              return 11;
    case WEATHER_HAIL:               return 12;
    default:                         return 0;
    }
}

static weather_kind_t kind_from_dropdown_index(uint32_t idx)
{
    static const weather_kind_t kinds[] = {
        WEATHER_CLEAR_DAY,
        WEATHER_CLEAR_NIGHT,
        WEATHER_PARTLY_CLOUDY_DAY,
        WEATHER_PARTLY_CLOUDY_NIGHT,
        WEATHER_CLOUDY,
        WEATHER_FOG,
        WEATHER_RAIN,
        WEATHER_HEAVY_RAIN,
        WEATHER_SNOW,
        WEATHER_THUNDERSTORM,
        WEATHER_SLEET,
        WEATHER_HAIL,
    };
    if (idx == 0) return WEATHER_CLEAR_DAY;
    idx -= 1;
    if (idx >= sizeof(kinds) / sizeof(kinds[0])) return WEATHER_CLEAR_DAY;
    return kinds[idx];
}

static const char *desc_from_mode(test_desc_mode_t mode, const weather_state_t *st, bool is_night)
{
    weather_kind_t raw_kind = (st && st->kind > WEATHER_UNKNOWN && st->kind < WEATHER_KIND_COUNT)
        ? st->kind : WEATHER_CLOUDY;

    switch (mode) {
    case TEST_DESC_KIND:
        return weather_kind_label_uk(raw_kind);
    case TEST_DESC_CLEAR:       return "Ясно";
    case TEST_DESC_PARTLY:      return "Мінлива хмарність";
    case TEST_DESC_CLOUDY:      return "Хмарно";
    case TEST_DESC_FOG:         return "Туман";
    case TEST_DESC_RAIN:        return "Дощ";
    case TEST_DESC_HEAVY_RAIN:  return "Злива";
    case TEST_DESC_SNOW:        return "Сніг";
    case TEST_DESC_THUNDER:     return "Гроза";
    case TEST_DESC_SLEET:       return "Мокрий сніг";
    case TEST_DESC_HAIL:        return "Град";
    case TEST_DESC_AUTO:
    default:
        if (!st) return weather_kind_label_uk(raw_kind);
        return weather_kind_label_uk(derive_kind_from_raw(st, is_night));
    }
}

static void test_seed_from_live(void)
{
    weather_state_t live = { 0 };
    if (!eva_weather_copy(&live)) {
        const weather_state_t *cur = eva_weather_get();
        if (cur) live = *cur;
    }
    s_test_state = live;
    if (s_test_state.kind <= WEATHER_UNKNOWN || s_test_state.kind >= WEATHER_KIND_COUNT) {
        s_test_state.kind = WEATHER_CLEAR_DAY;
    }
    if (!s_test_state.desc[0]) {
        strlcpy(s_test_state.desc, weather_kind_label_uk(s_test_state.kind), sizeof(s_test_state.desc));
    }
    s_test_kind_auto = true;
    s_test_manual_kind = s_test_state.kind;
    s_test_desc_mode = TEST_DESC_AUTO;
}

static void update_test_banner(void);
static void test_refresh_control_labels(void);
static void test_sync_controls_from_state(void);

static void test_apply_scene(void)
{
    weather_state_t st = s_test_state;
    eva_weather_canvas_set_time_offset(s_test_hour_offset);
    st.temp_c = clamp_i(st.temp_c, -50, 50);
    st.feels_like_c = clamp_i(st.feels_like_c, -50, 50);
    st.wind_kph = clamp_i(st.wind_kph, 0, 120);
    st.wind_dir_deg = clamp_i(st.wind_dir_deg, -1, 359);
    st.cloud_low_pct = (uint8_t)clamp_i(st.cloud_low_pct, 0, 100);
    st.cloud_mid_pct = (uint8_t)clamp_i(st.cloud_mid_pct, 0, 100);
    st.cloud_high_pct = (uint8_t)clamp_i(st.cloud_high_pct, 0, 100);
    st.cloud_total_pct = (uint8_t)clamp_i(st.cloud_total_pct, 0, 100);
    st.cloud_cover_pct = (uint8_t)clamp_i(st.cloud_cover_pct, 0, 100);
    st.fog_pct = (uint8_t)clamp_i(st.fog_pct, 0, 100);
    st.precip_mm_x10 = (uint16_t)clamp_i((int)st.precip_mm_x10, 0, 65535);
    st.sunrise_min = (int16_t)clamp_minute_of_day(st.sunrise_min);
    st.sunset_min  = (int16_t)clamp_minute_of_day(st.sunset_min);
    st.moonrise_min = (int16_t)clamp_minute_of_day(st.moonrise_min);
    st.moonset_min  = (int16_t)clamp_minute_of_day(st.moonset_min);
    st.moon_phase_pct = (uint8_t)clamp_i(st.moon_phase_pct, 0, 100);
    st.moon_waning = st.moon_waning ? 1 : 0;

    time_t now = time(NULL);
    if (now > 1700000000) {
        now += (time_t)s_test_hour_offset * 3600;
    }
    struct tm tm_now;
    memset(&tm_now, 0, sizeof(tm_now));
    if (now > 1700000000) {
        localtime_r(&now, &tm_now);
    } else {
        int total_s = (int)((esp_timer_get_time() / 1000000) % (24 * 3600));
        total_s += s_test_hour_offset * 3600;
        total_s %= (24 * 3600);
        if (total_s < 0) {
            total_s += 24 * 3600;
        }
        tm_now.tm_hour = total_s / 3600;
        tm_now.tm_min = (total_s / 60) % 60;
    }
    int now_min = tm_now.tm_hour * 60 + tm_now.tm_min;
    bool is_night = (now_min < 360 || now_min >= 1080);
    if (st.sunrise_min >= 0 && st.sunset_min > st.sunrise_min) {
        is_night = (now_min < st.sunrise_min || now_min >= st.sunset_min);
    }

    if (s_test_kind_auto) {
        st.kind = derive_kind_from_raw(&st, is_night);
    } else {
        st.kind = s_test_manual_kind;
    }
    if (st.kind <= WEATHER_UNKNOWN || st.kind >= WEATHER_KIND_COUNT) {
        st.kind = WEATHER_CLOUDY;
    }

    strlcpy(st.desc, desc_from_mode(s_test_desc_mode, &st, is_night), sizeof(st.desc));
    st.fetched_at = now > 1700000000 ? now : time(NULL);

    s_test_state = st;

    eva_weather_canvas_clear_test_overrides();
    eva_weather_canvas_set_weather(&st);
    eva_weather_canvas_show(true);
    update_weather_labels(&st);
    test_refresh_control_labels();
    update_test_banner();
}

static void apply_test_weather(void)
{
    if (s_test_weather_idx < 0 || s_test_weather_idx >= TEST_WEATHER_CYCLE_LEN) return;
    weather_kind_t kind = s_test_weather_cycle[s_test_weather_idx];
    test_seed_from_live();
    s_test_kind_auto = false;
    s_test_manual_kind = kind;
    s_test_desc_mode = TEST_DESC_AUTO;

    switch (kind) {
    case WEATHER_CLEAR_DAY:
        s_test_state.temp_c = 24;
        s_test_state.feels_like_c = 24;
        s_test_state.wind_kph = 8;
        s_test_state.wind_dir_deg = 90;
        s_test_state.cloud_cover_pct = 5;
        s_test_state.cloud_total_pct = 5;
        s_test_state.cloud_low_pct = 0;
        s_test_state.cloud_mid_pct = 0;
        s_test_state.cloud_high_pct = 0;
        s_test_state.fog_pct = 0;
        s_test_state.precip_type = PRECIP_NONE;
        s_test_state.precip_mm_x10 = 0;
        break;
    case WEATHER_CLEAR_NIGHT:
        s_test_state.temp_c = 18;
        s_test_state.feels_like_c = 16;
        s_test_state.wind_kph = 4;
        s_test_state.wind_dir_deg = 120;
        s_test_state.cloud_cover_pct = 5;
        s_test_state.cloud_total_pct = 5;
        s_test_state.cloud_low_pct = 0;
        s_test_state.cloud_mid_pct = 0;
        s_test_state.cloud_high_pct = 0;
        s_test_state.fog_pct = 0;
        s_test_state.precip_type = PRECIP_NONE;
        s_test_state.precip_mm_x10 = 0;
        break;
    case WEATHER_PARTLY_CLOUDY_DAY:
    case WEATHER_PARTLY_CLOUDY_NIGHT:
        s_test_state.temp_c = 22;
        s_test_state.feels_like_c = 21;
        s_test_state.wind_kph = 12;
        s_test_state.wind_dir_deg = 140;
        s_test_state.cloud_cover_pct = 45;
        s_test_state.cloud_total_pct = 45;
        s_test_state.cloud_low_pct = 35;
        s_test_state.cloud_mid_pct = 45;
        s_test_state.cloud_high_pct = 25;
        s_test_state.fog_pct = 0;
        s_test_state.precip_type = PRECIP_NONE;
        s_test_state.precip_mm_x10 = 0;
        break;
    case WEATHER_CLOUDY:
        s_test_state.temp_c = 14;
        s_test_state.feels_like_c = 12;
        s_test_state.wind_kph = 16;
        s_test_state.wind_dir_deg = 180;
        s_test_state.cloud_cover_pct = 90;
        s_test_state.cloud_total_pct = 90;
        s_test_state.cloud_low_pct = 85;
        s_test_state.cloud_mid_pct = 80;
        s_test_state.cloud_high_pct = 60;
        s_test_state.fog_pct = 5;
        s_test_state.precip_type = PRECIP_NONE;
        s_test_state.precip_mm_x10 = 0;
        break;
    case WEATHER_FOG:
        s_test_state.temp_c = 8;
        s_test_state.feels_like_c = 7;
        s_test_state.wind_kph = 3;
        s_test_state.wind_dir_deg = 0;
        s_test_state.cloud_cover_pct = 100;
        s_test_state.cloud_total_pct = 100;
        s_test_state.cloud_low_pct = 100;
        s_test_state.cloud_mid_pct = 95;
        s_test_state.cloud_high_pct = 90;
        s_test_state.fog_pct = 100;
        s_test_state.precip_type = PRECIP_NONE;
        s_test_state.precip_mm_x10 = 0;
        break;
    case WEATHER_RAIN:
        s_test_state.temp_c = 11;
        s_test_state.feels_like_c = 9;
        s_test_state.wind_kph = 22;
        s_test_state.wind_dir_deg = 250;
        s_test_state.cloud_cover_pct = 95;
        s_test_state.cloud_total_pct = 95;
        s_test_state.cloud_low_pct = 95;
        s_test_state.cloud_mid_pct = 85;
        s_test_state.cloud_high_pct = 75;
        s_test_state.fog_pct = 10;
        s_test_state.precip_type = PRECIP_RAIN;
        s_test_state.precip_mm_x10 = 24;
        break;
    case WEATHER_HEAVY_RAIN:
        s_test_state.temp_c = 9;
        s_test_state.feels_like_c = 7;
        s_test_state.wind_kph = 30;
        s_test_state.wind_dir_deg = 260;
        s_test_state.cloud_cover_pct = 100;
        s_test_state.cloud_total_pct = 100;
        s_test_state.cloud_low_pct = 100;
        s_test_state.cloud_mid_pct = 95;
        s_test_state.cloud_high_pct = 90;
        s_test_state.fog_pct = 15;
        s_test_state.precip_type = PRECIP_HEAVY_RAIN;
        s_test_state.precip_mm_x10 = 48;
        break;
    case WEATHER_SNOW:
        s_test_state.temp_c = -3;
        s_test_state.feels_like_c = -7;
        s_test_state.wind_kph = 14;
        s_test_state.wind_dir_deg = 320;
        s_test_state.cloud_cover_pct = 90;
        s_test_state.cloud_total_pct = 90;
        s_test_state.cloud_low_pct = 85;
        s_test_state.cloud_mid_pct = 90;
        s_test_state.cloud_high_pct = 80;
        s_test_state.fog_pct = 8;
        s_test_state.precip_type = PRECIP_SNOW;
        s_test_state.precip_mm_x10 = 16;
        break;
    case WEATHER_THUNDERSTORM:
        s_test_state.temp_c = 23;
        s_test_state.feels_like_c = 26;
        s_test_state.wind_kph = 38;
        s_test_state.wind_dir_deg = 230;
        s_test_state.cloud_cover_pct = 100;
        s_test_state.cloud_total_pct = 100;
        s_test_state.cloud_low_pct = 100;
        s_test_state.cloud_mid_pct = 100;
        s_test_state.cloud_high_pct = 95;
        s_test_state.fog_pct = 0;
        s_test_state.precip_type = PRECIP_THUNDER;
        s_test_state.precip_mm_x10 = 55;
        break;
    case WEATHER_SLEET:
        s_test_state.temp_c = 2;
        s_test_state.feels_like_c = -1;
        s_test_state.wind_kph = 18;
        s_test_state.wind_dir_deg = 300;
        s_test_state.cloud_cover_pct = 95;
        s_test_state.cloud_total_pct = 95;
        s_test_state.cloud_low_pct = 90;
        s_test_state.cloud_mid_pct = 90;
        s_test_state.cloud_high_pct = 80;
        s_test_state.fog_pct = 10;
        s_test_state.precip_type = PRECIP_SLEET;
        s_test_state.precip_mm_x10 = 20;
        break;
    case WEATHER_HAIL:
        s_test_state.temp_c = 7;
        s_test_state.feels_like_c = 4;
        s_test_state.wind_kph = 26;
        s_test_state.wind_dir_deg = 290;
        s_test_state.cloud_cover_pct = 95;
        s_test_state.cloud_total_pct = 95;
        s_test_state.cloud_low_pct = 95;
        s_test_state.cloud_mid_pct = 90;
        s_test_state.cloud_high_pct = 85;
        s_test_state.fog_pct = 5;
        s_test_state.precip_type = PRECIP_HAIL;
        s_test_state.precip_mm_x10 = 12;
        break;
    default:
        break;
    }
    snprintf(s_test_state.desc, sizeof(s_test_state.desc), "%s (тест)", weather_kind_label_uk(kind));
    test_sync_controls_from_state();
    test_apply_scene();
}

static void update_test_banner(void);
static void test_refresh_control_labels(void);
static void test_slider_panel_show(bool show);

typedef enum {
    TEST_NUM_TEMP = 0,
    TEST_NUM_FEELS,
    TEST_NUM_COVER,
    TEST_NUM_TOTAL,
    TEST_NUM_CLOUD_HIGH,
    TEST_NUM_CLOUD_MID,
    TEST_NUM_CLOUD_LOW,
    TEST_NUM_FOG,
    TEST_NUM_WIND_DIR,
    TEST_NUM_WIND_SPEED,
    TEST_NUM_PRECIP_MM,
    TEST_NUM_SUNRISE,
    TEST_NUM_SUNSET,
    TEST_NUM_MOONRISE,
    TEST_NUM_MOONSET,
    TEST_NUM_MOON_PHASE,
    TEST_NUM_HOUR_OFFSET,
} test_numeric_field_t;

typedef enum {
    TEST_DD_PRESET = 0,
    TEST_DD_KIND,
    TEST_DD_DESC,
    TEST_DD_PRECIP,
} test_dropdown_field_t;

typedef enum {
    TEST_SW_MOON_WANING = 0,
} test_switch_field_t;

static void test_numeric_slider_cb(lv_event_t *e)
{
    if (s_test_ui_syncing) return;
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);
    int v = lv_slider_get_value(target);

    switch ((test_numeric_field_t)id) {
    case TEST_NUM_TEMP:       s_test_state.temp_c = (int16_t)v; break;
    case TEST_NUM_FEELS:      s_test_state.feels_like_c = (int16_t)v; break;
    case TEST_NUM_COVER:      s_test_state.cloud_cover_pct = (uint8_t)clamp_i(v, 0, 100); break;
    case TEST_NUM_TOTAL:      s_test_state.cloud_total_pct = (uint8_t)clamp_i(v, 0, 100); break;
    case TEST_NUM_CLOUD_HIGH: s_test_state.cloud_high_pct = (uint8_t)clamp_i(v, 0, 100); s_test_state.cloud_total_pct = (uint8_t)clamp_i((s_test_state.cloud_high_pct + s_test_state.cloud_mid_pct + s_test_state.cloud_low_pct) / 3, 0, 100); break;
    case TEST_NUM_CLOUD_MID:  s_test_state.cloud_mid_pct = (uint8_t)clamp_i(v, 0, 100); s_test_state.cloud_total_pct = (uint8_t)clamp_i((s_test_state.cloud_high_pct + s_test_state.cloud_mid_pct + s_test_state.cloud_low_pct) / 3, 0, 100); break;
    case TEST_NUM_CLOUD_LOW:  s_test_state.cloud_low_pct = (uint8_t)clamp_i(v, 0, 100); s_test_state.cloud_total_pct = (uint8_t)clamp_i((s_test_state.cloud_high_pct + s_test_state.cloud_mid_pct + s_test_state.cloud_low_pct) / 3, 0, 100); break;
    case TEST_NUM_FOG:        s_test_state.fog_pct = (uint8_t)clamp_i(v, 0, 100); break;
    case TEST_NUM_WIND_DIR:   s_test_state.wind_dir_deg = (int16_t)clamp_i(v, -1, 359); break;
    case TEST_NUM_WIND_SPEED:  s_test_state.wind_kph = (int16_t)clamp_i(v, 0, 120); break;
    case TEST_NUM_PRECIP_MM:   s_test_state.precip_mm_x10 = (uint16_t)clamp_i(v, 0, 65535); break;
    case TEST_NUM_SUNRISE:     s_test_state.sunrise_min = (int16_t)clamp_minute_of_day(v); break;
    case TEST_NUM_SUNSET:      s_test_state.sunset_min = (int16_t)clamp_minute_of_day(v); break;
    case TEST_NUM_MOONRISE:    s_test_state.moonrise_min = (int16_t)clamp_minute_of_day(v); break;
    case TEST_NUM_MOONSET:     s_test_state.moonset_min = (int16_t)clamp_minute_of_day(v); break;
    case TEST_NUM_MOON_PHASE:  s_test_state.moon_phase_pct = (uint8_t)clamp_i(v, 0, 100); break;
    case TEST_NUM_HOUR_OFFSET:
        s_test_hour_offset = v;
        if (s_clock) eva_clock_set_hour_offset(s_clock, s_test_hour_offset);
        eva_weather_canvas_set_time_offset(s_test_hour_offset);
        break;
    default: return;
    }
    s_test_weather_idx = TEST_WEATHER_PRESET_CUSTOM;
    test_apply_scene();
}

static void test_dropdown_cb(lv_event_t *e)
{
    if (s_test_ui_syncing) return;
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    uint32_t sel = lv_dropdown_get_selected(lv_event_get_target(e));

    switch ((test_dropdown_field_t)id) {
    case TEST_DD_PRESET:
        if (sel == 0) {
            s_test_weather_idx = TEST_WEATHER_PRESET_LIVE;
            s_test_kind_auto = true;
            s_test_desc_mode = TEST_DESC_AUTO;
            test_seed_from_live();
            test_sync_controls_from_state();
            test_apply_scene();
        } else if (sel == 1) {
            s_test_weather_idx = TEST_WEATHER_PRESET_CUSTOM;
            test_sync_controls_from_state();
            test_apply_scene();
        } else {
            s_test_weather_idx = (int)sel - 2;
            apply_test_weather();
        }
        return;
    case TEST_DD_KIND:
        if (sel == 0) {
            s_test_kind_auto = true;
        } else {
            s_test_kind_auto = false;
            s_test_manual_kind = kind_from_dropdown_index(sel);
        }
        s_test_weather_idx = TEST_WEATHER_PRESET_CUSTOM;
        break;
    case TEST_DD_DESC:
        s_test_desc_mode = (test_desc_mode_t)clamp_i((int)sel, 0, TEST_DESC_HAIL);
        s_test_weather_idx = TEST_WEATHER_PRESET_CUSTOM;
        break;
    case TEST_DD_PRECIP:
        s_test_state.precip_type = precip_from_index(sel);
        s_test_weather_idx = TEST_WEATHER_PRESET_CUSTOM;
        break;
    default:
        return;
    }
    test_apply_scene();
}

static void test_switch_cb(lv_event_t *e)
{
    if (s_test_ui_syncing) return;
    intptr_t id = (intptr_t)lv_event_get_user_data(e);
    lv_obj_t *target = lv_event_get_target(e);

    switch ((test_switch_field_t)id) {
    case TEST_SW_MOON_WANING:
        s_test_state.moon_waning = lv_obj_has_state(target, LV_STATE_CHECKED) ? 1 : 0;
        s_test_weather_idx = TEST_WEATHER_PRESET_CUSTOM;
        break;
    default:
        return;
    }
    test_apply_scene();
}

static void test_advanced_toggle_cb(lv_event_t *e)
{
    if (s_test_ui_syncing) return;
    (void)lv_event_get_user_data(e);
    s_test_advanced_visible = !s_test_advanced_visible;
    if (s_test_advanced_panel) {
        if (s_test_advanced_visible) {
            lv_obj_remove_flag(s_test_advanced_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_test_advanced_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_test_advanced_toggle_label) {
        lv_label_set_text(s_test_advanced_toggle_label,
                          s_test_advanced_visible ? "сховати додаткові" : "показати додаткові");
    }
}

static void test_advanced_panel_set_visible(bool visible)
{
    s_test_advanced_visible = visible;
    if (s_test_advanced_panel) {
        if (visible) {
            lv_obj_remove_flag(s_test_advanced_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_test_advanced_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (visible && s_test_advanced_toggle) {
        lv_obj_move_foreground(s_test_advanced_toggle);
    }
    if (s_test_advanced_toggle_label) {
        lv_label_set_text(s_test_advanced_toggle_label,
                          visible ? "сховати додаткові" : "показати додаткові");
    }
}

static void style_floating_card(lv_obj_t *obj, uint32_t bg_hex, uint32_t border_hex);

static lv_obj_t *test_make_card(lv_obj_t *parent, uint32_t bg_hex, uint32_t border_hex)
{
    /* Compact spacing so each slider/dropdown row sits at ~52 px instead of
     * the previous ~80 px. Five rows + title + dropdown fit in 420 px. */
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    style_floating_card(card, bg_hex, border_hex);
    lv_obj_set_style_bg_opa(card, LV_OPA_50, 0);
    lv_obj_set_style_pad_all(card, 6, 0);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_set_style_pad_column(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *test_section_create(lv_obj_t *parent, const char *title)
{
    lv_obj_t *section = test_make_card(parent, 0x151b25, 0x2e3d53);
    lv_obj_set_width(section, LV_PCT(100));
    lv_obj_set_flex_flow(section, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(section, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *hdr = lv_label_create(section);
    lv_obj_set_style_text_font(hdr, &eva_font_uk_22, 0);
    lv_obj_set_style_text_color(hdr, lv_color_hex(0xffe6ad), 0);
    lv_obj_set_style_text_opa(hdr, LV_OPA_90, 0);
    lv_label_set_text(hdr, title);
    return section;
}

static lv_obj_t *test_slider_row_create(lv_obj_t *parent, const char *title,
                                        int min, int max, int init,
                                        test_numeric_field_t field,
                                        lv_obj_t **slider_out,
                                        lv_obj_t **label_out)
{
    lv_obj_t *row = test_make_card(parent, 0x1a1f2a, 0x31394a);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Compact slider row: smaller font + slimmer track so the whole 5-row
     * panel fits inside a 360 px tall column without scrolling. Was
     * eva_font_uk_22 (~28 px tall) → eva_font_uk_14 (~16 px); slider track
     * 16 → 10 px. */
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &eva_font_uk_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffe6ad), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_90, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, title);
    if (label_out) {
        *label_out = label;
    }

    lv_obj_t *sl = lv_slider_create(row);
    lv_obj_set_width(sl, LV_PCT(100));
    lv_obj_set_height(sl, 10);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, init, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0x2b3140), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sl, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_radius(sl, 999, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xffe6ad), LV_PART_INDICATOR);
    lv_obj_set_style_radius(sl, 999, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, lv_color_hex(0xfff5cf), LV_PART_KNOB);
    lv_obj_set_style_border_width(sl, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sl, test_numeric_slider_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)field);
    if (slider_out) {
        *slider_out = sl;
    }
    return row;
}

static lv_obj_t *test_dropdown_row_create(lv_obj_t *parent, const char *title,
                                          const char *options,
                                          test_dropdown_field_t field,
                                          lv_obj_t **dropdown_out)
{
    lv_obj_t *row = test_make_card(parent, 0x1a1f2a, 0x31394a);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    /* Compact dropdown row — same uk_14 font as the slider rows so heights
     * align across the panel (was uk_22 → ~28 px row, now uk_14 → ~18 px). */
    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_font(label, &eva_font_uk_14, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffe6ad), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_90, 0);
    lv_label_set_text(label, title);

    lv_obj_t *dd = lv_dropdown_create(row);
    lv_obj_set_width(dd, LV_PCT(100));
    lv_obj_set_height(dd, 28);
    lv_dropdown_set_options(dd, options);
    /* Suppress the LVGL chevron — it's a FontAwesome glyph (LV_SYMBOL_DOWN
     * = U+F078) that's not in eva_font_uk_*, so it renders as a missing-
     * glyph box. Empty symbol = no glyph drawn. */
    lv_dropdown_set_symbol(dd, NULL);
    lv_obj_set_style_text_font(dd, &eva_font_uk_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(dd, lv_color_hex(0xf4e8c8), LV_PART_MAIN);
    lv_obj_set_style_bg_color(dd, lv_color_hex(0x242b38), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dd, LV_OPA_90, LV_PART_MAIN);
    lv_obj_set_style_pad_all(dd, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(dd, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(dd, lv_color_hex(0x44516a), LV_PART_MAIN);
    lv_obj_set_style_radius(dd, 10, LV_PART_MAIN);
    lv_obj_set_style_text_font(dd, &eva_font_uk_14, LV_PART_INDICATOR);
    lv_obj_add_event_cb(dd, test_dropdown_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)field);
    if (dropdown_out) {
        *dropdown_out = dd;
    }
    return row;
}

static lv_obj_t *test_switch_row_create(lv_obj_t *parent, const char *title,
                                        test_switch_field_t field,
                                        lv_obj_t **switch_out,
                                        lv_obj_t **label_out)
{
    lv_obj_t *row = test_make_card(parent, 0x1a1f2a, 0x31394a);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(row);
    lv_obj_set_width(label, LV_PCT(74));
    lv_obj_set_style_text_font(label, &eva_font_uk_22, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xffe6ad), 0);
    lv_obj_set_style_text_opa(label, LV_OPA_90, 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_label_set_text(label, title);
    if (label_out) {
        *label_out = label;
    }

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_add_event_cb(sw, test_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)field);
    if (switch_out) {
        *switch_out = sw;
    }
    return row;
}

static void test_refresh_control_labels(void)
{
    if (s_test_slider_high_label) {
        lv_label_set_text_fmt(s_test_slider_high_label, "Верхні хмари %u%%", (unsigned)s_test_state.cloud_high_pct);
    }
    if (s_test_slider_mid_label) {
        lv_label_set_text_fmt(s_test_slider_mid_label, "Середні хмари %u%%", (unsigned)s_test_state.cloud_mid_pct);
    }
    if (s_test_slider_low_label) {
        lv_label_set_text_fmt(s_test_slider_low_label, "Нижні хмари %u%%", (unsigned)s_test_state.cloud_low_pct);
    }
    if (s_test_slider_wind_label) {
        lv_label_set_text_fmt(s_test_slider_wind_label, "Вітер %d км/год", (int)s_test_state.wind_kph);
    }
    if (s_test_slider_hour_label) {
        lv_label_set_text_fmt(s_test_slider_hour_label, "Час %+dh", s_test_hour_offset);
    }
    if (s_test_temp_label) {
        /* No degree symbol — U+00B0 is missing from eva_font_uk_22. Use a
         * trailing C instead, matches how temperature is shown elsewhere
         * (e.g. the big canvas label "+22C" without °). */
        lv_label_set_text_fmt(s_test_temp_label, "Температура %+dC", (int)s_test_state.temp_c);
    }
    if (s_test_feels_label) {
        lv_label_set_text_fmt(s_test_feels_label, "Відчувається %+dC", (int)s_test_state.feels_like_c);
    }
    if (s_test_cloud_cover_label) {
        lv_label_set_text_fmt(s_test_cloud_cover_label, "Хмарність неба %u%%", (unsigned)s_test_state.cloud_cover_pct);
    }
    if (s_test_cloud_total_label) {
        lv_label_set_text_fmt(s_test_cloud_total_label, "Загальні хмари %u%%", (unsigned)s_test_state.cloud_total_pct);
    }
    if (s_test_fog_label) {
        lv_label_set_text_fmt(s_test_fog_label, "Туман %u%%", (unsigned)s_test_state.fog_pct);
    }
    if (s_test_wind_dir_label) {
        if (s_test_state.wind_dir_deg < 0) {
            lv_label_set_text(s_test_wind_dir_label, "Напрям авто");
        } else {
            lv_label_set_text_fmt(s_test_wind_dir_label, "Напрям %d°", (int)s_test_state.wind_dir_deg);
        }
    }
    if (s_test_precip_mm_label) {
        lv_label_set_text_fmt(s_test_precip_mm_label, "Опади %.1f мм",
                              (double)s_test_state.precip_mm_x10 / 10.0);
    }
    if (s_test_sunrise_label) {
        char buf[16];
        minutes_to_text(s_test_state.sunrise_min, buf, sizeof(buf));
        lv_label_set_text_fmt(s_test_sunrise_label, "Схід сонця %s", s_test_state.sunrise_min < 0 ? "авто" : buf);
    }
    if (s_test_sunset_label) {
        char buf[16];
        minutes_to_text(s_test_state.sunset_min, buf, sizeof(buf));
        lv_label_set_text_fmt(s_test_sunset_label, "Захід сонця %s", s_test_state.sunset_min < 0 ? "авто" : buf);
    }
    if (s_test_moonrise_label) {
        char buf[16];
        minutes_to_text(s_test_state.moonrise_min, buf, sizeof(buf));
        lv_label_set_text_fmt(s_test_moonrise_label, "Схід Місяця %s", s_test_state.moonrise_min < 0 ? "авто" : buf);
    }
    if (s_test_moonset_label) {
        char buf[16];
        minutes_to_text(s_test_state.moonset_min, buf, sizeof(buf));
        lv_label_set_text_fmt(s_test_moonset_label, "Захід Місяця %s", s_test_state.moonset_min < 0 ? "авто" : buf);
    }
    if (s_test_moon_phase_label) {
        lv_label_set_text_fmt(s_test_moon_phase_label, "Фаза Місяця %u%% %s",
                              (unsigned)s_test_state.moon_phase_pct,
                              s_test_state.moon_waning ? "спадає" : "зростає");
    }
    if (s_test_moon_waning_label) {
        lv_label_set_text(s_test_moon_waning_label,
                          s_test_state.moon_waning ? "Місяць спадає" : "Місяць зростає");
    }
}

static void test_sync_controls_from_state(void)
{
    if (!s_test_slider_panel) return;
    s_test_ui_syncing = true;

    if (s_test_preset_dropdown) {
        uint32_t preset_sel = 0;
        if (s_test_weather_idx == TEST_WEATHER_PRESET_CUSTOM) {
            preset_sel = 1;
        } else if (s_test_weather_idx >= 0 && s_test_weather_idx < TEST_WEATHER_CYCLE_LEN) {
            preset_sel = (uint32_t)(s_test_weather_idx + 2);
        }
        lv_dropdown_set_selected(s_test_preset_dropdown, preset_sel);
    }
    if (s_test_kind_dropdown) {
        lv_dropdown_set_selected(s_test_kind_dropdown,
                                 s_test_kind_auto ? 0 : kind_dropdown_index_from_kind(s_test_manual_kind));
    }
    if (s_test_desc_dropdown) {
        lv_dropdown_set_selected(s_test_desc_dropdown, (uint32_t)s_test_desc_mode);
    }
    if (s_test_precip_dropdown) {
        lv_dropdown_set_selected(s_test_precip_dropdown, (uint32_t)s_test_state.precip_type);
    }
    if (s_test_temp_slider) {
        lv_slider_set_value(s_test_temp_slider, s_test_state.temp_c, LV_ANIM_OFF);
    }
    if (s_test_feels_slider) {
        lv_slider_set_value(s_test_feels_slider, s_test_state.feels_like_c, LV_ANIM_OFF);
    }
    if (s_test_cloud_cover_slider) {
        lv_slider_set_value(s_test_cloud_cover_slider, s_test_state.cloud_cover_pct, LV_ANIM_OFF);
    }
    if (s_test_cloud_total_slider) {
        lv_slider_set_value(s_test_cloud_total_slider, s_test_state.cloud_total_pct, LV_ANIM_OFF);
    }
    if (s_test_fog_slider) {
        lv_slider_set_value(s_test_fog_slider, s_test_state.fog_pct, LV_ANIM_OFF);
    }
    if (s_test_wind_dir_slider) {
        lv_slider_set_value(s_test_wind_dir_slider, s_test_state.wind_dir_deg, LV_ANIM_OFF);
    }
    if (s_test_precip_slider) {
        lv_slider_set_value(s_test_precip_slider, s_test_state.precip_mm_x10, LV_ANIM_OFF);
    }
    if (s_test_sunrise_slider) {
        lv_slider_set_value(s_test_sunrise_slider, s_test_state.sunrise_min, LV_ANIM_OFF);
    }
    if (s_test_sunset_slider) {
        lv_slider_set_value(s_test_sunset_slider, s_test_state.sunset_min, LV_ANIM_OFF);
    }
    if (s_test_moonrise_slider) {
        lv_slider_set_value(s_test_moonrise_slider, s_test_state.moonrise_min, LV_ANIM_OFF);
    }
    if (s_test_moonset_slider) {
        lv_slider_set_value(s_test_moonset_slider, s_test_state.moonset_min, LV_ANIM_OFF);
    }
    if (s_test_moon_phase_slider) {
        lv_slider_set_value(s_test_moon_phase_slider, s_test_state.moon_phase_pct, LV_ANIM_OFF);
    }
    if (s_test_moon_waning_switch) {
        if (s_test_state.moon_waning) {
            lv_obj_add_state(s_test_moon_waning_switch, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_test_moon_waning_switch, LV_STATE_CHECKED);
        }
    }
    if (s_test_slider_wind) {
        lv_slider_set_value(s_test_slider_wind, s_test_state.wind_kph, LV_ANIM_OFF);
    }
    if (s_test_slider_hour) {
        lv_slider_set_value(s_test_slider_hour, s_test_hour_offset, LV_ANIM_OFF);
    }
    if (s_test_slider_high) {
        lv_slider_set_value(s_test_slider_high, s_test_state.cloud_high_pct, LV_ANIM_OFF);
    }
    if (s_test_slider_mid) {
        lv_slider_set_value(s_test_slider_mid, s_test_state.cloud_mid_pct, LV_ANIM_OFF);
    }
    if (s_test_slider_low) {
        lv_slider_set_value(s_test_slider_low, s_test_state.cloud_low_pct, LV_ANIM_OFF);
    }

    s_test_ui_syncing = false;
    test_refresh_control_labels();
}

/* Build the control deck once on first activation. It behaves like a
 * scrollable touch-first dashboard: quick presets, scene selectors, a
 * compact core weather block, and a hidden advanced block for rarer fields. */
static void test_slider_panel_create(lv_obj_t *parent)
{
    if (s_test_slider_panel) return;

    /* Compact panel: anchored top-left, narrower (200 px) so it doesn't
     * cover the moon/sun area. Only the controls that produce visually
     * obvious changes on screen are exposed; everything else (feels-like,
     * precip mm, dew point, moon phase, sunrise/sunset min, etc.) lives
     * behind CDC commands for power users. */
    s_test_slider_panel = lv_obj_create(parent);
    lv_obj_remove_style_all(s_test_slider_panel);
    /* Sized so 1 dropdown + 5 compact slider rows fit without scrolling.
     * 200 wide × 456 tall ≈ 42 % of the 800×480 display, anchored to the
     * top-left corner with 8 px margin all around. */
    lv_obj_set_size(s_test_slider_panel, 200, 456);
    lv_obj_align(s_test_slider_panel, LV_ALIGN_TOP_LEFT, 8, 12);
    style_floating_card(s_test_slider_panel, 0x10151f, 0xffe6ad);
    lv_obj_set_style_bg_opa(s_test_slider_panel, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_test_slider_panel, 10, 0);
    lv_obj_set_style_pad_row(s_test_slider_panel, 8, 0);
    lv_obj_set_style_pad_column(s_test_slider_panel, 0, 0);
    lv_obj_add_flag(s_test_slider_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_test_slider_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_test_slider_panel, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_flex_flow(s_test_slider_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_test_slider_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(s_test_slider_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_test_slider_panel, LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *title = lv_label_create(s_test_slider_panel);
    lv_obj_set_style_text_font(title, &eva_font_uk_22, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffe6ad), 0);
    lv_obj_set_style_text_opa(title, LV_OPA_90, 0);
    lv_label_set_text(title, "ТЕСТ");

    /* One unified preset selector: shows weather kind as the primary picker
     * (was split into "Сцена" + "Kind" + "Опис" — three dropdowns competing
     * for the same job). */
    (void)test_dropdown_row_create(s_test_slider_panel, "Погода",
                                   s_kind_options, TEST_DD_KIND,
                                   &s_test_kind_dropdown);

    /* The five sliders that visibly drive the scene. */
    (void)test_slider_row_create(s_test_slider_panel, "Температура", -50, 50, 0,
                                 TEST_NUM_TEMP, &s_test_temp_slider, &s_test_temp_label);
    (void)test_slider_row_create(s_test_slider_panel, "Вітер", 0, 120, 0,
                                 TEST_NUM_WIND_SPEED, &s_test_slider_wind, &s_test_slider_wind_label);
    (void)test_slider_row_create(s_test_slider_panel, "Хмари", 0, 100, 0,
                                 TEST_NUM_COVER, &s_test_cloud_cover_slider, &s_test_cloud_cover_label);
    (void)test_slider_row_create(s_test_slider_panel, "Опади", 0, 2000, 0,
                                 TEST_NUM_PRECIP_MM, &s_test_precip_slider, &s_test_precip_mm_label);
    (void)test_slider_row_create(s_test_slider_panel, "Час", -23, 23, 0,
                                 TEST_NUM_HOUR_OFFSET, &s_test_slider_hour, &s_test_slider_hour_label);

    /* Hidden but referenced by callbacks — leave NULL pointers, the test code
     * already null-checks each slider pointer before reading/writing it. */
    s_test_advanced_panel = NULL;
    s_test_advanced_toggle = NULL;
    s_test_advanced_toggle_label = NULL;
    s_test_preset_dropdown = NULL;
    s_test_desc_dropdown = NULL;
    s_test_feels_slider = s_test_feels_label = NULL;
    s_test_cloud_total_slider = s_test_cloud_total_label = NULL;
    s_test_fog_slider = s_test_fog_label = NULL;
    s_test_wind_dir_slider = s_test_wind_dir_label = NULL;
    s_test_slider_high = s_test_slider_high_label = NULL;
    s_test_slider_mid  = s_test_slider_mid_label  = NULL;
    s_test_slider_low  = s_test_slider_low_label  = NULL;
    s_test_precip_dropdown = NULL;
    s_test_sunrise_slider = s_test_sunrise_label = NULL;
    s_test_sunset_slider  = s_test_sunset_label  = NULL;
    s_test_moonrise_slider = s_test_moonrise_label = NULL;
    s_test_moonset_slider  = s_test_moonset_label  = NULL;
    s_test_moon_phase_slider = s_test_moon_phase_label = NULL;
    s_test_moon_waning_switch = s_test_moon_waning_label = NULL;

    test_refresh_control_labels();
}

static void test_slider_panel_show(bool show)
{
    if (!s_test_slider_panel) return;
    if (show) {
        lv_obj_remove_flag(s_test_slider_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_test_slider_panel);
        lv_obj_scroll_to_y(s_test_slider_panel, 0, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(s_test_slider_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void style_floating_card(lv_obj_t *obj, uint32_t bg_hex, uint32_t border_hex)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
    lv_obj_set_style_radius(obj, 18, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_hex), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(obj, 16, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_20, 0);
}

static void set_test_mode(bool on)
{
    if (s_test_mode == on) return;
    s_test_mode = on;
    if (on) {
        s_test_weather_idx = TEST_WEATHER_PRESET_LIVE;
        test_seed_from_live();
        s_test_kind_auto = true;
        s_test_desc_mode = TEST_DESC_AUTO;
        test_advanced_panel_set_visible(false);
        test_sync_controls_from_state();
        test_apply_scene();
        test_slider_panel_show(true);
        /* Reveal the FPS overlay alongside the test panel — debug-only info,
         * hidden during normal use. */
        eva_fps_overlay_show(s_fps_overlay, true);
        /* IP/Wi-Fi readout also belongs in test mode only — it's clutter
         * during a normal weather glance. */
        eva_wifi_status_show(s_wifi_status, true);
    } else {
        s_test_hour_offset = 0;
        s_test_kind_auto = true;
        s_test_desc_mode = TEST_DESC_AUTO;
        s_test_weather_idx = TEST_WEATHER_PRESET_LIVE;
        test_advanced_panel_set_visible(false);
        /* Release canvas overrides and reset slider visuals. */
        eva_weather_canvas_clear_test_overrides();
        eva_weather_canvas_set_time_offset(0);
        test_slider_panel_show(false);
        eva_fps_overlay_show(s_fps_overlay, false);
        eva_wifi_status_show(s_wifi_status, false);
        /* Re-pull live weather so the canvas leaves test state. */
        weather_state_t st = { 0 };
        if (!eva_weather_copy(&st)) {
            st = *eva_weather_get();
        }
        eva_weather_canvas_set_weather(&st);
        eva_weather_canvas_show(true);
        update_weather_labels(&st);
        test_refresh_control_labels();
    }
    update_test_banner();
    eva_clock_set_hour_offset(s_clock, s_test_mode ? s_test_hour_offset : 0);
    eva_weather_canvas_set_time_offset(s_test_mode ? s_test_hour_offset : 0);
}

static void update_test_banner(void)
{
    if (!s_test_banner) return;
    if (!s_test_mode) {
        lv_obj_add_flag(s_test_banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    /* Compact 1-line readout: kind + temp + wind + cloud cover. The previous
     * 4-line tech-dump (sunrise/sunset/moonrise/moonset/precip mm/auto/manual
     * etc.) belonged in a debug log, not as floating UI. Everything that was
     * in the banner is still reachable via `weather` and `status` CDC commands. */
    lv_obj_set_width(s_test_banner, 240);
    lv_label_set_long_mode(s_test_banner, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_test_banner, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_line_space(s_test_banner, 2, 0);
    /* Use ASCII separators ("|") instead of U+00B7 — eva_font_uk_14 covers
     * only Latin-1 up to U+00B0 (degree), so a centre-dot renders as a
     * "missing glyph" box. The degree symbol itself IS in the font, so
     * "%+d°C" works fine. */
    lv_label_set_text_fmt(s_test_banner,
                          "%s | %+d°C | %d kph | %u%%",
                          weather_kind_label_uk(s_test_state.kind),
                          (int)s_test_state.temp_c,
                          (int)s_test_state.wind_kph,
                          (unsigned)s_test_state.cloud_cover_pct);
    lv_obj_remove_flag(s_test_banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_test_banner);
}

static void test_checkbox_event_cb(lv_event_t *e)
{
    lv_obj_t *cb = lv_event_get_target(e);
    bool checked = lv_obj_has_state(cb, LV_STATE_CHECKED);
    set_test_mode(checked);
}

static void screen_gesture_event_cb(lv_event_t *e)
{
    (void)e;
    if (!s_test_mode) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    switch (dir) {
        case LV_DIR_LEFT:
            s_test_weather_idx = (s_test_weather_idx + 1) % TEST_WEATHER_CYCLE_LEN;
            apply_test_weather();
            break;
        case LV_DIR_RIGHT:
            s_test_weather_idx = (s_test_weather_idx - 1 + TEST_WEATHER_CYCLE_LEN) % TEST_WEATHER_CYCLE_LEN;
            apply_test_weather();
            break;
        /* Vertical swipes replaced by HOUR slider in the test panel. */
        default:
            return;
    }
    update_test_banner();
    if (s_clock) eva_clock_tick(s_clock);
}

static uint32_t weatherdebug_mix32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

static int weatherdebug_rand_range(uint32_t *seed, int lo, int hi)
{
    if (hi <= lo) return lo;
    *seed = weatherdebug_mix32(*seed);
    return lo + (int)(*seed % (uint32_t)(hi - lo + 1));
}

static void debug_weather_in_lvgl(void *user)
{
    /* Previously this returned early when test mode was on, on the theory
     * that the user's slider state shouldn't be clobbered. But weatherdebug
     * is an explicit CDC command — if the host asks for it, we should
     * honour it. The slider/kind dropdown can then re-sync from the new
     * state via test_seed_from_live() if needed. */
    uintptr_t packed = (uintptr_t)user;
    weather_kind_t kind = (weather_kind_t)(packed & 0xff);
    int frame = (int)((packed >> 8) & 0xff);
    uint32_t seed = weatherdebug_mix32(0x9e3779b9u ^ ((uint32_t)kind << 24) ^ (uint32_t)frame);

    weather_state_t st = { 0 };
    if (!eva_weather_copy(&st)) {
        st = *eva_weather_get();
    }
    st.kind = kind;

    /* Without this, weatherdebug switches kind but keeps the live cloud
     * cover (which is whatever the last open-meteo fetch said — often 0%
     * for a clear day). We also use `frame` as a seed so repeated captures
     * can deliberately vary cloud layouts without touching the live weather
     * state. */
    switch (kind) {
    case WEATHER_CLEAR_DAY:
    case WEATHER_CLEAR_NIGHT:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 0, 10);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 0, 14);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 0, 18);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 0, 12);
        st.cloud_cover_pct = st.cloud_total_pct;
        break;
    case WEATHER_PARTLY_CLOUDY_DAY:
    case WEATHER_PARTLY_CLOUDY_NIGHT:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 45, 72);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 24, 48);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 0, 14);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 44, 68);
        st.cloud_cover_pct = st.cloud_total_pct;
        break;
    case WEATHER_CLOUDY:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 68, 96);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 72, 100);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 48, 86);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 74, 100);
        st.cloud_cover_pct = st.cloud_total_pct;
        break;
    case WEATHER_FOG:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 58, 92);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 54, 88);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 36, 72);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 60, 100);
        st.cloud_cover_pct = st.cloud_total_pct;
        st.fog_pct = (uint8_t)weatherdebug_rand_range(&seed, 68, 100);
        break;
    case WEATHER_RAIN:
    case WEATHER_HEAVY_RAIN:
    case WEATHER_THUNDERSTORM:
    case WEATHER_SLEET:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 72, 100);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 64, 98);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 28, 82);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 82, 100);
        st.cloud_cover_pct = st.cloud_total_pct;
        st.fog_pct = (uint8_t)weatherdebug_rand_range(&seed, 0, 22);
        st.wind_kph = (int16_t)clamp_i((int)st.wind_kph + weatherdebug_rand_range(&seed, -6, 10), 0, 120);
        break;
    case WEATHER_SNOW:
    case WEATHER_HAIL:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 68, 96);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 60, 94);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 26, 76);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 78, 100);
        st.cloud_cover_pct = st.cloud_total_pct;
        st.wind_kph = (int16_t)clamp_i((int)st.wind_kph + weatherdebug_rand_range(&seed, -4, 8), 0, 120);
        break;
    default:
        st.cloud_low_pct = (uint8_t)weatherdebug_rand_range(&seed, 28, 72);
        st.cloud_mid_pct = (uint8_t)weatherdebug_rand_range(&seed, 28, 72);
        st.cloud_high_pct = (uint8_t)weatherdebug_rand_range(&seed, 20, 66);
        st.cloud_total_pct = (uint8_t)weatherdebug_rand_range(&seed, 30, 80);
        st.cloud_cover_pct = st.cloud_total_pct;
        break;
    }
    if (st.wind_dir_deg >= 0) {
        st.wind_dir_deg = (int16_t)((st.wind_dir_deg + weatherdebug_rand_range(&seed, -45, 45) + 360) % 360);
    }
    if (kind == WEATHER_CLEAR_DAY || kind == WEATHER_CLEAR_NIGHT) {
        st.wind_kph = (int16_t)clamp_i((int)st.wind_kph + weatherdebug_rand_range(&seed, -3, 6), 0, 120);
    }
    /* For night kinds, force sunrise/sunset so the current wall-clock time
     * lands well outside any sunrise/sunset window — otherwise the sunset
     * gradient (rose/lavender) will leak into a clear-night demo. Tuning the
     * times to a wide AM band makes any debug call show a real night sky. */
    if (kind == WEATHER_CLEAR_NIGHT || kind == WEATHER_PARTLY_CLOUDY_NIGHT) {
        st.sunrise_min = 360;   /* 06:00 next-day */
        st.sunset_min  = 1080;  /* 18:00 previous-day; both far from "now" */
        /* Also fudge "now" via skipping the daytime sun draw — handled in
         * sky_for_kind via is_night_kind, which already prioritises the
         * kind tag. Nothing else to do here. */
    }
    snprintf(st.desc, sizeof(st.desc), "%s наживо", weather_kind_label_uk(kind));
    st.fetched_at = time(NULL);
    eva_weather_set(&st);
    eva_weather_canvas_set_kind(kind);
    eva_weather_canvas_show(true);
    update_weather_labels(&st);

    /* When test mode is on, keep the slider panel + banner in sync with the
     * weather we just applied. We deliberately do NOT call test_apply_scene()
     * here — that would re-derive desc/kind from raw data via
     * derive_kind_from_raw() + desc_from_mode(), which can silently change
     * the result (e.g. flip clear-day → clear-night at 23:00 because the
     * wall-clock says night). The whole point of weatherdebug is to BYPASS
     * derivation: the host asked for kind=X, we honour it verbatim. */
    if (s_test_mode) {
        s_test_state = st;
        s_test_kind_auto = false;
        s_test_manual_kind = kind;
        /* Sync visible controls (dropdown selection, slider positions, etc.)
         * to reflect the new state without re-applying anything. */
        test_sync_controls_from_state();
        update_test_banner();
    }
}

static void wifi_status_cb(const char *msg)
{
    eva_wifi_status_set_text(s_wifi_status, msg);
}

static void cdc_send_weather_status(void)
{
    time_t now = time(NULL);
    char time_buf[40];
    weather_state_t st = { 0 };
    if (!eva_weather_copy(&st)) {
        st = *eva_weather_get();
    }
    if (now > 1700000000) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
    } else {
        snprintf(time_buf, sizeof(time_buf), "not synced");
    }

    char sun_buf[40];
    if (st.sunrise_min >= 0 && st.sunset_min >= 0) {
        snprintf(sun_buf, sizeof(sun_buf), "%02d:%02d / %02d:%02d",
                 st.sunrise_min / 60, st.sunrise_min % 60,
                 st.sunset_min / 60, st.sunset_min % 60);
    } else {
        snprintf(sun_buf, sizeof(sun_buf), "unknown");
    }

    char clock_solar_buf[56];
    if (now > 1700000000) {
        struct tm tm_now;
        localtime_r(&now, &tm_now);
        clock_solar_t sun;
        clock_solar_state(minutes_from_clock_now(&tm_now), &st, &sun);
        snprintf(clock_solar_buf, sizeof(clock_solar_buf),
                 "apex=%02d:%02d elevation=%u%%",
                 sun.apex_min / 60, sun.apex_min % 60,
                 (unsigned)(sun.elevation * 100.0f + 0.5f));
    } else {
        snprintf(clock_solar_buf, sizeof(clock_solar_buf), "not synced");
    }

    char wind_buf[24];
    if (st.wind_kph >= 0) {
        if (st.wind_dir_deg >= 0) {
            snprintf(wind_buf, sizeof(wind_buf), "%d km/h @ %d deg",
                     (int)st.wind_kph, (int)st.wind_dir_deg);
        } else {
            snprintf(wind_buf, sizeof(wind_buf), "%d km/h", (int)st.wind_kph);
        }
    } else {
        snprintf(wind_buf, sizeof(wind_buf), "unknown");
    }

    /* Per-source provenance for the sources block */
    int64_t om_ts = weather_fetch_openmeteo_last_ts();
    int64_t co_ts = weather_fetch_clearoutside_last_ts();
    char om_str[32], co_str[32];
    if (om_ts > 0) {
        time_t t = (time_t)om_ts;
        struct tm tm; localtime_r(&t, &tm);
        strftime(om_str, sizeof(om_str), "%Y-%m-%d %H:%M", &tm);
    } else {
        snprintf(om_str, sizeof(om_str), "never");
    }
    if (co_ts > 0) {
        time_t t = (time_t)co_ts;
        struct tm tm; localtime_r(&t, &tm);
        strftime(co_str, sizeof(co_str), "%Y-%m-%d %H:%M", &tm);
    } else {
        snprintf(co_str, sizeof(co_str), "never");
    }

    cdc_sendf(
        "status:\r\n"
        "  firmware: weather\r\n"
        "  usb_cdc: %s\r\n"
        "  clock: %s\r\n"
        "  weather: %s %+dC feels %+dC \"%s\"\r\n"
        "  wind: %s\r\n"
        "  clouds: cover=%u%% [low=%u%% mid=%u%% high=%u%%] fog=%u%% visibility=%u.%u km\r\n"
        "  precip: %s %u.%u mm\r\n"
        "  weather_code: %d (WMO)\r\n"
        "  sunshine: %u min/day\r\n"
        "  sun: %s\r\n"
        "  clock_solar: %s\r\n"
        "  tz: %s\r\n"
        "  log_level: %s\r\n"
        "  uptime_s: %llu\r\n"
        "  sources:\r\n"
        "    openmeteo:    %s%s\r\n"
        "    clearoutside: %s%s\r\n",
        eva_cdc_host_open(s_cdc) ? "connected" : "disconnected",
        time_buf,
        weather_kind_name(st.kind),
        (int)st.temp_c,
        (int)st.feels_like_c,
        st.desc,
        wind_buf,
        (unsigned)st.cloud_cover_pct,
        (unsigned)st.cloud_low_pct,
        (unsigned)st.cloud_mid_pct,
        (unsigned)st.cloud_high_pct,
        (unsigned)st.fog_pct,
        (unsigned)(st.visibility_km_x10 / 10),
        (unsigned)(st.visibility_km_x10 % 10),
        precip_type_name(st.precip_type),
        (unsigned)(st.precip_mm_x10 / 10),
        (unsigned)(st.precip_mm_x10 % 10),
        st.weather_code,
        (unsigned)st.sunshine_minutes,
        sun_buf,
        clock_solar_buf,
        eva_settings_get_tz(s_settings),
        log_level_to_str(esp_log_level_get("*")),
        (unsigned long long)(esp_timer_get_time() / 1000000ULL),
        om_str, weather_fetch_openmeteo_retrying()    ? " (retrying)" : "",
        co_str, weather_fetch_clearoutside_retrying() ? " (retrying)" : "");
}

static void cdc_send_perf_status(void)
{
    uint32_t hz = eva_weather_canvas_last_tick_hz();
    uint32_t work = eva_weather_canvas_last_work_us();
    uint32_t bg = 0, cl = 0, pa = 0, li = 0, lvgl = 0, vsync = 0;
    uint16_t c_active = 0, c_max = 0;
    eva_weather_canvas_last_breakdown_us(&bg, &cl, &pa, &li, &lvgl, &vsync);
    eva_weather_canvas_cloud_budget(&c_active, &c_max);

    cdc_sendf(
        "perf:\r\n"
        "  tick: %lu Hz\r\n"
        "  work: %lu us\r\n"
        "  breakdown_us: bg=%lu cl=%lu pa=%lu li=%lu lvgl=%lu vsync=%lu\r\n"
        "  clouds_3d: %u/%u\r\n",
        (unsigned long)hz,
        (unsigned long)work,
        (unsigned long)bg,
        (unsigned long)cl,
        (unsigned long)pa,
        (unsigned long)li,
        (unsigned long)lvgl,
        (unsigned long)vsync,
        (unsigned)c_active,
        (unsigned)c_max);
}

static void cdc_handle_command(void *user, char *line)
{
    (void)user;
    char *cmd = trim_in_place(line);
    if (!cmd[0]) return;

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        cdc_send(
            "commands:\r\n"
            "  whoami\r\n"
            "  time\r\n"
            "  tz\r\n"
            "  tz <posix-tz-string>\r\n"
            "  clockoffset <hours>\r\n"
            "  weather\r\n"
            "  weather <" WEATHER_KIND_HELP "> <temp_c> \"Description\"\r\n"
            "  weather refresh\r\n"
            "  weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>\r\n"
            "  weatherdebug <" WEATHER_KIND_HELP "> <frame>\r\n"
            "  status\r\n"
            "  perf\r\n"
            "  screenshot\r\n"
            "  log\r\n"
            "  log <none|error|warn|info|debug|verbose>\r\n");
        return;
    }

    if (strcmp(cmd, "whoami") == 0) {
        cdc_send("weather\r\n");
        return;
    }

    if (strcmp(cmd, "time") == 0) {
        time_t now = time(NULL);
        if (now > 1700000000) {
            char time_buf[40];
            struct tm tm_now;
            localtime_r(&now, &tm_now);
            strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
            cdc_sendf("%s\r\n", time_buf);
        } else {
            cdc_send("time: not synced yet\r\n");
        }
        return;
    }

    if (strcmp(cmd, "tz") == 0) {
        cdc_sendf("tz: %s\r\n", eva_settings_get_tz(s_settings));
        return;
    }

    if (strncmp(cmd, "tz", 2) == 0 && isspace((unsigned char)cmd[2])) {
        char *value = trim_in_place(cmd + 2);
        if (!value[0]) {
            cdc_send("ERR timezone is empty\r\n");
            return;
        }
        if (strlen(value) >= eva_settings_tz_capacity(s_settings)) {
            cdc_send("ERR timezone too long\r\n");
            return;
        }
        eva_settings_set_tz(s_settings, value);
        eva_settings_save(s_settings);
        apply_timezone();
        cdc_sendf("OK timezone saved: %s\r\n", eva_settings_get_tz(s_settings));
        return;
    }

    if (strncmp(cmd, "clockoffset", 11) == 0 && isspace((unsigned char)cmd[11])) {
        char *value = trim_in_place(cmd + 11);
        int hours = clamp_i(atoi(value), -23, 23);
        s_test_hour_offset = hours;
        if (s_clock) eva_clock_set_hour_offset(s_clock, hours);
        eva_weather_canvas_set_time_offset(hours);
        if (s_test_mode) {
            test_apply_scene();
        }
        cdc_sendf("OK clockoffset %+d\r\n", hours);
        return;
    }

    if (strcmp(cmd, "status") == 0) {
        cdc_send_weather_status();
        return;
    }

    if (strcmp(cmd, "perf") == 0) {
        cdc_send_perf_status();
        return;
    }

    if (strcmp(cmd, "scene") == 0) {
        cdc_send("scene: weather\r\n");
        return;
    }

    if (strcmp(cmd, "weather") == 0) {
        weather_state_t st = { 0 };
        if (!eva_weather_copy(&st)) {
            st = *eva_weather_get();
        }
        cdc_sendf("weather: %s %+dC \"%s\"\r\n",
                  weather_kind_name(st.kind), (int)st.temp_c, st.desc);
        return;
    }

    if (strcmp(cmd, "weather refresh") == 0) {
        weather_fetch_request();
        cdc_send("OK weather refresh queued\r\n");
        return;
    }

    if (strncmp(cmd, "weatherraw", 10) == 0 && isspace((unsigned char)cmd[10])) {
        char *value = trim_in_place(cmd + 10);
        char *save = NULL;
        char *tok[7] = { 0 };
        for (int i = 0; i < 7; ++i) {
            tok[i] = strtok_r(i == 0 ? value : NULL, " \t", &save);
            if (!tok[i]) {
                cdc_send("ERR weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>\r\n");
                return;
            }
        }
        precip_type_t precip;
        if (!parse_precip_type(tok[5], &precip)) {
            cdc_send("ERR precip must be none|drizzle|light-rain|rain|heavy-rain|snow|sleet|hail|thunder\r\n");
            return;
        }
        weather_state_t st = { 0 };
        if (!eva_weather_copy(&st)) {
            st = *eva_weather_get();
        }
        st.cloud_low_pct = clamp_pct_i(atoi(tok[0]));
        st.cloud_mid_pct = clamp_pct_i(atoi(tok[1]));
        st.cloud_high_pct = clamp_pct_i(atoi(tok[2]));
        st.cloud_total_pct = clamp_pct_i(atoi(tok[3]));
        st.fog_pct = clamp_pct_i(atoi(tok[4]));
        st.precip_type = precip;
        int mm_x10 = atoi(tok[6]);
        if (mm_x10 < 0) mm_x10 = 0;
        if (mm_x10 > 65535) mm_x10 = 65535;
        st.precip_mm_x10 = (uint16_t)mm_x10;
        st.kind = WEATHER_UNKNOWN;
        st.desc[0] = '\0';
        st.fetched_at = time(NULL);
        eva_weather_set(&st);
        cdc_sendf("OK weatherraw clouds L/M/H/T=%u/%u/%u/%u fog=%u precip=%s %u.%umm\r\n",
                  (unsigned)st.cloud_low_pct, (unsigned)st.cloud_mid_pct,
                  (unsigned)st.cloud_high_pct, (unsigned)st.cloud_total_pct,
                  (unsigned)st.fog_pct, precip_type_name(st.precip_type),
                  (unsigned)(st.precip_mm_x10 / 10),
                  (unsigned)(st.precip_mm_x10 % 10));
        return;
    }

    if (strncmp(cmd, "weatherdebug", 12) == 0 && isspace((unsigned char)cmd[12])) {
        char *value = trim_in_place(cmd + 12);
        char *kind_s = value;
        char *frame_s = strchr(value, ' ');
        if (!frame_s) {
            cdc_send("ERR weatherdebug <" WEATHER_KIND_HELP "> <frame>\r\n");
            return;
        }
        *frame_s++ = '\0';
        frame_s = trim_in_place(frame_s);
        weather_kind_t kind;
        if (!weather_kind_parse(kind_s, &kind) || kind == WEATHER_UNKNOWN) {
            cdc_send("ERR weather kind must be " WEATHER_KIND_HELP "\r\n");
            return;
        }
        int frame = atoi(frame_s);
        uintptr_t packed = ((uintptr_t)(frame & 0xff) << 8) | (uintptr_t)(kind & 0xff);
        debug_weather_in_lvgl((void *)packed);
        cdc_sendf("OK weatherdebug %s frame %d\r\n", weather_kind_name(kind), frame);
        return;
    }

    if (strncmp(cmd, "weather", 7) == 0 && isspace((unsigned char)cmd[7])) {
        char *value = trim_in_place(cmd + 7);
        char *kind_s = value;
        char *temp_s = strchr(kind_s, ' ');
        if (!temp_s) {
            cdc_send("ERR weather <" WEATHER_KIND_HELP "> <temp_c> \"Description\"\r\n");
            return;
        }
        *temp_s++ = '\0';
        temp_s = trim_in_place(temp_s);
        char *desc_s = strchr(temp_s, ' ');
        if (!desc_s) {
            cdc_send("ERR weather needs a description\r\n");
            return;
        }
        *desc_s++ = '\0';
        desc_s = trim_in_place(desc_s);
        unquote_desc(desc_s);

        weather_kind_t kind;
        if (!weather_kind_parse(kind_s, &kind) || kind == WEATHER_UNKNOWN) {
            cdc_send("ERR weather kind must be " WEATHER_KIND_HELP "\r\n");
            return;
        }

        weather_state_t st = { 0 };
        if (!eva_weather_copy(&st)) {
            st = *eva_weather_get();
        }
        st.kind = kind;
        st.temp_c = (int8_t)atoi(temp_s);
        st.fetched_at = time(NULL);
        strlcpy(st.desc, desc_s, sizeof(st.desc));
        eva_weather_set(&st);
        cdc_sendf("OK weather %s %+dC \"%s\"\r\n",
                  weather_kind_name(st.kind), (int)st.temp_c, st.desc);
        return;
    }

    if (strcmp(cmd, "screenshot") == 0) {
        cdc_handle_screenshot();
        return;
    }

    /* Test mode is disabled in the native display path. The pipeline no
     * longer registers an LVGL display, so the sliders/banner widgets that
     * test mode used to create would crash on the first lv_obj_create.
     * Scene control for development still works through `weatherdebug
     * <kind> <frame>` (defined below) which pokes the canvas directly. */
    if (strcmp(cmd, "test") == 0 ||
        strcmp(cmd, "test on") == 0 ||
        strcmp(cmd, "test off") == 0) {
        cdc_send("ERR test mode disabled; use 'weatherdebug <kind> <frame>'\r\n");
        return;
    }

    if (strcmp(cmd, "log") == 0) {
        cdc_sendf("log_level: %s\r\n", log_level_to_str(esp_log_level_get("*")));
        return;
    }

    if (strncmp(cmd, "log", 3) == 0 && isspace((unsigned char)cmd[3])) {
        char *value = trim_in_place(cmd + 3);
        esp_log_level_t level;
        if (!parse_log_level(value, &level)) {
            cdc_send("ERR log level must be none|error|warn|info|debug|verbose\r\n");
            return;
        }
        esp_log_level_set("*", level);
        cdc_sendf("OK log level set to %s\r\n", log_level_to_str(level));
        return;
    }

    cdc_send("ERR unknown command; type help\r\n");
}

static void weather_screen_init_native(esp_lcd_panel_handle_t panel)
{
    eva_weather_canvas_init_native(panel);
    eva_weather_canvas_show(true);

    weather_state_t st = { 0 };
    if (!eva_weather_copy(&st)) {
        st = *eva_weather_get();
    }
    eva_weather_canvas_set_weather(&st);
    update_weather_labels(&st);
}

void app_main(void)
{
    s_cdc = eva_cdc_create(cdc_handle_command, NULL);
    ESP_LOGI(TAG, "Eva WEATHER v%s built %s %s", EVA_FIRMWARE_VERSION, __DATE__, __TIME__);

    esp_err_t ne = nvs_flash_init();
    if (ne == ESP_ERR_NVS_NO_FREE_PAGES || ne == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    s_settings = eva_settings_create();
    eva_settings_load(s_settings);
    apply_timezone();
    eva_weather_init();

    ESP_ERROR_CHECK(bsp_i2c_init());
    lv_init();
    bsp_lcd_handles_t lcd_handles = { 0 };
    ESP_ERROR_CHECK(bsp_display_new_with_handles(NULL, &lcd_handles));
    ESP_ERROR_CHECK(lcd_handles.panel ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_handles.panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());
    weather_screen_init_native(lcd_handles.panel);

    /* Clock runs as a FreeRTOS task (replaces the LVGL timer path that the
     * native pipeline disabled). Without this the canvas would keep showing
     * the placeholder "00:00" because no one calls eva_clock_tick. */
    s_clock = eva_clock_create();
    if (!s_clock) {
        ESP_LOGW(TAG, "clock create failed; canvas will show 00:00 until reboot");
    }

    eva_weather_set_update_cb(weather_update_cb, NULL);
    eva_screenshot_init();
    eva_wifi_set_status_cb(wifi_status_cb);
    eva_wifi_start();
    weather_fetch_start();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
