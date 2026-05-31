/* main/weather_fetch_openmeteo.c
 *
 * Fetches current weather from api.open-meteo.com and parses the JSON
 * into weather_partial_t. Coordinates come from project config.
 */
#include "weather_fetch_openmeteo.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "eva_weather.h"
#include "sdkconfig.h"

static const char *TAG = "wx_openmeteo";

/* Query string is long; build it once at compile time. The list of `current`
 * variables determines what we get back — keep it in sync with the parser.
 * `weather_code` is the WMO code that drives weather_kind_t mapping.
 * `sunshine_duration` is in seconds per current day. */
#define OPENMETEO_URL \
    "https://api.open-meteo.com/v1/forecast" \
    "?latitude=" CONFIG_EVA_WEATHER_LATITUDE \
    "&longitude=" CONFIG_EVA_WEATHER_LONGITUDE \
    "&current=temperature_2m,apparent_temperature," \
              "precipitation,weather_code,cloud_cover," \
              "cloud_cover_low,cloud_cover_mid,cloud_cover_high," \
              "wind_speed_10m,wind_direction_10m,is_day" \
    "&daily=sunrise,sunset,sunshine_duration" \
    "&timezone=auto&forecast_days=1"

/* Open-meteo current response is ~3-5 KB. Allocate 8 KB to be safe. */
#define OPENMETEO_BUF_SIZE 8192

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} fetch_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    fetch_buf_t *fb = (fetch_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        size_t avail = fb->cap - fb->len - 1;
        size_t copy = (evt->data_len < (int)avail) ? (size_t)evt->data_len : avail;
        if (copy > 0) {
            memcpy(fb->buf + fb->len, evt->data, copy);
            fb->len += copy;
            fb->buf[fb->len] = '\0';
        }
    }
    return ESP_OK;
}

/* Map WMO weather code → weather_kind_t.
 * Returns true if a kind was matched; false → caller falls back to
 * cloud-cover/precip heuristic. is_day = 1 → day variant, 0 → night. */
static bool wmo_to_kind(int code, int is_day, weather_kind_t *kind_out)
{
    switch (code) {
        case 0:
            *kind_out = is_day ? WEATHER_CLEAR_DAY : WEATHER_CLEAR_NIGHT;
            return true;
        case 1: case 2:
            *kind_out = is_day ? WEATHER_PARTLY_CLOUDY_DAY
                               : WEATHER_PARTLY_CLOUDY_NIGHT;
            return true;
        case 3:
            *kind_out = WEATHER_CLOUDY;
            return true;
        case 45: case 48:
            *kind_out = WEATHER_FOG;
            return true;
        case 51: case 53: case 55:   /* drizzle */
        case 56: case 57:            /* freezing drizzle */
        case 61: case 63:            /* light/moderate rain */
        case 80: case 81:            /* rain showers */
            *kind_out = WEATHER_RAIN;
            return true;
        case 65:                     /* heavy rain */
        case 82:                     /* violent showers */
            *kind_out = WEATHER_HEAVY_RAIN;
            return true;
        case 66: case 67:            /* freezing rain */
            *kind_out = WEATHER_SLEET;
            return true;
        case 71: case 73: case 75:   /* snow */
        case 77:                     /* snow grains */
        case 85: case 86:            /* snow showers */
            *kind_out = WEATHER_SNOW;
            return true;
        case 95:                     /* thunderstorm */
            *kind_out = WEATHER_THUNDERSTORM;
            return true;
        case 96: case 99:            /* thunderstorm with hail */
            *kind_out = WEATHER_HAIL;
            return true;
        default:
            return false;
    }
}

/* Parse ISO-8601 "YYYY-MM-DDTHH:MM" → minutes-of-day local. Returns -1 on
 * malformed input. open-meteo returns naïve local time when timezone=auto. */
static int16_t iso_to_minutes(const char *iso)
{
    if (!iso || strlen(iso) < 16) return -1;
    int hh = (iso[11] - '0') * 10 + (iso[12] - '0');
    int mm = (iso[14] - '0') * 10 + (iso[15] - '0');
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return -1;
    return (int16_t)(hh * 60 + mm);
}

/* Map precip mm + WMO code → precip_type_t. We don't have a separate
 * precipitation_type from open-meteo, so infer from weather_code. */
static precip_type_t precip_type_from_code(int code, float precip_mm)
{
    if (precip_mm < 0.05f) return PRECIP_NONE;
    if (code >= 71 && code <= 77) return PRECIP_SNOW;
    if (code == 85 || code == 86) return PRECIP_SNOW;
    if (code == 66 || code == 67) return PRECIP_SLEET;
    if (code == 96 || code == 99) return PRECIP_HAIL;
    return PRECIP_RAIN;
}

