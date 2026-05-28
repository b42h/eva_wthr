#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Encode the current weather scene buffer (800x480 RGB565) into a JPEG using
 * the P4 hardware encoder. The output buffer is allocated once on first call
 * and reused; caller MUST NOT free it.
 *
 * Returns ESP_OK on success and fills *out_ptr / *out_size with the JPEG
 * stream pointer (within the persistent encoder output buffer) and byte size.
 * Returns an error if the scene buffer isn't ready or the encoder fails. */
esp_err_t eva_screenshot_capture(const uint8_t **out_ptr, size_t *out_size);

/* Optional: install the screenshot subsystem and prime the JPEG encoder so
 * the first capture isn't artificially slow. Safe to call multiple times. */
void eva_screenshot_init(void);

#ifdef __cplusplus
}
#endif
