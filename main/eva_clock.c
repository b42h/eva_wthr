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

/* Night dim: 10% backlight in [01:00, 06:00), 100% otherwise, faded ~0.5 s. */
#define NIGHT_DIM_START_H   1
#define NIGHT_DIM_END_H     6
#define NIGHT_DIM_PERCENT   10
#define DAY_PERCENT         100
#define BRIGHT_FADE_STEPS   16
#define BRIGHT_FADE_STEP_MS 30

static const char *TAG = "eva_clock";

static void apply_brightness_schedule(eva_clock_t *self, int hour);
static int eva_clock_current_hour(const eva_clock_t *self);

struct eva_clock_s {
    TaskHandle_t task;
    int hour_offset;
    char text[16];
    char date_text[24];
    int cur_brightness;   /* last value pushed to the panel; -1 = not set yet */
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
        apply_brightness_schedule(self, eva_clock_current_hour(self));
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

/* Apply the night-dim schedule for the given hour: [01:00, 06:00) → 10%, else
 * 100%, faded on change. MUST be called from a single context (the clock task)
 * only — the fade does blocking bsp_display_brightness_set() PWM writes, and
 * the LEDC driver + cur_brightness state are not safe against two concurrent
 * fades. eva_clock_tick() is also invoked synchronously from the CDC
 * set_hour_offset path, so the schedule lives here, NOT in eva_clock_tick. */
static void apply_brightness_schedule(eva_clock_t *self, int hour)
{
    int target = (hour >= NIGHT_DIM_START_H && hour < NIGHT_DIM_END_H)
                     ? NIGHT_DIM_PERCENT : DAY_PERCENT;
    if (target != self->cur_brightness) {
        fade_brightness(self->cur_brightness, target);
        self->cur_brightness = target;
    }
}

/* Current local hour using the same time source as eva_clock_tick (real time
 * once synced, uptime fallback otherwise), including the test hour-offset. */
static int eva_clock_current_hour(const eva_clock_t *self)
{
    time_t now = time(NULL);
    if (now > 1700000000) {
        now += (time_t)self->hour_offset * 3600;
        struct tm tm_now = { 0 };
        localtime_r(&now, &tm_now);
        return tm_now.tm_hour;
    }
    int total_s = (int)((esp_timer_get_time() / 1000000) % (24 * 3600));
    total_s += self->hour_offset * 3600;
    total_s %= (24 * 3600);
    if (total_s < 0) total_s += 24 * 3600;
    return total_s / 3600;
}

const char *eva_clock_text(const eva_clock_t *self)
{
    return self ? self->text : "";
}
