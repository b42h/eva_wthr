#include "eva_ota_status.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "eva_ota_status";

/* Ukrainian glyphs (0x400-0x4FF) — the montserrat fonts LVGL ships have Latin
 * only, so the banner would render as boxes with them. */
LV_FONT_DECLARE(eva_font_uk_22);

/* ~4 Hz. See the header for why this limit exists. */
#define PROGRESS_MIN_INTERVAL_US 250000

struct eva_ota_status_s {
    lv_obj_t *label;
    SemaphoreHandle_t lock;
    char text[96];
    bool visible;
    int64_t last_paint_us;
    int last_pct;
};

static void status_apply_cb(void *user)
{
    eva_ota_status_t *self = (eva_ota_status_t *)user;
    if (!self || !self->label || !self->lock) return;

    if (xSemaphoreTake(self->lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    bool visible = self->visible;
    if (visible) {
        lv_label_set_text(self->label, self->text);
    }
    xSemaphoreGive(self->lock);

    /* Visibility is toggled here rather than in the setter because both touch
     * LVGL objects and must run on the LVGL thread. */
    if (visible) {
        lv_obj_remove_flag(self->label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(self->label);
    } else {
        lv_obj_add_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    }
}

eva_ota_status_t *eva_ota_status_create(lv_obj_t *parent)
{
    eva_ota_status_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }

    self->lock = xSemaphoreCreateMutex();
    self->last_pct = -1;
    self->label = lv_label_create(parent);
    lv_obj_set_style_text_font(self->label, &eva_font_uk_22, 0);
    lv_obj_set_style_text_color(self->label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_color(self->label, lv_color_hex(0x101418), 0);
    lv_obj_set_style_bg_opa(self->label, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(self->label, 10, 0);
    lv_obj_set_style_radius(self->label, 6, 0);
    lv_obj_align(self->label, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(self->label, "");
    lv_obj_add_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    return self;
}

void eva_ota_status_destroy(eva_ota_status_t *self)
{
    if (!self) return;
    if (self->lock) {
        vSemaphoreDelete(self->lock);
    }
    free(self);
}

void eva_ota_status_set(eva_ota_status_t *self, const char *msg)
{
    if (!self || !self->lock) return;

    if (xSemaphoreTake(self->lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    if (msg) {
        strlcpy(self->text, msg, sizeof(self->text));
        self->visible = true;
    } else {
        self->visible = false;
    }
    /* An explicit message always paints — only the progress path is throttled. */
    self->last_paint_us = esp_timer_get_time();
    self->last_pct = -1;
    xSemaphoreGive(self->lock);

    lv_async_call(status_apply_cb, self);
}

void eva_ota_status_reset_throttle(eva_ota_status_t *self)
{
    if (!self || !self->lock) return;
    if (xSemaphoreTake(self->lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        self->last_paint_us = 0;
        self->last_pct = -1;
        xSemaphoreGive(self->lock);
    }
}

void eva_ota_status_progress(eva_ota_status_t *self, const char *label,
                             size_t done, size_t total)
{
    if (!self || !self->lock || !label || total == 0) return;

    int pct = (int)((uint64_t)done * 100u / (uint64_t)total);
    if (pct > 100) pct = 100;

    int64_t now = esp_timer_get_time();
    bool paint = false;

    if (xSemaphoreTake(self->lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }
    /* Paint when the rate limit has elapsed AND the number actually changed, or
     * whenever we reach 100% so the final frame is never dropped. */
    if (pct != self->last_pct &&
        (pct >= 100 || now - self->last_paint_us >= PROGRESS_MIN_INTERVAL_US)) {
        snprintf(self->text, sizeof(self->text), "%s %d%%", label, pct);
        self->visible = true;
        self->last_paint_us = now;
        self->last_pct = pct;
        paint = true;
    }
    xSemaphoreGive(self->lock);

    if (paint) {
        lv_async_call(status_apply_cb, self);
    }
}

void eva_ota_status_move_foreground(eva_ota_status_t *self)
{
    if (!self || !self->label) return;
    lv_obj_move_foreground(self->label);
}
