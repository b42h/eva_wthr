#include "eva_settings.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "eva_settings";

struct eva_settings_s {
    char tz[64];
};

eva_settings_t *eva_settings_create(void)
{
    eva_settings_t *self = calloc(1, sizeof(*self));
    if (!self) {
        ESP_LOGE(TAG, "alloc failed");
        return NULL;
    }
    strlcpy(self->tz, "EET-2EEST,M3.5.0/3,M10.5.0/4", sizeof(self->tz));
    return self;
}

void eva_settings_destroy(eva_settings_t *self)
{
    free(self);
}

void eva_settings_load(eva_settings_t *self)
{
    if (!self) return;

    nvs_handle_t h;
    if (nvs_open("eva", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t sz = sizeof(self->tz);
    (void)nvs_get_str(h, "tz", self->tz, &sz);
    nvs_close(h);
}

void eva_settings_save(const eva_settings_t *self)
{
    if (!self) return;

    nvs_handle_t h;
    if (nvs_open("eva", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_str(h, "tz", self->tz);
    (void)nvs_commit(h);
    nvs_close(h);
}

const char *eva_settings_get_tz(const eva_settings_t *self)
{
    return self ? self->tz : "UTC0";
}

void eva_settings_set_tz(eva_settings_t *self, const char *tz)
{
    if (!self || !tz) return;
    strlcpy(self->tz, tz, sizeof(self->tz));
}

size_t eva_settings_tz_capacity(const eva_settings_t *self)
{
    (void)self;
    return sizeof(((eva_settings_t *)0)->tz);
}
