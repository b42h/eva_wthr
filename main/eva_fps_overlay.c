#include "eva_fps_overlay.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "eva_weather_canvas.h"

static const char *TAG = "eva_fps_overlay";

struct eva_fps_overlay_s {
    lv_obj_t *label;
    lv_timer_t *timer;
    char prev[32];
};

static void style_floating_card(lv_obj_t *obj, uint32_t bg_hex, uint32_t border_hex)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_80, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(border_hex), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_80, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_radius(obj, 8, 0);
}

static void fps_tick_cb(lv_timer_t *timer)
{
    eva_fps_overlay_t *self = (eva_fps_overlay_t *)lv_timer_get_user_data(timer);
    if (!self || !self->label) return;

    uint32_t hz = eva_weather_canvas_last_tick_hz();
    uint32_t work = eva_weather_canvas_last_work_us();
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu fps  %lu us", (unsigned long)hz, (unsigned long)work);
    if (strcmp(buf, self->prev) != 0) {
        lv_label_set_text(self->label, buf);
        memcpy(self->prev, buf, sizeof(self->prev));
        lv_obj_move_foreground(self->label);
    }
}

eva_fps_overlay_t *eva_fps_overlay_create(lv_obj_t *parent)
{
    eva_fps_overlay_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }

    self->label = lv_label_create(parent);
    lv_obj_set_style_text_font(self->label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(self->label, lv_color_hex(0xa0e0ff), 0);
    lv_obj_set_style_text_opa(self->label, LV_OPA_80, 0);
    style_floating_card(self->label, 0x101820, 0x4ea3d8);
    lv_obj_set_style_bg_opa(self->label, LV_OPA_60, 0);
    lv_obj_set_style_pad_all(self->label, 6, 0);
    lv_obj_align(self->label, LV_ALIGN_TOP_RIGHT, -12, 12);
    lv_label_set_text(self->label, "-- fps");
    lv_obj_add_flag(self->label, LV_OBJ_FLAG_HIDDEN);

    self->timer = lv_timer_create(fps_tick_cb, 1000, self);
    return self;
}

void eva_fps_overlay_destroy(eva_fps_overlay_t *self)
{
    if (!self) return;
    if (self->timer) {
        lv_timer_delete(self->timer);
    }
    free(self);
}

void eva_fps_overlay_show(eva_fps_overlay_t *self, bool show)
{
    if (!self || !self->label) return;
    if (show) {
        lv_obj_remove_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    }
}
