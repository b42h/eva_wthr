/* main/weather_fetch.c
 *
 * Owns two FreeRTOS timers. On fire, dispatches the relevant provider on a
 * worker task (HTTP must not run from timer context). On success, merges
 * the partial state into the canonical weather_state_t and applies it via
 * eva_weather_set(). On failure, schedules a FIB_8 minute retry; after
 * 3 retries returns to the regular cadence.
 *
 * Merge rules: see spec, section "Merge rules". Per-field source priority:
 *   weather_code, kind, temp, wind, precip, sunshine
 *     → open-meteo always
 *   cloud_cover_pct, cloud_low/mid/high_pct, sun, moon, visibility, fog
 *     → clearoutside if fresh (last success < 2*cadence), else open-meteo
 *       or hardcoded fallback
 *
 * User decision 2026-05-25: clearoutside owns ALL cloud fields because its
 * layer breakdown + "Total Clouds (% Sky Obscured)" metric match what we
 * render. open-meteo's cloud_cover is only used as fallback when clearoutside
 * is stale.
 */
#include "weather_fetch.h"
#include "weather_provider.h"
#include "weather_fetch_openmeteo.h"
#include "weather_fetch_clearoutside.h"
#include "eva_weather.h"
#include "eva_wifi.h"

#include <string.h>
#include <sys/time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "weather_fetch";

/* Fibonacci-aligned cadences (minutes). */
#define FIB_8   8
#define FIB_13  13
#define FIB_89  89

#define OPENMETEO_INTERVAL_MIN     FIB_13
#define CLEAROUTSIDE_INTERVAL_MIN  FIB_89
#define RETRY_BACKOFF_MIN          FIB_8
#define MAX_RETRIES                3
#define FIRST_FETCH_DELAY_MS       (15 * 1000)

/* "Fresh" window for secondary fields: 2 * cadence. */
#define CLEAROUTSIDE_FRESH_SEC     (2 * CLEAROUTSIDE_INTERVAL_MIN * 60)

typedef enum {
    JOB_OPENMETEO,
    JOB_CLEAROUTSIDE,
} job_kind_t;

static esp_timer_handle_t s_t_openmeteo;
static esp_timer_handle_t s_t_clearoutside;
static esp_timer_handle_t s_retry_t_openmeteo;
static esp_timer_handle_t s_retry_t_clearoutside;
static QueueHandle_t      s_job_queue;
static SemaphoreHandle_t  s_state_mutex;
static bool               s_wifi_ready_delay_done = false;

/* Per-source provenance */
static int64_t s_last_ok_openmeteo    = 0;
static int64_t s_last_ok_clearoutside = 0;
static int     s_retries_openmeteo    = 0;
static int     s_retries_clearoutside = 0;
static bool    s_retrying_openmeteo   = false;
static bool    s_retrying_clearoutside = false;

/* Last successful partial from each source. */
static weather_partial_t s_last_openmeteo;
static weather_partial_t s_last_clearoutside;
static bool s_have_openmeteo    = false;
static bool s_have_clearoutside = false;

static int64_t now_epoch_sec(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec;
}

static bool clearoutside_is_fresh(void)
{
    if (!s_have_clearoutside) return false;
    return (now_epoch_sec() - s_last_ok_clearoutside) < CLEAROUTSIDE_FRESH_SEC;
}

static void queue_job(job_kind_t kind)
{
    if (!s_job_queue) return;
    (void)xQueueSend(s_job_queue, &kind, 0);
}

static bool wait_for_wifi_ready(void)
{
    int waited_s = 0;
    while (!eva_wifi_is_connected()) {
        if ((waited_s % 10) == 0) {
            ESP_LOGW(TAG, "Wi-Fi not connected yet; weather fetch waits");
        }
        (void)eva_wifi_wait_connected(pdMS_TO_TICKS(1000));
        waited_s++;
    }
    if (!s_wifi_ready_delay_done) {
        s_wifi_ready_delay_done = true;
        ESP_LOGI(TAG, "Wi-Fi connected; first weather fetch in %d ms",
                 FIRST_FETCH_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(FIRST_FETCH_DELAY_MS));
    }
    return true;
}

/* Merge the two latest partials into the canonical weather_state_t and
 * apply it. Always called with s_state_mutex held. */
