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

#include "esp_ota_ops.h"

#include "eva_cdc.h"
#include "eva_clock.h"
#include "eva_ota.h"
#include "eva_ota_state.h"
#include "eva_ota_status.h"
#include "eva_settings.h"
#include "eva_screenshot.h"
#include "eva_weather.h"
#include "eva_weather_canvas.h"
#include "eva_wifi.h"
#include "eva_wifi_status.h"
#include "weather_fetch.h"

#define WEATHER_KIND_HELP \
    "clear-day|clear-night|partly-cloudy-day|partly-cloudy-night|cloudy|fog|rain|heavy-rain|snow|thunderstorm|sleet|hail"

static const char *TAG = "eva";
static const char *EVA_FIRMWARE_VERSION = "1";

static eva_cdc_t *s_cdc;
static eva_settings_t *s_settings;
static eva_wifi_status_t *s_wifi_status;
static eva_ota_status_t *s_ota_status;
static eva_clock_t *s_clock;

/* CDC `clockoffset` hour shift applied to wall clock + canvas sky timing.
 * Cleared by `weatherlive`. */
static int s_hour_offset;

#define CLOCK_SOLAR_FALLBACK_SUNRISE_MIN  (6 * 60)
#define CLOCK_SOLAR_FALLBACK_SUNSET_MIN   (20 * 60)
#define CLOCK_SOLAR_PI 3.1415926f

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
    /* Feed sun times to the clock so the backlight ramp tracks the real day. */
    if (s_clock) {
        eva_clock_set_sun_times(s_clock, st->sunrise_min, st->sunset_min);
    }
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

static void weatherdebug_apply(void *user)
{
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
    /* Keep live sunrise/sunset from the cached fetch so sky twilight and
     * day/night palette follow real local sun times (not fixed 06:00/18:00). */
    if (st.sunrise_min < 0 || st.sunset_min < 0) {
        const weather_state_t *live = eva_weather_get();
        if (live) {
            if (st.sunrise_min < 0) st.sunrise_min = live->sunrise_min;
            if (st.sunset_min < 0)  st.sunset_min  = live->sunset_min;
        }
    }
    /* Plain label, no marker. This used to append " наживо" ("live"), which
     * was both wrong (weatherdebug is the opposite of live) and user-visible
     * on the panel. eva_weather.c keys its "discard debug snapshot from NVS"
     * check off that marker, so it now uses the transient flag instead. */
    snprintf(st.desc, sizeof(st.desc), "%s", weather_kind_label_uk(kind));
    st.fetched_at = time(NULL);
    eva_weather_set_transient(&st);
    eva_weather_canvas_set_weather(&st);
    eva_weather_canvas_show(true);
    update_weather_labels(&st);
}

