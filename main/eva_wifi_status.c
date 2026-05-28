#include "eva_wifi_status.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"

static const char *TAG = "eva_wifi_status";

struct eva_wifi_status_s {
    lv_obj_t *label;
    SemaphoreHandle_t lock;
    char text[64];
};

static void wifi_status_apply_cb(void *user)
{
    eva_wifi_status_t *self = (eva_wifi_status_t *)user;
    if (!self || !self->label || !self->lock) return;

    if (xSemaphoreTake(self->lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        lv_label_set_text(self->label, self->text);
        xSemaphoreGive(self->lock);
    }
}

eva_wifi_status_t *eva_wifi_status_create(lv_obj_t *parent)
{
    eva_wifi_status_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }

    strlcpy(self->text, "Wi-Fi: ...", sizeof(self->text));
    self->lock = xSemaphoreCreateMutex();
    self->label = lv_label_create(parent);
    lv_obj_set_style_text_font(self->label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(self->label, lv_color_hex(0xd0d6de), 0);
    lv_obj_set_style_text_opa(self->label, LV_OPA_60, 0);
    lv_obj_align(self->label, LV_ALIGN_TOP_RIGHT, -12, 44);
    lv_label_set_text(self->label, self->text);
    lv_obj_add_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    return self;
}

void eva_wifi_status_destroy(eva_wifi_status_t *self)
{
    if (!self) return;
    if (self->lock) {
        vSemaphoreDelete(self->lock);
    }
    free(self);
}

void eva_wifi_status_set_text(eva_wifi_status_t *self, const char *msg)
{
    if (!self || !self->lock || !msg) return;

    if (xSemaphoreTake(self->lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        strlcpy(self->text, msg, sizeof(self->text));
        xSemaphoreGive(self->lock);
        lv_async_call(wifi_status_apply_cb, self);
    }
}

void eva_wifi_status_show(eva_wifi_status_t *self, bool show)
{
    if (!self || !self->label) return;
    if (show) {
        lv_obj_remove_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(self->label, LV_OBJ_FLAG_HIDDEN);
    }
}

void eva_wifi_status_move_foreground(eva_wifi_status_t *self)
{
    if (!self || !self->label) return;
    lv_obj_move_foreground(self->label);
}
