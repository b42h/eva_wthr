#pragma once

#include <stdbool.h>

#include "esp_lcd_types.h"
#include "lvgl.h"

#include "eva_weather.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EVA_WEATHER_CANVAS_W 800
#define EVA_WEATHER_CANVAS_H 480

lv_obj_t *eva_weather_canvas_init(lv_obj_t *parent);
void eva_weather_canvas_init_native(esp_lcd_panel_handle_t panel);
void eva_weather_canvas_set_kind(weather_kind_t kind);
void eva_weather_canvas_set_weather(const weather_state_t *st);
void eva_weather_canvas_show(bool show);
void eva_weather_canvas_set_time_offset(int hours);
void eva_weather_canvas_set_clock_text(const char *text);
void eva_weather_canvas_set_temp_text(const char *text);
void eva_weather_canvas_set_desc_text(const char *text);

/* Read-only pointer to the live 800x480 RGB565 scene buffer for snapshot
 * use (see eva_screenshot.c). This is the direct landscape render target,
 * not the rotated portrait DPI framebuffer. Returns NULL until init.
 *
 * The buffer is updated from the LVGL task — callers from other tasks should
 * take the LVGL lock before reading it. */
const uint16_t *eva_weather_canvas_display_buf(void);

/* Last computed canvas tick rate in Hz. Updated alongside the FPS log line
 * (every LOG_EVERY_FRAMES ticks). Zero until the first window completes. */
uint32_t eva_weather_canvas_last_tick_hz(void);

/* Last frame's render work time in microseconds (render_weather +
 * upscale combined). Useful as an FPS-independent measure of pipeline load. */
uint32_t eva_weather_canvas_last_work_us(void);

void eva_weather_canvas_last_breakdown_us(uint32_t *bg_us, uint32_t *cloud_us,
                                          uint32_t *particle_us, uint32_t *lightning_us,
                                          uint32_t *lvgl_us, uint32_t *vsync_us);
void eva_weather_canvas_cloud_budget(uint16_t *active, uint16_t *max);

/* Test-mode overrides. When non-negative, these values override the live
 * weather state. Pass -1 to clear the override and return to live values.
 *   pct_high/mid/low: 0..100 cloud coverage per altitude band
 *   wind_kph: 0..120 horizontal wind speed (sign: positive = east, negative = west)
 * The overrides take effect on the next render tick. */
void eva_weather_canvas_set_test_cloud_pct(int high, int mid, int low);
void eva_weather_canvas_set_test_wind_kph(int wind_kph);
void eva_weather_canvas_clear_test_overrides(void);

#ifdef __cplusplus
}
#endif
