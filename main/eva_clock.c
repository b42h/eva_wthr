#include "eva_clock.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bsp/display.h"
#include "eva_weather_canvas.h"

#define CLOCK_UPDATE_MS 987

/* Backlight floor/ceiling and the sun-driven ramp windows. By day the panel
 * runs at DAY_PERCENT; after sunset it eases down to NIGHT_DIM_PERCENT over
 * DUSK_FADE_MIN, holds through the night, then eases back up over DAWN_FADE_MIN
 * ending at sunrise. */
#define NIGHT_DIM_PERCENT   10
#define DAY_PERCENT         100
#define DUSK_FADE_MIN       180   /* sunset → 10% over 3 h */
#define DAWN_FADE_MIN       60    /* 10% → 100% over the hour before sunrise */

/* Fallback fixed schedule when sun times are unknown: 10% in [01:00, 06:00). */
#define NIGHT_DIM_START_H   1
#define NIGHT_DIM_END_H     6

#define BRIGHT_FADE_STEPS   16
#define BRIGHT_FADE_STEP_MS 30

static const char *TAG = "eva_clock";

static void apply_brightness_schedule(eva_clock_t *self, int hour);
static int eva_clock_current_minute(const eva_clock_t *self);

struct eva_clock_s {
    TaskHandle_t task;
    int hour_offset;
    char text[16];
    char date_text[24];
    int cur_brightness;   /* last value pushed to the panel; -1 = not set yet */
    int sunrise_min;      /* minutes-of-day, -1 = unknown → fixed-schedule fallback */
    int sunset_min;       /* minutes-of-day, -1 = unknown */
    volatile bool stop;
};

/* Step the LEDC backlight from `from`% to `to`% over ~0.5 s. The first call
 * (from < 0) snaps straight to `to` with no fade. Runs in the clock task. */
static void fade_brightness(int from, int to)
{
    if (from < 0) {
        bsp_display_brightness_set(to);
        return;
    }
    for (int i = 1; i <= BRIGHT_FADE_STEPS; ++i) {
        int v = from + (to - from) * i / BRIGHT_FADE_STEPS;
        bsp_display_brightness_set(v);
        vTaskDelay(pdMS_TO_TICKS(BRIGHT_FADE_STEP_MS));
    }
}

/* FreeRTOS task replacing the previous lv_timer-based driver. The native
 * render pipeline doesn't register an LVGL display, so lv_timer callbacks
 * no longer fire; running the clock as its own low-priority task keeps the
 * minute counter live independent of the display path. */
static void clock_task(void *arg)
{
    eva_clock_t *self = (eva_clock_t *)arg;
    while (!self->stop) {
        eva_clock_tick(self);
        /* Brightness schedule runs ONLY here (single context) so two fades
         * can never race on the LEDC driver. */
        apply_brightness_schedule(self, eva_clock_current_minute(self));
        vTaskDelay(pdMS_TO_TICKS(CLOCK_UPDATE_MS));
    }
    self->task = NULL;
    vTaskDelete(NULL);
}

eva_clock_t *eva_clock_create(void)
{
    eva_clock_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }
    self->cur_brightness = -1;   /* force the first tick to apply day/night level */
    self->sunrise_min = -1;      /* unknown until weather coordinator reports */
    self->sunset_min = -1;

    /* Render the current minute immediately so the canvas doesn't show the
     * default "00:00" between init and the first task tick. */
    eva_clock_tick(self);

    if (xTaskCreate(clock_task, "eva_clock", 3072, self, 2, &self->task) != pdPASS) {
        ESP_LOGE(TAG, "clock task create failed");
        free(self);
        return NULL;
    }
    return self;
}

void eva_clock_destroy(eva_clock_t *self)
{
    if (!self) return;
    if (self->task) {
        self->stop = true;
        /* Task self-deletes once it sees `stop`. Yield long enough for the
         * 987 ms sleep + cleanup to complete. */
        vTaskDelay(pdMS_TO_TICKS(CLOCK_UPDATE_MS + 200));
    }
    free(self);
}

void eva_clock_set_hour_offset(eva_clock_t *self, int hours)
{
    if (!self) return;
    self->hour_offset = hours;
    eva_clock_tick(self);
}

void eva_clock_set_sun_times(eva_clock_t *self, int sunrise_min, int sunset_min)
{
    if (!self) return;
    /* Two plain ints; the clock task reads them next tick. A torn read at worst
     * picks one stale value for one 987 ms tick — harmless for a backlight ramp,
     * so no lock needed. Out-of-range or inverted pairs disable the ramp. */
    self->sunrise_min = (sunrise_min >= 0 && sunrise_min < 1440) ? sunrise_min : -1;
    self->sunset_min  = (sunset_min  >= 0 && sunset_min  < 1440) ? sunset_min  : -1;
}

