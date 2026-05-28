#include "eva_wifi.h"

#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#define WIFI_SSID       CONFIG_EVA_WIFI_SSID
#define WIFI_PASSWORD   CONFIG_EVA_WIFI_PASSWORD
#define WIFI_START_DELAY_MS  10000
#define WIFI_MAX_RETRY  10

static const char *TAG = "eva_wifi";

#define BIT_CONNECTED  BIT0
#define BIT_FAIL       BIT1

static EventGroupHandle_t s_evt;
static int s_retry = 0;
static eva_wifi_status_cb_t s_status_cb = NULL;
static char s_status_buf[64];

static void report(const char *msg)
{
    if (s_status_cb) {
        s_status_cb(msg);
    }
}

void eva_wifi_set_status_cb(eva_wifi_status_cb_t cb)
{
    s_status_cb = cb;
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "STA started, connecting…");
        report("Wi-Fi: connecting…");
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_evt) {
            xEventGroupClearBits(s_evt, BIT_CONNECTED);
        }
        if (s_retry < WIFI_MAX_RETRY) {
            s_retry++;
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_retry, WIFI_MAX_RETRY);
            snprintf(s_status_buf, sizeof(s_status_buf),
                     "Wi-Fi: retry %d/%d", s_retry, WIFI_MAX_RETRY);
            report(s_status_buf);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up after %d retries", WIFI_MAX_RETRY);
            report("Wi-Fi: failed");
            xEventGroupSetBits(s_evt, BIT_FAIL);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "GOT IP " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_status_buf, sizeof(s_status_buf), "IP: " IPSTR, IP2STR(&e->ip_info.ip));
        report(s_status_buf);
        s_retry = 0;
        xEventGroupSetBits(s_evt, BIT_CONNECTED);
    }
}

static esp_err_t wifi_bring_up(void)
{
    /* 1:1 with esp_brookesia_phone Setting.cpp:507-523 (vendor recipe). */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    assert(sta);

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event, NULL, NULL));

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.sta.ssid,     WIFI_SSID,     sizeof(wc.sta.ssid));
    strncpy((char *)wc.sta.password, WIFI_PASSWORD, sizeof(wc.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_connect();

    return ESP_OK;
}

/* HTTP-based time sync as a workaround for SNTP-on-hosted-wifi crashes.
 *
 * On this board's hosted Wi-Fi (esp_hosted 2.8.0 host + 2.12.0 C6 slave) the
 * lwIP SNTP path (UDP/123 through esp_wifi_remote) deterministically resets
 * the chip ~3s after esp_sntp_init(). The HTTP/TCP path through the same
 * stack is fine — proven by weather_fetch hitting clearoutside successfully.
 * So we read the time from a small HTTP endpoint and feed settimeofday()
 * ourselves. Accuracy is ~1s, plenty for a wall clock.
 *
 * Parse target: the response from worldtimeapi.org/api/ip contains
 *   "unixtime":1747680123,
 * which we grep without a full JSON parser. */

#include "esp_http_client.h"

/* Grab time from the HTTP Date: response header of any well-known server.
 * Works on any HTTP/1.1 endpoint, no parsing of JSON, no API quotas, no
 * single point of failure. Tried in order until one responds. */
static const char * const TIME_URLS[] = {
    "http://www.google.com/generate_204",
    "http://cloudflare.com/cdn-cgi/trace",
    "http://example.com/",
};
#define TIME_URLS_COUNT (sizeof(TIME_URLS) / sizeof(TIME_URLS[0]))

/* Parse RFC 1123 date like "Thu, 21 May 2026 14:30:25 GMT" via strptime. */
static bool parse_http_date(const char *hdr, time_t *out)
{
    struct tm tm = {0};
    char *end = strptime(hdr, "%a, %d %b %Y %H:%M:%S", &tm);
    if (!end) return false;
    /* timegm equivalent: setenv TZ=UTC, mktime, restore */
    char *prev_tz = getenv("TZ");
    char saved[64] = {0};
    if (prev_tz) snprintf(saved, sizeof(saved), "%s", prev_tz);
    setenv("TZ", "UTC0", 1); tzset();
    time_t t = mktime(&tm);
    if (saved[0]) setenv("TZ", saved, 1); else unsetenv("TZ");
    tzset();
    if (t == (time_t)-1) return false;
    *out = t;
    return true;
}

/* Capture Date: header via the event handler — esp_http_client_get_header()
 * doesn't expose response headers after a HEAD on this IDF version. */
static esp_err_t http_time_evt(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->user_data) {
        if (evt->header_key && strcasecmp(evt->header_key, "Date") == 0) {
            char *dst = (char *)evt->user_data;
            strncpy(dst, evt->header_value, 63);
            dst[63] = '\0';
        }
    }
    return ESP_OK;
}