static esp_err_t parse_openmeteo_json(const char *json, weather_partial_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse failed");
        return ESP_FAIL;
    }
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsObject(cur)) {
        ESP_LOGW(TAG, "no 'current' object");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    /* --- clouds (parsed before kind so kind can use real cover) --- */
    cJSON *jcc  = cJSON_GetObjectItem(cur, "cloud_cover");
    cJSON *jccl = cJSON_GetObjectItem(cur, "cloud_cover_low");
    cJSON *jccm = cJSON_GetObjectItem(cur, "cloud_cover_mid");
    cJSON *jcch = cJSON_GetObjectItem(cur, "cloud_cover_high");
    if (cJSON_IsNumber(jcc)) {
        out->has_clouds = true;
        out->cloud_cover_pct = (uint8_t)jcc->valuedouble;
        out->cloud_low_pct  = cJSON_IsNumber(jccl) ? (uint8_t)jccl->valuedouble : 0;
        out->cloud_mid_pct  = cJSON_IsNumber(jccm) ? (uint8_t)jccm->valuedouble : 0;
        out->cloud_high_pct = cJSON_IsNumber(jcch) ? (uint8_t)jcch->valuedouble : 0;
    }

    /* --- weather_code + kind --- */
    cJSON *jcode  = cJSON_GetObjectItem(cur, "weather_code");
    cJSON *jisday = cJSON_GetObjectItem(cur, "is_day");
    int code = cJSON_IsNumber(jcode) ? jcode->valueint : -1;
    int is_day = cJSON_IsNumber(jisday) ? jisday->valueint : 1;
    out->weather_code = code;
    weather_kind_t kind;
    if (code >= 0 && wmo_to_kind(code, is_day, &kind)) {
        /* WMO 3 ("overcast") is reported by Open-Meteo well before the sky is
         * actually solid. Only treat it as true overcast when cover is high;
         * otherwise keep a partly cloudy scene so sun/moon remains visible. */
        if (kind == WEATHER_CLOUDY && out->has_clouds &&
            out->cloud_cover_pct < 88) {
            kind = is_day ? WEATHER_PARTLY_CLOUDY_DAY
                          : WEATHER_PARTLY_CLOUDY_NIGHT;
        }
        out->has_kind = true;
        out->kind = kind;
    }

    /* --- temp --- */
    cJSON *jt  = cJSON_GetObjectItem(cur, "temperature_2m");
    cJSON *jat = cJSON_GetObjectItem(cur, "apparent_temperature");
    if (cJSON_IsNumber(jt)) {
        out->has_temp = true;
        out->temp_c = (int16_t)jt->valuedouble;
        out->feels_like_c = cJSON_IsNumber(jat)
            ? (int16_t)jat->valuedouble : out->temp_c;
    }

    /* --- wind --- */
    cJSON *jws = cJSON_GetObjectItem(cur, "wind_speed_10m");
    cJSON *jwd = cJSON_GetObjectItem(cur, "wind_direction_10m");
    if (cJSON_IsNumber(jws)) {
        out->has_wind = true;
        /* open-meteo default unit for wind_speed_10m is km/h. */
        out->wind_kph = (uint16_t)jws->valuedouble;
        out->wind_dir_deg = cJSON_IsNumber(jwd) ? (uint16_t)jwd->valuedouble : 0;
    }

    /* --- precip --- */
    cJSON *jp = cJSON_GetObjectItem(cur, "precipitation");
    if (cJSON_IsNumber(jp)) {
        out->has_precip = true;
        float mm = (float)jp->valuedouble;
        out->precip = precip_type_from_code(code, mm);
        out->precip_mm_x10 = (uint16_t)(mm * 10.0f);
    }

    /* --- sun (daily[0]) --- */
    if (cJSON_IsObject(daily)) {
        cJSON *sr_arr = cJSON_GetObjectItem(daily, "sunrise");
        cJSON *ss_arr = cJSON_GetObjectItem(daily, "sunset");
        cJSON *sd_arr = cJSON_GetObjectItem(daily, "sunshine_duration");
        if (cJSON_IsArray(sr_arr) && cJSON_IsArray(ss_arr)) {
            cJSON *sr0 = cJSON_GetArrayItem(sr_arr, 0);
            cJSON *ss0 = cJSON_GetArrayItem(ss_arr, 0);
            int16_t sr_min = cJSON_IsString(sr0) ? iso_to_minutes(sr0->valuestring) : -1;
            int16_t ss_min = cJSON_IsString(ss0) ? iso_to_minutes(ss0->valuestring) : -1;
            if (sr_min >= 0 && ss_min >= 0) {
                out->has_sun = true;
                out->sunrise_min = sr_min;
                out->sunset_min = ss_min;
            }
        }
        if (cJSON_IsArray(sd_arr)) {
            cJSON *sd0 = cJSON_GetArrayItem(sd_arr, 0);
            if (cJSON_IsNumber(sd0)) {
                out->has_sunshine = true;
                /* sunshine_duration is seconds for the day — convert to minutes. */
                double mins = sd0->valuedouble / 60.0;
                if (mins < 0) mins = 0;
                if (mins > 65535) mins = 65535;
                out->sunshine_minutes = (uint16_t)mins;
            }
        }
    }

    cJSON_Delete(root);
    return ESP_OK;
}

esp_err_t weather_provider_openmeteo_fetch(weather_partial_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->weather_code = -1;
    out->sunrise_min = -1;
    out->sunset_min = -1;
    out->moonrise_min = -1;
    out->moonset_min = -1;

    fetch_buf_t fb = { .buf = malloc(OPENMETEO_BUF_SIZE), .len = 0, .cap = OPENMETEO_BUF_SIZE };
    if (!fb.buf) return ESP_ERR_NO_MEM;
    fb.buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = OPENMETEO_URL,
        .event_handler = http_event_handler,
        .user_data = &fb,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { free(fb.buf); return ESP_FAIL; }

    esp_err_t err = esp_http_client_perform(client);
    int status = err == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "fetch failed err=%d http=%d", err, status);
        free(fb.buf);
        return (err == ESP_OK) ? ESP_FAIL : err;
    }
    ESP_LOGI(TAG, "downloaded %u bytes", (unsigned)fb.len);

    err = parse_openmeteo_json(fb.buf, out);
    free(fb.buf);
    return err;
}
