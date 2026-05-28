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

#include "eva_weather_canvas.h"

#define CLOCK_UPDATE_MS 987

static const char *TAG = "eva_clock";

struct eva_clock_s {
    TaskHandle_t task;
    int hour_offset;
    char text[16];
    volatile bool stop;
};

/* FreeRTOS task replacing the previous lv_timer-based driver. The native
 * render pipeline doesn't register an LVGL display, so lv_timer callbacks
 * no longer fire; running the clock as its own low-priority task keeps the
 * minute counter live independent of the display path. */
static void clock_task(void *arg)
{
    eva_clock_t *self = (eva_clock_t *)arg;
    while (!self->stop) {
        eva_clock_tick(self);
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
}

const char *eva_clock_text(const eva_clock_t *self)
{
    return self ? self->text : "";
}
