#pragma once

#include <stdbool.h>

#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Launches a background task that waits 10s, connects to hardcoded Wi-Fi
 * via the onboard ESP32-C6 (esp_hosted SDIO link), then syncs time from HTTP
 * Date headers. SNTP is intentionally avoided on this hosted-Wi-Fi stack.
 * Safe to call once from app_main after NVS is initialized. */
void eva_wifi_start(void);
bool eva_wifi_wait_connected(TickType_t timeout);
bool eva_wifi_is_connected(void);

/* Status reporter callback. Called from the wifi task whenever state changes.
 * String is short, statically allocated, safe to pass to lv_label_set_text.
 * Register before eva_wifi_start. NULL disables reporting. */
typedef void (*eva_wifi_status_cb_t)(const char *msg);
void eva_wifi_set_status_cb(eva_wifi_status_cb_t cb);

#ifdef __cplusplus
}
#endif