static void restore_live_weather_and_time(void)
{
    weather_fetch_set_pinned(false);
    s_hour_offset = 0;
    if (s_clock) {
        eva_clock_set_hour_offset(s_clock, 0);
    }
    eva_weather_canvas_set_time_offset(0);
    eva_weather_canvas_clear_test_overrides();
    eva_weather_discard_saved();
    weather_fetch_reapply_cached();
    weather_fetch_request();
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
        "  backlight: %d%%\r\n"
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
        eva_clock_current_brightness(s_clock),
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
    uint32_t vst = eva_weather_canvas_last_vsync_timeouts();

    cdc_sendf(
        "perf:\r\n"
        "  tick: %lu Hz\r\n"
        "  work: %lu us\r\n"
        "  breakdown_us: bg=%lu cl=%lu pa=%lu li=%lu lvgl=%lu vsync=%lu\r\n"
        "  vsync_timeouts: %lu\r\n"
        "  clouds_3d: %u/%u\r\n",
        (unsigned long)hz,
        (unsigned long)work,
        (unsigned long)bg,
        (unsigned long)cl,
        (unsigned long)pa,
        (unsigned long)li,
        (unsigned long)lvgl,
        (unsigned long)vsync,
        (unsigned long)vst,
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
            "  weatherlive — real clock + live fetch (undoes debug/pin/offset)\r\n"
            "  weather\r\n"
            "  weather <" WEATHER_KIND_HELP "> <temp_c> \"Description\"\r\n"
            "  weather refresh\r\n"
            "  weatherraw <low> <mid> <high> <total> <fog> <precip> <mm_x10>\r\n"
            "  weatherdebug <" WEATHER_KIND_HELP "> <frame>\r\n"
            "  status\r\n"
            "  perf\r\n"
            "  cloudinfo — cloud asset pool status\r\n"
            "  lightning — force a strike (thunderstorm/hail only)\r\n"
            "  weatherpin on|off — freeze the scene against live fetch\r\n"
            "  screenshot\r\n"
            "  log\r\n"
            "  log <none|error|warn|info|debug|verbose>\r\n"
            "  transition <ms> — smooth-transition duration (0 = instant)\r\n"
            "  otainfo — IP, mDNS name, recorded hashes, running slot\r\n"
            "  otaserver on|off — HTTP update server\r\n"
            "  otavalidate — confirm the running image (cancel rollback)\r\n"
            "  otarollback — revert to the previous app slot and reboot\r\n"
            "  otaclearhash app|pack|all — force a re-send on the next ota.py\r\n");
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
        s_hour_offset = hours;
        if (s_clock) eva_clock_set_hour_offset(s_clock, hours);
        eva_weather_canvas_set_time_offset(hours);
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

    if (strcmp(cmd, "cloudinfo") == 0) {
        char info[320];
        eva_weather_canvas_cloud_info(info, sizeof info);
        cdc_send(info);
        return;
    }

    if (strcmp(cmd, "lightning") == 0) {
        eva_weather_canvas_trigger_lightning();
        cdc_send("OK lightning strike queued (fires only in thunderstorm/hail)\r\n");
        return;
    }

    if (strncmp(cmd, "transition", 10) == 0 &&
        (cmd[10] == '\0' || isspace((unsigned char)cmd[10]))) {
        if (cmd[10] == '\0') {
            cdc_send("usage: transition <ms>  (0 = instant)\r\n");
            return;
        }
        char *value = trim_in_place(cmd + 10);
        int ms = atoi(value);
        if (ms < 0) ms = 0;
        eva_weather_canvas_set_transition_ms(ms);
        cdc_sendf("OK transition duration %d ms\r\n", ms);
        return;
    }

    if (strcmp(cmd, "cloudvolume") == 0) {
        bool on = eva_weather_canvas_toggle_volume();
        cdc_sendf("OK cloud volume %s (3-plane %s)\r\n",
                  on ? "ON" : "OFF", on ? "shadow+core+light" : "light-only");
        return;
    }

    if (strcmp(cmd, "weatherlive") == 0) {
        restore_live_weather_and_time();
        cdc_send("OK weatherlive — unpinned, clock offset cleared, fetch queued\r\n");
        return;
    }

    if (strncmp(cmd, "weatherpin", 10) == 0) {
        char *value = trim_in_place(cmd + 10);
        if (strcmp(value, "on") == 0) {
            weather_fetch_set_pinned(true);
            cdc_send("OK weather pinned — live fetch won't overwrite the scene\r\n");
        } else if (strcmp(value, "off") == 0) {
            weather_fetch_set_pinned(false);
            weather_fetch_request();
            cdc_send("OK weather unpinned — live fetch resumes\r\n");
        } else {
            cdc_sendf("weatherpin: %s (usage: weatherpin on|off)\r\n",
                      weather_fetch_is_pinned() ? "on" : "off");
        }
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
        eva_weather_set_transient(&st);
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
        weatherdebug_apply((void *)packed);
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
        eva_weather_set_transient(&st);
        cdc_sendf("OK weather %s %+dC \"%s\"\r\n",
                  weather_kind_name(st.kind), (int)st.temp_c, st.desc);
        return;
    }

    if (strcmp(cmd, "screenshot") == 0) {
        cdc_handle_screenshot();
        return;
    }

    /* LVGL test-mode UI removed (PLAN 10d). Keep the CDC verb so old scripts
     * get a clear redirect instead of "unknown command". */
    if (strcmp(cmd, "test") == 0 ||
        strcmp(cmd, "test on") == 0 ||
        strcmp(cmd, "test off") == 0) {
        cdc_send("ERR test mode disabled; use 'weatherdebug <kind> <frame>'\r\n");
        return;
    }

    /* Force a test wind so both drift directions can be checked on demand.
     *   wind <signed_kph>  e.g. "wind 40" (east, →) or "wind -40" (west, ←)
     *   wind clear         release the override, back to live wind */
    if (strncmp(cmd, "wind", 4) == 0 && isspace((unsigned char)cmd[4])) {
        char *value = trim_in_place(cmd + 4);
        if (strcmp(value, "clear") == 0) {
            eva_weather_canvas_set_test_wind_kph(-1000);
            cdc_send("OK wind cleared (live)\r\n");
        } else {
            int kph = atoi(value);
            eva_weather_canvas_set_test_wind_kph(kph);
            cdc_sendf("OK wind %+d kph (%s)\r\n", kph,
                      kph > 0 ? "east/right" : kph < 0 ? "west/left" : "calm");
        }
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

    /* OTA commands are debug conveniences — the real update path is the HTTP
     * server, because sending a CDC command needs the cable this feature
     * exists to eliminate. `otainfo` is the "where is my device" recovery
     * command: it prints the IP and the mDNS name. */
    if (strcmp(cmd, "otainfo") == 0) {
        char buf[640];
        eva_ota_info(buf, sizeof buf);
        cdc_send(buf);
        return;
    }

    if (strcmp(cmd, "otaserver on") == 0) {
        cdc_sendf("OK ota server %s\r\n", eva_ota_server_start() ? "started" : "FAILED");
        return;
    }

    if (strcmp(cmd, "otaserver off") == 0) {
        eva_ota_server_stop();
        cdc_send("OK ota server stopped\r\n");
        return;
    }

    if (strcmp(cmd, "otavalidate") == 0) {
        esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
        cdc_sendf("OK mark_app_valid -> %s\r\n", esp_err_to_name(err));
        return;
    }

    if (strcmp(cmd, "otarollback") == 0) {
        cdc_send("OK rolling back to the previous slot; rebooting\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return;
    }

    if (strncmp(cmd, "otaclearhash", 12) == 0) {
        const char *what = cmd + 12;
        while (*what == ' ') ++what;
        if (strcmp(what, "app") == 0) {
            eva_ota_state_clear_app_sha();
            cdc_send("OK app hash cleared\r\n");
        } else if (strcmp(what, "pack") == 0) {
            eva_ota_state_clear_pack_sha();
            cdc_send("OK pack hash cleared\r\n");
        } else if (strcmp(what, "all") == 0 || *what == '\0') {
            eva_ota_state_clear_app_sha();
            eva_ota_state_clear_pack_sha();
            cdc_send("OK app+pack hashes cleared\r\n");
        } else {
            cdc_send("ERR otaclearhash app|pack|all\r\n");
        }
        return;
    }

    cdc_send("ERR unknown command; type help\r\n");
}

/* Rollback gate. With CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE a freshly booted
 * OTA image is PENDING_VERIFY: unless it marks itself valid, the bootloader
 * reverts to the previous slot on the next reset.
 *
 * "Valid" here means the two things that make the panel recoverable, not just
 * that main() ran:
 *   - Wi-Fi came back. Without it no future update can be pushed, which is the
 *     real brick condition for a device with no cable workflow.
 *   - The render loop is actually producing frames.
 * The 90 s Wi-Fi budget covers WIFI_START_DELAY_MS (10 s) plus a full
 * WIFI_CONNECT_WAIT_MS (30 s) attempt with room to spare — do not shorten it
 * below ~60 s or a slow AP will trigger a spurious rollback. */
static void ota_validate_task(void *arg)
{
    (void)arg;

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "image is PENDING_VERIFY — running self-check");

    /* Poll instead of one long wait: eva_wifi_start() creates its event group
     * inside its own task, and eva_wifi_wait_connected() called before that
     * exists just sleeps out the entire timeout — which here would look like
     * "Wi-Fi never came up" and trigger a rollback of a perfectly good image. */
    bool wifi_ok = false;
    for (int i = 0; i < 90 && !wifi_ok; ++i) {
        wifi_ok = eva_wifi_is_connected();
        if (!wifi_ok) vTaskDelay(pdMS_TO_TICKS(1000));
    }
    vTaskDelay(pdMS_TO_TICKS(5000));
    bool render_ok = eva_weather_canvas_last_tick_hz() > 0;

    if (wifi_ok && render_ok) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "self-check passed — image marked VALID");
    } else {
        ESP_LOGE(TAG, "self-check FAILED (wifi=%d render=%d) — rolling back",
                 wifi_ok, render_ok);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
    vTaskDelete(NULL);
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
    ESP_ERROR_CHECK(bsp_display_brightness_init());   /* enable LEDC PWM backlight control */
    ESP_ERROR_CHECK(bsp_display_backlight_on());       /* 100% after PWM init */
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

    /* If the bootloader ran the other slot, the recorded app hash describes an
     * image that is not running — drop it so ota.py re-pushes. */
    if (eva_ota_state_check_rollback()) {
        ESP_LOGW(TAG, "app was rolled back to %s", esp_ota_get_running_partition()->label);
    }
    eva_ota_start(s_ota_status);
    xTaskCreate(ota_validate_task, "ota_validate", 4096, NULL, 2, NULL);

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
