#include "eva_ota.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "mdns.h"

#include "eva_clp_toc.h"
#include "eva_ota_state.h"
#include "eva_weather_canvas.h"
#include "eva_wifi.h"

static const char *TAG = "eva_ota";

#ifndef EVA_FW_VERSION
#define EVA_FW_VERSION "unknown"
#endif

#define OTA_PORT          8080
#define OTA_HOSTNAME      "eva-weather"
#define OTA_CHUNK         4096
/* Body size guard: the largest thing we ever accept is the clouds pack. */
#define OTA_MAX_BODY      (12u * 1024u * 1024u)

static httpd_handle_t s_server;
static eva_ota_status_t *s_status;
static volatile bool s_busy;          /* one upload at a time */
static bool s_mdns_up;

/* ---------------------------------------------------------------- helpers */

static void hex_of(const uint8_t digest[32], char out[65])
{
    static const char *hexd = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        out[i * 2]     = hexd[digest[i] >> 4];
        out[i * 2 + 1] = hexd[digest[i] & 0xf];
    }
    out[64] = '\0';
}

/* Case-insensitive, length-exact compare so a host sending uppercase hex is
 * still accepted. */
static bool hex_eq(const char *a, const char *b)
{
    if (!a || !b) return false;
    for (int i = 0; i < 64; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'F') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'F') cb = (char)(cb - 'A' + 'a');
        if (ca != cb || ca == '\0') return false;
    }
    return a[64] == '\0' && b[64] == '\0';
}

static bool take_busy(void)
{
    if (s_busy) return false;
    s_busy = true;
    return true;
}

static void release_busy(void)
{
    s_busy = false;
}

static void ui(const char *msg)
{
    if (s_status) eva_ota_status_set(s_status, msg);
}

static void ui_progress(const char *label, size_t done, size_t total)
{
    if (s_status) eva_ota_status_progress(s_status, label, done, total);
}

static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "rebooting to apply update");
    esp_restart();
}

/* Reboot after `ms`, giving the HTTP response time to reach the host. Calling
 * esp_restart() inline would drop the response and ota.py could not tell a
 * successful update from a crash. */
static void schedule_reboot(int ms)
{
    esp_timer_handle_t t;
    const esp_timer_create_args_t args = {
        .callback = reboot_cb,
        .name = "ota_reboot",
    };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)ms * 1000ull);
    } else {
        esp_restart();
    }
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t fail(httpd_req_t *req, httpd_err_code_t code, const char *msg)
{
    ESP_LOGW(TAG, "request failed: %s", msg);
    ui("Помилка оновлення");
    return httpd_resp_send_err(req, code, msg);
}

/* Read the expected digest the host says it is sending. Required: without it
 * we would have to trust the transfer, and a truncated body would become a
 * bootable image. */
static bool want_sha(httpd_req_t *req, char out[65])
{
    return httpd_req_get_hdr_value_str(req, "X-Eva-Sha256", out, 65) == ESP_OK &&
           strlen(out) == 64;
}

/* ------------------------------------------------------------ pack status */

/* Read the pack header straight from flash rather than through the mmap in
 * eva_cloud_assets.c: after a pack upload that mapping is stale until reboot,
 * and /ota/status must report what is actually stored. */
static void pack_info(char magic[8], uint16_t *entries, size_t *size)
{
    magic[0] = '\0';
    *entries = 0;
    *size = 0;

    const esp_partition_t *st = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "storage");
    if (!st) return;
    *size = st->size;

    uint8_t head[6];
    if (esp_partition_read(st, 0, head, sizeof head) != ESP_OK) return;
    if (memcmp(head, "CLP3", 4) == 0 || memcmp(head, "CLP2", 4) == 0) {
        memcpy(magic, head, 4);
        magic[4] = '\0';
        *entries = (uint16_t)(head[4] | (head[5] << 8));
    }
}

/* --------------------------------------------------------------- handlers */

