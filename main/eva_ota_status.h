#ifndef EVA_OTA_STATUS_H
#define EVA_OTA_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

/* Centered on-screen banner shown only while an update is running.
 *
 * An LVGL label overlay, NOT text baked into the canvas scene base — the base
 * is a cached image with its own dirty-flag protocol (see CLAUDE.md), and an
 * update banner has no business invalidating it. */

typedef struct eva_ota_status_s eva_ota_status_t;

eva_ota_status_t *eva_ota_status_create(lv_obj_t *parent);
void eva_ota_status_destroy(eva_ota_status_t *self);

/* Show `msg` (hides the banner when msg is NULL). Safe from any task: the text
 * is copied under a mutex and applied on the LVGL thread via lv_async_call. */
void eva_ota_status_set(eva_ota_status_t *self, const char *msg);

/* Progress banner: "<label> 42%". Rate-limited internally to ~4 Hz — a 7.6 MB
 * pack arrives in ~1950 chunks, and one lv_async_call per chunk would flood the
 * async queue and cost exactly the FPS this project spent so long protecting.
 * `done`/`total` are byte counts; total == 0 is ignored. */
void eva_ota_status_progress(eva_ota_status_t *self, const char *label,
                             size_t done, size_t total);

/* Drop the rate limiter so the next progress call is guaranteed to paint.
 * Call when starting a new phase, so its first frame is never swallowed. */
void eva_ota_status_reset_throttle(eva_ota_status_t *self);

void eva_ota_status_move_foreground(eva_ota_status_t *self);

#endif