void eva_clock_tick(eva_clock_t *self)
{
    if (!self) return;

    time_t now = time(NULL);
    struct tm tm_now = { 0 };
    if (now > 1700000000) {
        if (self->hour_offset != 0) {
            now += (time_t)self->hour_offset * 3600;
        }
        localtime_r(&now, &tm_now);
    } else {
        int total_s = (int)((esp_timer_get_time() / 1000000) % (24 * 3600));
        total_s += self->hour_offset * 3600;
        total_s %= (24 * 3600);
        if (total_s < 0) {
            total_s += 24 * 3600;
        }
        tm_now.tm_hour = total_s / 3600;
        tm_now.tm_min = (total_s / 60) % 60;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    if (strcmp(buf, self->text) != 0) {
        strlcpy(self->text, buf, sizeof(self->text));
        eva_weather_canvas_set_clock_text(self->text);
    }

    if (now > 1700000000) {
        char date_buf[24];
        snprintf(date_buf, sizeof(date_buf), "%02d.%02d.%04d",
                 tm_now.tm_mday, tm_now.tm_mon + 1, tm_now.tm_year + 1900);
        if (strcmp(date_buf, self->date_text) != 0) {
            strlcpy(self->date_text, date_buf, sizeof(self->date_text));
            eva_weather_canvas_set_date_text(self->date_text);
        }
    }
}

/* Linear interpolation: at x=0 → a, at x=span → b, clamped to [a,b] order. */
static int lerp_pct(int a, int b, int x, int span)
{
    if (span <= 0) return b;
    if (x <= 0) return a;
    if (x >= span) return b;
    return a + (b - a) * x / span;
}

/* Target backlight % for a minute-of-day, driven by sun times when known:
 *   day (sunrise..sunset)            → 100%
 *   dusk (sunset .. sunset+3h)       → linear 100% → 10%
 *   night                            → 10%
 *   dawn (sunrise-1h .. sunrise)     → linear 10% → 100%
 * Falls back to the fixed [01:00,06:00)→10% schedule when sun times are
 * unknown. Windows are clamped to the wall clock so they wrap cleanly. */
static int brightness_target_for_minute(const eva_clock_t *self, int m)
{
    int sr = self->sunrise_min;
    int ss = self->sunset_min;
    if (sr < 0 || ss < 0 || ss <= sr) {
        int hour = m / 60;
        return (hour >= NIGHT_DIM_START_H && hour < NIGHT_DIM_END_H)
                   ? NIGHT_DIM_PERCENT : DAY_PERCENT;
    }

    /* Daytime: full brightness. */
    if (m >= sr && m < ss) {
        return DAY_PERCENT;
    }

    /* Dusk ramp: sunset → sunset+DUSK_FADE_MIN, 100% → 10%. */
    int into_dusk = m - ss;
    if (into_dusk >= 0 && into_dusk < DUSK_FADE_MIN) {
        return lerp_pct(DAY_PERCENT, NIGHT_DIM_PERCENT, into_dusk, DUSK_FADE_MIN);
    }

    /* Dawn ramp: sunrise-DAWN_FADE_MIN → sunrise, 10% → 100%. */
    int before_sunrise = sr - m;
    if (before_sunrise > 0 && before_sunrise <= DAWN_FADE_MIN) {
        return lerp_pct(NIGHT_DIM_PERCENT, DAY_PERCENT,
                        DAWN_FADE_MIN - before_sunrise, DAWN_FADE_MIN);
    }

    /* Everything else (deep night) holds the dim floor. */
    return NIGHT_DIM_PERCENT;
}

/* Apply the sun-driven backlight ramp for the given minute-of-day. MUST be
 * called from a single context (the clock task) only — the writes touch the
 * blocking LEDC PWM and cur_brightness, which aren't safe against concurrent
 * fades. eva_clock_tick() is also invoked synchronously from the CDC
 * set_hour_offset path, so the schedule lives here, NOT in eva_clock_tick.
 *
 * The first apply (cur_brightness < 0) and the fixed-schedule fallback take
 * the 16-step fade for a smooth jump. On the continuous sun-driven ramp the
 * per-tick step is ~1% every couple of minutes, so we write it directly — a
 * fade-per-step would just be 16× redundant PWM writes for an invisible delta. */
static void apply_brightness_schedule(eva_clock_t *self, int minute_of_day)
{
    int target = brightness_target_for_minute(self, minute_of_day);
    if (target == self->cur_brightness) return;

    bool known_sun = (self->sunrise_min >= 0 && self->sunset_min > self->sunrise_min);
    if (self->cur_brightness < 0 || !known_sun) {
        fade_brightness(self->cur_brightness, target);   /* jump: smooth fade */
    } else {
        bsp_display_brightness_set(target);              /* ramp: direct write */
    }
    self->cur_brightness = target;
}

int eva_clock_current_brightness(const eva_clock_t *self)
{
    return self ? self->cur_brightness : -1;
}

/* Current local minute-of-day (0..1439) using the same time source as
 * eva_clock_tick (real time once synced, uptime fallback otherwise), including
 * the test hour-offset. */
static int eva_clock_current_minute(const eva_clock_t *self)
{
    time_t now = time(NULL);
    if (now > 1700000000) {
        now += (time_t)self->hour_offset * 3600;
        struct tm tm_now = { 0 };
        localtime_r(&now, &tm_now);
        return tm_now.tm_hour * 60 + tm_now.tm_min;
    }
    int total_s = (int)((esp_timer_get_time() / 1000000) % (24 * 3600));
    total_s += self->hour_offset * 3600;
    total_s %= (24 * 3600);
    if (total_s < 0) total_s += 24 * 3600;
    return total_s / 60;
}

const char *eva_clock_text(const eva_clock_t *self)
{
    return self ? self->text : "";
}