static esp_err_t status_get(httpd_req_t *req)
{
    char app_sha[65] = "", pack_sha[65] = "";
    eva_ota_state_get_app_sha(app_sha);
    eva_ota_state_get_pack_sha(pack_sha);

    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);

    const char *state_str = "unknown";
    switch (st) {
    case ESP_OTA_IMG_NEW:            state_str = "new";            break;
    case ESP_OTA_IMG_PENDING_VERIFY: state_str = "pending_verify"; break;
    case ESP_OTA_IMG_VALID:          state_str = "valid";          break;
    case ESP_OTA_IMG_INVALID:        state_str = "invalid";        break;
    case ESP_OTA_IMG_ABORTED:        state_str = "aborted";        break;
    default: break;
    }

    char magic[8];
    uint16_t entries;
    size_t psize;
    pack_info(magic, &entries, &psize);

    char buf[640];
    snprintf(buf, sizeof buf,
             "{\"fw_version\":\"%s\","
             "\"app_sha256\":\"%s\","
             "\"app_running\":\"%s\","
             "\"app_state\":\"%s\","
             "\"pack_sha256\":\"%s\","
             "\"pack_magic\":\"%s\","
             "\"pack_entries\":%u,"
             "\"pack_part_size\":%u,"
             "\"free_heap\":%u,"
             "\"uptime_s\":%llu,"
             "\"ota_busy\":%s}",
             EVA_FW_VERSION, app_sha,
             run ? run->label : "?", state_str,
             pack_sha, magic, (unsigned)entries, (unsigned)psize,
             (unsigned)esp_get_free_heap_size(),
             (unsigned long long)(esp_timer_get_time() / 1000000),
             s_busy ? "true" : "false");
    return send_json(req, buf);
}

static esp_err_t ping_get(httpd_req_t *req)
{
    char buf[160];
    snprintf(buf, sizeof buf, "{\"ok\":true,\"fw_version\":\"%s\"}", EVA_FW_VERSION);
    return send_json(req, buf);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    esp_err_t err = send_json(req, "{\"ok\":true,\"reboot_in_ms\":800}");
    schedule_reboot(800);
    return err;
}

static esp_err_t app_post(httpd_req_t *req)
{
    char want[65];
    if (!want_sha(req, want)) {
        return fail(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Eva-Sha256");
    }
    if (req->content_len <= 0 || (size_t)req->content_len > OTA_MAX_BODY) {
        return fail(req, HTTPD_400_BAD_REQUEST, "bad Content-Length");
    }
    /* esp_http_server has no 409 in httpd_err_code_t; 500 with a distinct
     * message is enough for ota.py to tell the cases apart. */
    if (!take_busy()) {
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "another update is in progress");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        release_busy();
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    }
    if ((size_t)req->content_len > target->size) {
        release_busy();
        return fail(req, HTTPD_413_CONTENT_TOO_LARGE, "image larger than OTA slot");
    }

    ESP_LOGI(TAG, "app update -> %s (%d bytes)", target->label, req->content_len);
    if (s_status) eva_ota_status_reset_throttle(s_status);
    ui_progress("Оновлення прошивки", 0, (size_t)req->content_len);

    esp_ota_handle_t h = 0;
    if (esp_ota_begin(target, (size_t)req->content_len, &h) != ESP_OK) {
        release_busy();
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "esp_ota_begin failed");
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    static uint8_t buf[OTA_CHUNK];
    int remaining = req->content_len;
    bool ok = true;

    while (remaining > 0) {
        int want_n = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int r = httpd_req_recv(req, (char *)buf, want_n);
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed (%d) with %d left", r, remaining);
            ok = false;
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)r);
        if (esp_ota_write(h, buf, (size_t)r) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed");
            ok = false;
            break;
        }
        remaining -= r;
        ui_progress("Оновлення прошивки",
                    (size_t)(req->content_len - remaining), (size_t)req->content_len);
        /* Core 0 also runs cloud_bake and lightning; never hog it. */
        taskYIELD();
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (!ok) {
        esp_ota_abort(h);
        release_busy();
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer failed");
    }

    char got[65];
    hex_of(digest, got);
    if (!hex_eq(got, want)) {
        /* Verify BEFORE committing the boot pointer: a corrupted image must
         * never become bootable. */
        ESP_LOGE(TAG, "sha mismatch: got %s want %s", got, want);
        esp_ota_abort(h);
        release_busy();
        return fail(req, HTTPD_400_BAD_REQUEST, "sha256 mismatch");
    }

    ui("Перевірка…");
    if (esp_ota_end(h) != ESP_OK) {       /* also validates the image header */
        release_busy();
        return fail(req, HTTPD_400_BAD_REQUEST, "image validation failed");
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        release_busy();
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "set_boot_partition failed");
    }

    eva_ota_state_set_app_sha(want);
    eva_ota_state_set_app_part(target->label);

    ESP_LOGI(TAG, "app update committed to %s", target->label);
    ui("Перезавантаження…");
    esp_err_t err = send_json(req, "{\"ok\":true,\"reboot_in_ms\":1500}");
    release_busy();
    schedule_reboot(1500);
    return err;
}