static bool http_time_fetch_one(const char *url)
{
    char date_buf[64] = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_HEAD,
        .timeout_ms = 6000,
        .disable_auto_redirect = true,
        .event_handler = http_time_evt,
        .user_data = date_buf,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "time HEAD %s failed: %s", url, esp_err_to_name(err));
        return false;
    }
    if (date_buf[0] == '\0') {
        ESP_LOGW(TAG, "no Date header from %s", url);
        return false;
    }

    time_t ts = 0;
    if (!parse_http_date(date_buf, &ts) || ts < 1700000000LL || ts > 4000000000LL) {
        ESP_LOGW(TAG, "bad date '%s' from %s", date_buf, url);
        return false;
    }

    struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    struct tm tm_now;
    char buf[64];
    localtime_r(&ts, &tm_now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &tm_now);
    ESP_LOGI(TAG, "HTTP time sync: %s (via %s)", buf, url);
    return true;
}

static bool http_time_fetch(void)
{
    for (size_t i = 0; i < TIME_URLS_COUNT; i++) {
        if (http_time_fetch_one(TIME_URLS[i])) return true;
    }
    return false;
}

static void time_sync_task(void *arg)
{
    /* Wait for wifi/RPC stack to settle, then fetch time over HTTP.
     * Retry every 30s on failure, refresh every 6h once synced. */
    vTaskDelay(pdMS_TO_TICKS(5000));
    bool synced = false;
    while (1) {
        if (http_time_fetch()) {
            synced = true;
            vTaskDelay(pdMS_TO_TICKS(6 * 3600 * 1000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(synced ? 60000 : 30000));
        }
    }
}

static void wifi_task(void *arg)
{
    report("Wi-Fi: waiting…");
    ESP_LOGI(TAG, "delaying %d ms before Wi-Fi init", WIFI_START_DELAY_MS);
    vTaskDelay(pdMS_TO_TICKS(WIFI_START_DELAY_MS));

    s_evt = xEventGroupCreate();
    report("Wi-Fi: init…");

    if (wifi_bring_up() != ESP_OK) {
        ESP_LOGE(TAG, "wifi bring-up failed, UI continues without time sync");
        report("Wi-Fi: bring-up failed");
        vTaskDelete(NULL);
        return;
    }

    EventBits_t bits = xEventGroupWaitBits(
        s_evt, BIT_CONNECTED | BIT_FAIL, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    if (bits & BIT_CONNECTED) {
        /* Time sync is started from a separate, low-priority task to avoid
         * racing with the hosted Wi-Fi event/RPC stack. Earlier attempts to
         * use lwIP SNTP caused a board reset ~3s later, so this task uses
         * HTTP Date headers over the TCP path instead. */
        xTaskCreate(time_sync_task, "eva_time", 4096, NULL, 2, NULL);
    } else {
        ESP_LOGE(TAG, "no IP within 30s (bits=0x%x)", (unsigned)bits);
    }

    vTaskDelete(NULL);
}

void eva_wifi_start(void)
{
    xTaskCreate(wifi_task, "eva_wifi", 6144, NULL, 4, NULL);
}

bool eva_wifi_wait_connected(TickType_t timeout)
{
    if (!s_evt) {
        vTaskDelay(timeout);
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(
        s_evt, BIT_CONNECTED, pdFALSE, pdFALSE, timeout);
    return (bits & BIT_CONNECTED) != 0;
}

bool eva_wifi_is_connected(void)
{
    if (!s_evt) {
        return false;
    }
    return (xEventGroupGetBits(s_evt) & BIT_CONNECTED) != 0;
}