static void merge_and_apply(void)
{
    weather_state_t st = {0};
    /* Pull baseline from current canonical state so we keep fields neither
     * provider populates this cycle. */
    const weather_state_t *cur = eva_weather_get();
    if (cur) memcpy(&st, cur, sizeof(st));

    const weather_partial_t *om = s_have_openmeteo ? &s_last_openmeteo : NULL;
    const weather_partial_t *co = s_have_clearoutside ? &s_last_clearoutside : NULL;
    bool co_fresh = clearoutside_is_fresh();

    /* --- open-meteo wins for non-cloud lifestyle fields --- */
    if (om) {
        if (om->has_kind) {
            st.kind = om->kind;
            st.weather_code = om->weather_code;
        }
        if (om->has_temp) {
            st.temp_c       = (int8_t)om->temp_c;
            st.feels_like_c = (int8_t)om->feels_like_c;
        }
        if (om->has_wind) {
            st.wind_kph     = (int16_t)om->wind_kph;
            st.wind_dir_deg = (int16_t)om->wind_dir_deg;
        }
        if (om->has_precip) {
            st.precip_type   = om->precip;
            st.precip_mm_x10 = om->precip_mm_x10;
        }
        if (om->has_sunshine) {
            st.sunshine_minutes = om->sunshine_minutes;
        }
    }

    /* --- ALL clouds: clearoutside is primary (per-layer + total cover) --- */
    /* User decision 2026-05-25: clearoutside owns every cloud field because
     * its per-layer breakdown and the "Total Clouds (% Sky Obscured)" metric
     * match what we render. open-meteo's cloud_cover is fallback when
     * clearoutside is stale or has never succeeded. */
    if (co && co_fresh && co->has_clouds) {
        st.cloud_low_pct   = co->cloud_low_pct;
        st.cloud_mid_pct   = co->cloud_mid_pct;
        st.cloud_high_pct  = co->cloud_high_pct;
        st.cloud_total_pct = co->cloud_cover_pct;   /* clearoutside "Total Clouds" */
        st.cloud_cover_pct = co->cloud_cover_pct;   /* drives sky_cover_fraction() */
    } else if (om && om->has_clouds) {
        st.cloud_low_pct   = om->cloud_low_pct;
        st.cloud_mid_pct   = om->cloud_mid_pct;
        st.cloud_high_pct  = om->cloud_high_pct;
        st.cloud_total_pct = om->cloud_cover_pct;
        st.cloud_cover_pct = om->cloud_cover_pct;   /* open-meteo fallback */
    }

    /* Sun: clearoutside wins when fresh, else open-meteo */
    if (co && co_fresh && co->has_sun) {
        st.sunrise_min = co->sunrise_min;
        st.sunset_min  = co->sunset_min;
    } else if (om && om->has_sun) {
        st.sunrise_min = om->sunrise_min;
        st.sunset_min  = om->sunset_min;
    }

    /* Moon: clearoutside only. If stale, keep whatever was in canonical state.
     * If never populated, hardcoded fallback (50%, waxing). */
    if (co && co_fresh && co->has_moon) {
        st.moonrise_min   = co->moonrise_min;
        st.moonset_min    = co->moonset_min;
        st.moon_phase_pct = co->moon_phase_pct;
        st.moon_waning    = co->moon_waning;
    } else if (st.moonrise_min <= 0) {
        /* Never had moon data — use generic fallback */
        st.moon_phase_pct = 50;
        st.moon_waning    = 0;
    }

    /* Visibility / fog: clearoutside wins when fresh */
    if (co && co_fresh && co->has_visibility) {
        st.visibility_km_x10 = (uint8_t)(co->visibility_km_x10 > 255 ? 255 : co->visibility_km_x10);
        st.fog_pct           = co->fog_pct;
    }

    /* Provenance timestamps */
    st.openmeteo_ts    = s_last_ok_openmeteo;
    st.clearoutside_ts = s_last_ok_clearoutside;
    st.fetched_at      = (time_t)now_epoch_sec();

    /* Ukrainian description from kind */
    snprintf(st.desc, sizeof(st.desc), "%s", weather_kind_label_uk(st.kind));

    eva_weather_set(&st);
}

/* Run one provider call; on success update s_last_*. On failure bump retry
 * counter and rearm the timer for backoff. */