/* The clouds pack cannot be A/B staged — two 7.6 MB copies do not fit in what
 * is left after the app slots — so it is written in place. That is safe only
 * because the canvas has a real "no pack" mode: suspending the readers makes it
 * bake clouds procedurally, and eva_clp_parse() bounds-checks every TOC entry,
 * so even a half-written pack is rejected at the next boot rather than read out
 * of bounds. Power loss mid-write therefore degrades to procedural clouds, and
 * the cleared hash makes the next ota.py run re-send automatically. */
static esp_err_t pack_post(httpd_req_t *req)
{
    char want[65];
    if (!want_sha(req, want)) {
        return fail(req, HTTPD_400_BAD_REQUEST, "missing or malformed X-Eva-Sha256");
    }
    if (req->content_len <= 0 || (size_t)req->content_len > OTA_MAX_BODY) {
        return fail(req, HTTPD_400_BAD_REQUEST, "bad Content-Length");
    }
    if (!take_busy()) {
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "another update is in progress");
    }

    const esp_partition_t *st = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, 0x40, "storage");
    if (!st) {
        release_busy();
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no storage partition");
    }
    if ((size_t)req->content_len > st->size) {
        release_busy();
        return fail(req, HTTPD_413_CONTENT_TOO_LARGE, "pack larger than storage partition");
    }

    ESP_LOGW(TAG, "pack update (%d bytes) — suspending cloud pack readers",
             req->content_len);

    /* From here on the old pack is gone. The hash is cleared FIRST so that a
     * crash or power cut at any point below leaves the device asking for the
     * pack again rather than believing it still has a good one. */
    eva_ota_state_clear_pack_sha();
    eva_weather_canvas_suspend_cloud_assets();

    if (s_status) eva_ota_status_reset_throttle(s_status);
    ui_progress("Оновлення хмар", 0, (size_t)req->content_len);

    /* esp_partition_erase_range demands 4096-alignment on offset AND size.
     * Only the region the pack actually occupies is erased — wiping all
     * 10.875 MB would take many seconds for no benefit, since nothing reads
     * past the TOC-declared extents. */
    size_t erase_bytes = ((size_t)req->content_len + 4095u) & ~(size_t)4095u;
    if (erase_bytes > st->size) erase_bytes = st->size;
    esp_err_t eerr = esp_partition_erase_range(st, 0, erase_bytes);
    if (eerr != ESP_OK) {
        release_busy();
        ESP_LOGE(TAG, "erase failed: %s", esp_err_to_name(eerr));
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "flash erase failed");
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    static uint8_t buf[OTA_CHUNK];
    int remaining = req->content_len;
    size_t off = 0;
    bool ok = true;

    while (remaining > 0) {
        int want_n = remaining < OTA_CHUNK ? remaining : OTA_CHUNK;
        int r = httpd_req_recv(req, (char *)buf, want_n);
        if (r <= 0) {
            ESP_LOGE(TAG, "recv failed (%d) with %d left", r, remaining);
            ok = false;
            break;
        }
        mbedtls_sha256_update(&sha, buf, (size_t)r);
        if (esp_partition_write(st, off, buf, (size_t)r) != ESP_OK) {
            ESP_LOGE(TAG, "partition write failed at %u", (unsigned)off);
            ok = false;
            break;
        }
        off += (size_t)r;
        remaining -= r;
        ui_progress("Оновлення хмар", off, (size_t)req->content_len);
        taskYIELD();
    }

    uint8_t digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    if (!ok) {
        release_busy();
        /* Reboot anyway: the pack is junk, but a reboot returns the device to a
         * consistent state (procedural clouds) instead of running on a
         * suspended mapping. */
        schedule_reboot(1500);
        return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "transfer failed; rebooting");
    }

    char got[65];
    hex_of(digest, got);
    if (!hex_eq(got, want)) {
        ESP_LOGE(TAG, "pack sha mismatch: got %s want %s", got, want);
        release_busy();
        schedule_reboot(1500);
        return fail(req, HTTPD_400_BAD_REQUEST, "sha256 mismatch; rebooting");
    }

    ui("Перевірка…");

    /* Independent structural check: the bytes hashed correctly, but confirm the
     * device can actually parse what it just stored before recording success. */
    uint8_t head[6];
    if (esp_partition_read(st, 0, head, sizeof head) != ESP_OK ||
        (memcmp(head, "CLP3", 4) != 0 && memcmp(head, "CLP2", 4) != 0)) {
        release_busy();
        schedule_reboot(1500);
        return fail(req, HTTPD_400_BAD_REQUEST, "not a CLP2/CLP3 pack; rebooting");
    }
    uint16_t entries = (uint16_t)(head[4] | (head[5] << 8));
    if (entries == 0 || entries > EVA_CLP_MAX_ENTRIES) {
        release_busy();
        schedule_reboot(1500);
        return fail(req, HTTPD_400_BAD_REQUEST, "bad TOC entry count; rebooting");
    }

    /* Verify every TOC entry fits inside the bytes we actually received.
     *
     * eva_clp_parse() at boot can only bound entries against the PARTITION size
     * (10.875 MB), because that is all a mapped partition knows. A truncated
     * pack whose TOC survived therefore passes that check and only fails later,
     * per-entry, deep in the LZ4 decode. Here the true payload length is known,
     * so a short pack is caught before it is ever recorded as good. */
    size_t toc_end = 6 + (size_t)entries * 12;
    size_t payload = (size_t)req->content_len;
    if (toc_end > payload) {
        release_busy();
        eva_ota_state_clear_pack_sha();
        schedule_reboot(1500);
        return fail(req, HTTPD_400_BAD_REQUEST, "TOC truncated; rebooting");
    }
    for (uint16_t i = 0; i < entries; ++i) {
        uint8_t rec[12];
        if (esp_partition_read(st, 6 + (size_t)i * 12, rec, sizeof rec) != ESP_OK) {
            release_busy();
            eva_ota_state_clear_pack_sha();
            schedule_reboot(1500);
            return fail(req, HTTPD_500_INTERNAL_SERVER_ERROR, "TOC read failed; rebooting");
        }
        uint32_t eoff = (uint32_t)rec[4] | ((uint32_t)rec[5] << 8) |
                        ((uint32_t)rec[6] << 16) | ((uint32_t)rec[7] << 24);
        uint32_t esz  = (uint32_t)rec[8] | ((uint32_t)rec[9] << 8) |
                        ((uint32_t)rec[10] << 16) | ((uint32_t)rec[11] << 24);
        if (eoff < toc_end || esz == 0 || (size_t)eoff + esz > payload) {
            ESP_LOGE(TAG, "entry %u out of bounds: off=%u size=%u payload=%u",
                     (unsigned)i, (unsigned)eoff, (unsigned)esz, (unsigned)payload);
            release_busy();
            eva_ota_state_clear_pack_sha();
            schedule_reboot(1500);
            return fail(req, HTTPD_400_BAD_REQUEST, "pack entry out of bounds; rebooting");
        }
    }

    eva_ota_state_set_pack_sha(want);
    ESP_LOGI(TAG, "pack update committed (%u entries)", (unsigned)entries);

    ui("Перезавантаження…");
    esp_err_t err = send_json(req, "{\"ok\":true,\"staged\":true,\"reboot_in_ms\":1500}");
    release_busy();
    schedule_reboot(1500);   /* reboot re-mmaps the new pack */
    return err;
}

