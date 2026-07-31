#include "eva_ota_state.h"

#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "nvs.h"

static const char *TAG = "eva_ota_state";

#define KEY_APP_SHA   "ota_app_sha"
#define KEY_PACK_SHA  "ota_pack_sha"
#define KEY_APP_PART  "ota_app_part"

static bool get_str(const char *key, char *out, size_t cap)
{
    if (!out || cap == 0) return false;
    out[0] = '\0';

    nvs_handle_t h;
    if (nvs_open("eva", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t sz = cap;
    esp_err_t err = nvs_get_str(h, key, out, &sz);
    nvs_close(h);
    if (err != ESP_OK) {
        out[0] = '\0';
        return false;
    }
    return out[0] != '\0';
}

static void set_str(const char *key, const char *value)
{
    nvs_handle_t h;
    if (nvs_open("eva", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed writing %s", key);
        return;
    }
    if (value && value[0]) {
        (void)nvs_set_str(h, key, value);
    } else {
        (void)nvs_erase_key(h, key);
    }
    (void)nvs_commit(h);
    nvs_close(h);
}

bool eva_ota_state_get_app_sha(char *out)
{
    return get_str(KEY_APP_SHA, out, EVA_OTA_SHA_HEX_LEN + 1);
}

bool eva_ota_state_get_pack_sha(char *out)
{
    return get_str(KEY_PACK_SHA, out, EVA_OTA_SHA_HEX_LEN + 1);
}

void eva_ota_state_set_app_sha(const char *hex)
{
    set_str(KEY_APP_SHA, hex);
}

void eva_ota_state_set_pack_sha(const char *hex)
{
    set_str(KEY_PACK_SHA, hex);
}

void eva_ota_state_clear_app_sha(void)
{
    set_str(KEY_APP_SHA, NULL);
}

void eva_ota_state_clear_pack_sha(void)
{
    set_str(KEY_PACK_SHA, NULL);
}

bool eva_ota_state_get_app_part(char *out, int cap)
{
    return get_str(KEY_APP_PART, out, (size_t)cap);
}

void eva_ota_state_set_app_part(const char *label)
{
    set_str(KEY_APP_PART, label);
}

bool eva_ota_state_check_rollback(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) return false;

    char recorded[24];
    if (!eva_ota_state_get_app_part(recorded, sizeof recorded)) {
        /* Nothing recorded yet (first boot after the cable flash). Adopt the
         * running slot so the next real OTA has something to compare against.
         * The app hash stays empty, so ota.py sends the app once. */
        eva_ota_state_set_app_part(run->label);
        return false;
    }

    if (strcmp(recorded, run->label) == 0) {
        return false;
    }

    /* The bootloader picked a different slot than the one we last wrote — the
     * new image failed its self-check and was rolled back. The recorded app
     * hash describes an image that is NOT running, so drop it and let ota.py
     * re-push. */
    ESP_LOGW(TAG, "rollback detected: recorded=%s running=%s — clearing app hash",
             recorded, run->label);
    eva_ota_state_clear_app_sha();
    eva_ota_state_set_app_part(run->label);
    return true;
}
