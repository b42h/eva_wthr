#ifndef EVA_WIFI_STATUS_H
#define EVA_WIFI_STATUS_H

#include <stdbool.h>

#include "lvgl.h"

typedef struct eva_wifi_status_s eva_wifi_status_t;

eva_wifi_status_t *eva_wifi_status_create(lv_obj_t *parent);
void eva_wifi_status_destroy(eva_wifi_status_t *self);

void eva_wifi_status_set_text(eva_wifi_status_t *self, const char *msg);
void eva_wifi_status_show(eva_wifi_status_t *self, bool show);
void eva_wifi_status_move_foreground(eva_wifi_status_t *self);

#endif