/* ------------------------------------------------------------------ setup */

static const httpd_uri_t k_uris[] = {
    { .uri = "/ota/status", .method = HTTP_GET,  .handler = status_get },
    { .uri = "/ota/ping",   .method = HTTP_GET,  .handler = ping_get   },
    { .uri = "/ota/app",    .method = HTTP_POST, .handler = app_post   },
    { .uri = "/ota/pack",   .method = HTTP_POST, .handler = pack_post  },
    { .uri = "/ota/reboot", .method = HTTP_POST, .handler = reboot_post },
};

bool eva_ota_server_start(void)
{
    if (s_server) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port       = OTA_PORT;
    cfg.ctrl_port         = 32768;
    /* Priority 2 and core 0 are the FPS strategy: the render task is priority 5
     * pinned to core 1, so an upload never competes with it, and on core 0 this
     * stays below cloud_bake (3) and lightning (4). */
    cfg.task_priority     = 2;
    cfg.core_id           = 0;
    cfg.stack_size        = 8192;
    cfg.max_uri_handlers  = 8;
    cfg.max_open_sockets  = 3;
    cfg.lru_purge_enable  = true;
    cfg.recv_wait_timeout = 20;
    cfg.send_wait_timeout = 20;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed");
        s_server = NULL;
        return false;
    }
    for (size_t i = 0; i < sizeof k_uris / sizeof k_uris[0]; ++i) {
        httpd_register_uri_handler(s_server, &k_uris[i]);
    }
    ESP_LOGI(TAG, "OTA server on port %d", OTA_PORT);
    return true;
}

