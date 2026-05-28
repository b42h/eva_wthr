#ifndef EVA_FPS_OVERLAY_H
#define EVA_FPS_OVERLAY_H

#include <stdbool.h>

#include "lvgl.h"

typedef struct eva_fps_overlay_s eva_fps_overlay_t;

eva_fps_overlay_t *eva_fps_overlay_create(lv_obj_t *parent);
void eva_fps_overlay_destroy(eva_fps_overlay_t *self);

void eva_fps_overlay_show(eva_fps_overlay_t *self, bool show);

#endif
