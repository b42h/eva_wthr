#include "eva_screenshot.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "driver/jpeg_encode.h"
#include "driver/jpeg_types.h"

#include "eva_weather_canvas.h"

/* Capture the live 800x480 weather render buffer into a JPEG using the P4
 * hardware encoder.
 *
 * The framebuffer we encode is the direct landscape scene buffer returned by
 * eva_weather_canvas_display_buf(). That keeps the screenshot path aligned
 * with the new direct-render pipeline and avoids depending on the LVGL tree
 * or the rotated DPI framebuffer.
 *
 * Memory layout:
 *   in_buf      — DMA-aligned PSRAM allocated by jpeg_alloc_encoder_mem,
 *                 holds one RGB565 copy of the rendered scene
 *                 (800*480*2 = 750 KB). Allocated once and reused.
 *   out_buf     — DMA-aligned PSRAM for the resulting JPEG bitstream. */

#define EVA_SCREENSHOT_W   EVA_WEATHER_CANVAS_W   /* 800 */
#define EVA_SCREENSHOT_H   EVA_WEATHER_CANVAS_H   /* 480 */
#define EVA_JPEG_QUALITY   80
/* Output budget: assume worst-case ~2.5 bpp at quality 80 for noisy scenes. */
#define EVA_JPEG_OUT_CAP   (160 * 1024)

static const char *TAG = "eva_screenshot";

static jpeg_encoder_handle_t s_encoder;
static uint8_t *s_in_buf;     /* RGB565 snapshot, DMA-aligned */
static size_t   s_in_buf_sz;
static uint8_t *s_out_buf;    /* JPEG output, DMA-aligned */
static size_t   s_out_buf_sz;
static bool     s_disabled;   /* true after a hard error so we don't retry forever */

static bool ensure_encoder(void)
{
    if (s_disabled) return false;
    if (s_encoder && s_in_buf && s_out_buf) return true;

    if (!s_encoder) {
        jpeg_encode_engine_cfg_t cfg = {
            .intr_priority = 0,
            .timeout_ms    = 2000,
        };
        esp_err_t err = jpeg_new_encoder_engine(&cfg, &s_encoder);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "jpeg_new_encoder_engine failed: 0x%x", err);
            s_disabled = true;
            return false;
        }
    }
    if (!s_in_buf) {
        size_t need = EVA_SCREENSHOT_W * EVA_SCREENSHOT_H * 2;  /* RGB565 */
        jpeg_encode_memory_alloc_cfg_t mem = {
            .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER,
        };
        s_in_buf = jpeg_alloc_encoder_mem(need, &mem, &s_in_buf_sz);
        if (!s_in_buf || s_in_buf_sz < need) {
            ESP_LOGE(TAG, "input buffer alloc failed (need %u, got %u)",
                     (unsigned)need, (unsigned)s_in_buf_sz);
            s_disabled = true;
            return false;
        }
    }
    if (!s_out_buf) {
        jpeg_encode_memory_alloc_cfg_t mem = {
            .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER,
        };
        s_out_buf = jpeg_alloc_encoder_mem(EVA_JPEG_OUT_CAP, &mem, &s_out_buf_sz);
        if (!s_out_buf || s_out_buf_sz < 4096) {
            ESP_LOGE(TAG, "output buffer alloc failed (cap %u, got %u)",
                     (unsigned)EVA_JPEG_OUT_CAP, (unsigned)s_out_buf_sz);
            s_disabled = true;
            return false;
        }
    }
    return true;
}

void eva_screenshot_init(void)
{
    /* Best-effort: warm up the encoder so the first runtime capture isn't
     * artificially slow. If anything fails here, we just lazy-init on the
     * first capture call instead. */
    (void)ensure_encoder();
    if (s_encoder && s_in_buf && s_out_buf) {
        ESP_LOGI(TAG, "JPEG encoder ready (in %u KB, out %u KB)",
                 (unsigned)(s_in_buf_sz / 1024),
                 (unsigned)(s_out_buf_sz / 1024));
    }
}

esp_err_t eva_screenshot_capture(const uint8_t **out_ptr, size_t *out_size)
{
    if (!out_ptr || !out_size) return ESP_ERR_INVALID_ARG;
    *out_ptr = NULL;
    *out_size = 0;

    if (!ensure_encoder()) {
        return ESP_FAIL;
    }

    int64_t t0 = esp_timer_get_time();
    const uint16_t *src = eva_weather_canvas_display_buf();
    if (!src) {
        ESP_LOGW(TAG, "no render buffer available");
        return ESP_ERR_INVALID_STATE;
    }
    memcpy(s_in_buf, src, EVA_SCREENSHOT_W * EVA_SCREENSHOT_H * sizeof(uint16_t));
    ESP_LOGD(TAG, "screenshot src: raw-weather");
    int64_t t_copy = esp_timer_get_time() - t0;

    jpeg_encode_cfg_t enc = {
        .height        = EVA_SCREENSHOT_H,
        .width         = EVA_SCREENSHOT_W,
        .src_type      = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample    = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = EVA_JPEG_QUALITY,
    };
    uint32_t out_bytes = 0;
    t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s_encoder, &enc,
                                         s_in_buf, EVA_SCREENSHOT_W * EVA_SCREENSHOT_H * 2,
                                         s_out_buf, s_out_buf_sz, &out_bytes);
    int64_t t_enc = esp_timer_get_time() - t0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "jpeg_encoder_process failed: 0x%x", err);
        return err;
    }
    if (out_bytes == 0) {
        ESP_LOGE(TAG, "encoder returned 0 bytes");
        return ESP_FAIL;
    }

    *out_ptr = s_out_buf;
    *out_size = out_bytes;
    ESP_LOGI(TAG, "captured %u bytes JPEG (copy %lld us, encode %lld us)",
             (unsigned)out_bytes, t_copy, t_enc);
    return ESP_OK;
}