void eva_ota_server_stop(void)
{
    if (!s_server) return;
    httpd_stop(s_server);
    s_server = NULL;
    ESP_LOGI(TAG, "OTA server stopped");
}

bool eva_ota_server_running(void)
{
    return s_server != NULL;
}

static void mdns_bring_up(void)
{
    if (s_mdns_up) return;
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed — use the IP directly");
        return;
    }
    mdns_hostname_set(OTA_HOSTNAME);
    mdns_instance_name_set("Eva Weather Panel");
    /* The service lets ota.py find the panel by browsing even if the .local
     * name is contested on the network. */
    mdns_service_add(NULL, "_eva-ota", "_tcp", OTA_PORT, NULL, 0);
    mdns_service_txt_item_set("_eva-ota", "_tcp", "ver", EVA_FW_VERSION);
    s_mdns_up = true;
    ESP_LOGI(TAG, "mDNS up: %s.local", OTA_HOSTNAME);
}

static void ota_task(void *arg)
{
    (void)arg;
    /* mDNS and httpd both need the netif up, and mdns_init does network I/O —
     * hence a task rather than piggy-backing on the IP event handler.
     *
     * Poll in a loop rather than one long wait: eva_wifi_start() creates its
     * event group inside its own task, so for the first moments after boot
     * eva_wifi_wait_connected() sees a NULL group and just sleeps out the whole
     * timeout without ever looking again. A single portMAX_DELAY call there
     * sleeps forever and the OTA server never starts. */
    while (!eva_wifi_is_connected()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "wifi up — starting OTA services");
    mdns_bring_up();
    eva_ota_server_start();

    esp_netif_ip_info_t ip;
    if (eva_wifi_get_ip(&ip)) {
        ESP_LOGI(TAG, "OTA ready at http://" IPSTR ":%d/ota/status  (%s.local)",
                 IP2STR(&ip.ip), OTA_PORT, OTA_HOSTNAME);
    }
    vTaskDelete(NULL);
}

void eva_ota_start(eva_ota_status_t *status)
{
    s_status = status;
    xTaskCreatePinnedToCore(ota_task, "eva_ota", 4096, NULL, 2, NULL, 0);
}

const char *eva_ota_version(void)
{
    return EVA_FW_VERSION;
}

void eva_ota_info(char *buf, size_t buf_len)
{
    if (!buf || buf_len == 0) return;

    char app_sha[65] = "", pack_sha[65] = "", part[24] = "";
    eva_ota_state_get_app_sha(app_sha);
    eva_ota_state_get_pack_sha(pack_sha);
    eva_ota_state_get_app_part(part, sizeof part);

    const esp_partition_t *run = esp_ota_get_running_partition();

    char magic[8];
    uint16_t entries;
    size_t psize;
    pack_info(magic, &entries, &psize);

    char ipstr[20] = "none";
    esp_netif_ip_info_t ip;
    if (eva_wifi_get_ip(&ip)) {
        snprintf(ipstr, sizeof ipstr, IPSTR, IP2STR(&ip.ip));
    }

    snprintf(buf, buf_len,
             "ota:\r\n"
             "  version   %s\r\n"
             "  running   %s (recorded %s)\r\n"
             "  app sha   %s\r\n"
             "  pack sha  %s\r\n"
             "  pack      %s entries=%u part=%uK\r\n"
             "  ip        %s   http://%s.local:%d/ota/status\r\n"
             "  server    %s   mdns %s   busy %s\r\n",
             EVA_FW_VERSION,
             run ? run->label : "?", part[0] ? part : "-",
             app_sha[0] ? app_sha : "(none)",
             pack_sha[0] ? pack_sha : "(none)",
             magic[0] ? magic : "(none)", (unsigned)entries, (unsigned)(psize / 1024),
             ipstr, OTA_HOSTNAME, OTA_PORT,
             s_server ? "on" : "off", s_mdns_up ? "on" : "off",
             s_busy ? "yes" : "no");
}