static void run_job(job_kind_t kind)
{
    weather_partial_t p;
    esp_err_t err;

    if (kind == JOB_OPENMETEO) {
        ESP_LOGI(TAG, "open-meteo fetch start");
        err = weather_provider_openmeteo_fetch(&p);
    } else {
        ESP_LOGI(TAG, "clearoutside fetch start");
        err = weather_provider_clearoutside_fetch(&p);
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (err == ESP_OK) {
        if (kind == JOB_OPENMETEO) {
            memcpy(&s_last_openmeteo, &p, sizeof(p));
            s_have_openmeteo        = true;
            s_last_ok_openmeteo     = now_epoch_sec();
            s_retries_openmeteo     = 0;
            s_retrying_openmeteo    = false;
            esp_timer_stop(s_retry_t_openmeteo);
            ESP_LOGI(TAG, "open-meteo ok code=%d cover=%u%% temp=%dC",
                     p.weather_code, p.cloud_cover_pct, (int)p.temp_c);
        } else {
            memcpy(&s_last_clearoutside, &p, sizeof(p));
            s_have_clearoutside        = true;
            s_last_ok_clearoutside     = now_epoch_sec();
            s_retries_clearoutside     = 0;
            s_retrying_clearoutside    = false;
            esp_timer_stop(s_retry_t_clearoutside);
            ESP_LOGI(TAG, "clearoutside ok L=%u%% M=%u%% H=%u%% moon=%u%%",
                     p.cloud_low_pct, p.cloud_mid_pct, p.cloud_high_pct,
                     p.moon_phase_pct);
        }
        merge_and_apply();
    } else {
        int            *retries  = (kind == JOB_OPENMETEO) ? &s_retries_openmeteo   : &s_retries_clearoutside;
        bool           *retrying = (kind == JOB_OPENMETEO) ? &s_retrying_openmeteo  : &s_retrying_clearoutside;
        esp_timer_handle_t t     = (kind == JOB_OPENMETEO) ? s_retry_t_openmeteo     : s_retry_t_clearoutside;
        const char     *name     = (kind == JOB_OPENMETEO) ? "open-meteo"            : "clearoutside";

        (*retries)++;
        if (*retries <= MAX_RETRIES) {
            *retrying = true;
            ESP_LOGW(TAG, "%s failed err=%s, retry %d/%d in %d min",
                     name, esp_err_to_name(err), *retries, MAX_RETRIES, RETRY_BACKOFF_MIN);
            esp_timer_stop(t);
            esp_timer_start_once(t, (uint64_t)RETRY_BACKOFF_MIN * 60ULL * 1000000ULL);
        } else {
            *retrying = false;
            *retries  = 0;
            ESP_LOGE(TAG, "%s gave up after %d retries, back to normal cadence",
                     name, MAX_RETRIES);
        }
    }
    xSemaphoreGive(s_state_mutex);
}

static void worker_task(void *arg)
{
    (void)arg;
    job_kind_t kind;
    for (;;) {
        if (xQueueReceive(s_job_queue, &kind, portMAX_DELAY) == pdTRUE) {
            wait_for_wifi_ready();
            run_job(kind);
        }
    }
}

static void timer_cb_openmeteo(void *arg)
{
    (void)arg;
    queue_job(JOB_OPENMETEO);
}

static void timer_cb_clearoutside(void *arg)
{
    (void)arg;
    queue_job(JOB_CLEAROUTSIDE);
}

void weather_fetch_start(void)
{
    if (s_job_queue) return;  /* idempotent */
    s_state_mutex = xSemaphoreCreateMutex();
    s_job_queue   = xQueueCreate(4, sizeof(job_kind_t));
    xTaskCreate(worker_task, "wx_worker", 8192, NULL, 5, NULL);

    esp_timer_create_args_t om_args = {
        .callback = timer_cb_openmeteo,
        .name     = "wx_openmeteo",
    };
    esp_timer_create_args_t co_args = {
        .callback = timer_cb_clearoutside,
        .name     = "wx_clearoutside",
    };
    esp_timer_create_args_t om_retry_args = {
        .callback = timer_cb_openmeteo,
        .name     = "wx_om_retry",
    };
    esp_timer_create_args_t co_retry_args = {
        .callback = timer_cb_clearoutside,
        .name     = "wx_co_retry",
    };
    esp_timer_create(&om_args, &s_t_openmeteo);
    esp_timer_create(&co_args, &s_t_clearoutside);
    esp_timer_create(&om_retry_args, &s_retry_t_openmeteo);
    esp_timer_create(&co_retry_args, &s_retry_t_clearoutside);

    /* Periodic timers for regular cadence. */
    esp_timer_start_periodic(s_t_openmeteo,
        (uint64_t)OPENMETEO_INTERVAL_MIN * 60ULL * 1000000ULL);
    esp_timer_start_periodic(s_t_clearoutside,
        (uint64_t)CLEAROUTSIDE_INTERVAL_MIN * 60ULL * 1000000ULL);

    /* Kick both immediately at boot. */
    queue_job(JOB_OPENMETEO);
    queue_job(JOB_CLEAROUTSIDE);

    ESP_LOGI(TAG, "started: open-meteo every %d min, clearoutside every %d min",
             OPENMETEO_INTERVAL_MIN, CLEAROUTSIDE_INTERVAL_MIN);
}

void weather_fetch_request(void)
{
    if (!s_job_queue) return;
    queue_job(JOB_OPENMETEO);
    queue_job(JOB_CLEAROUTSIDE);
}

int64_t weather_fetch_openmeteo_last_ts(void)    { return s_last_ok_openmeteo; }
int64_t weather_fetch_clearoutside_last_ts(void) { return s_last_ok_clearoutside; }
bool    weather_fetch_openmeteo_retrying(void)    { return s_retrying_openmeteo; }
bool    weather_fetch_clearoutside_retrying(void) { return s_retrying_clearoutside; }
