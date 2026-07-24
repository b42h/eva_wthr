#pragma once

#include <stdbool.h>
#include <stddef.h>

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
/* Duration of smooth weather transitions, in milliseconds. 0 = instant
 * (snap, for A/B comparison). Default is EVA_WX_TRANSITION_DEFAULT_S. */
void eva_weather_canvas_set_transition_ms(int ms);
void eva_weather_canvas_set_clock_text(const char *text);
void eva_weather_canvas_set_date_text(const char *text);
void eva_weather_canvas_set_temp_text(const char *text);
void eva_weather_canvas_set_desc_text(const char *text);

/* Read-only pointer to the live 800x480 RGB565 scene buffer for snapshot
 * use (see eva_screenshot.c). This is the direct landscape render target,
 * not the rotated portrait DPI framebuffer. Returns NULL until init.
 *
 * The buffer is updated from the LVGL task — callers from other tasks should
 * take the LVGL lock before reading it. */
const uint16_t *eva_weather_canvas_display_buf(void);

/* Force a lightning strike on the next frame (CDC "lightning" test command).
 * Only fires while the active kind is THUNDERSTORM or HAIL — the lightning
 * state machine is gated to those kinds. */
void eva_weather_canvas_trigger_lightning(void);

/* Toggle 3-plane volume cloud rendering (shadow+core under the lit cap).
 * Returns the new state. CDC `cloudvolume` test command. */
bool eva_weather_canvas_toggle_volume(void);

/* Copy a coherent frame into `dst` under the render lock — unlike reading
 * display_buf() directly, this can never observe a half-painted frame.
 * `dst_bytes` must be ≥ 800*480*2. Returns false if the canvas isn't up. */
bool eva_weather_canvas_copy_display(uint16_t *dst, size_t dst_bytes);

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
void eva_weather_canvas_cloud_info(char *buf, size_t buf_len);

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
