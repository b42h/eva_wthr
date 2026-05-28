#include "eva_weather.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "eva_weather";

static weather_state_t s_weather = {
    .kind = WEATHER_UNKNOWN,
    .desc = "Невідомо",
    .temp_c = 0,
    .feels_like_c = 0,
    .wind_kph = -1,
    .wind_dir_deg = -1,
    .cloud_low_pct = 0,
    .cloud_mid_pct = 0,
    .cloud_high_pct = 0,
    .cloud_total_pct = 0,
    .fog_pct = 0,
    .visibility_km_x10 = 0,
    .precip_type = PRECIP_NONE,
    .precip_mm_x10 = 0,
    .sunrise_min = -1,
    .sunset_min = -1,
    .moonrise_min = -1,
    .moonset_min = -1,
    .moon_phase_pct = 0,
    .moon_waning = 0,
    .fetched_at = 0,
};
static SemaphoreHandle_t s_weather_lock;
static eva_weather_update_cb_t s_update_cb;
static void *s_update_user;

static int8_t clamp_temp(int temp_c)
{
    if (temp_c < -50) return -50;
    if (temp_c > 50) return 50;
    return (int8_t)temp_c;
}

const char *weather_kind_name(weather_kind_t kind)
{
    switch (kind) {
    case WEATHER_CLEAR_DAY:           return "clear-day";
    case WEATHER_CLEAR_NIGHT:         return "clear-night";
    case WEATHER_PARTLY_CLOUDY_DAY:   return "partly-cloudy-day";
    case WEATHER_PARTLY_CLOUDY_NIGHT: return "partly-cloudy-night";
    case WEATHER_CLOUDY:              return "cloudy";
    case WEATHER_FOG:                 return "fog";
    case WEATHER_RAIN:                return "rain";
    case WEATHER_HEAVY_RAIN:          return "heavy-rain";
    case WEATHER_SNOW:                return "snow";
    case WEATHER_THUNDERSTORM:        return "thunderstorm";
    case WEATHER_SLEET:               return "sleet";
    case WEATHER_HAIL:                return "hail";
    default:                          return "unknown";
    }
}

const char *weather_kind_label_uk(weather_kind_t kind)
{
    switch (kind) {
    case WEATHER_CLEAR_DAY:           return "Ясно";
    case WEATHER_CLEAR_NIGHT:         return "Ясна ніч";
    case WEATHER_PARTLY_CLOUDY_DAY:   return "Мінлива хмарність";
    case WEATHER_PARTLY_CLOUDY_NIGHT: return "Мінлива хмарність";
    case WEATHER_CLOUDY:              return "Хмарно";
    case WEATHER_FOG:                 return "Туман";
    case WEATHER_RAIN:                return "Дощ";
    case WEATHER_HEAVY_RAIN:          return "Злива";
    case WEATHER_SNOW:                return "Сніг";
    case WEATHER_THUNDERSTORM:        return "Гроза";
    case WEATHER_SLEET:               return "Мокрий сніг";
    case WEATHER_HAIL:                return "Град";
    default:                          return "Невідомо";
    }
}

bool weather_kind_parse(const char *name, weather_kind_t *out)
{
    if (!name || !out) return false;

    /* New canonical names. */
    if (strcmp(name, "clear-day") == 0)           { *out = WEATHER_CLEAR_DAY; return true; }
    if (strcmp(name, "clear-night") == 0)         { *out = WEATHER_CLEAR_NIGHT; return true; }
    if (strcmp(name, "partly-cloudy-day") == 0)   { *out = WEATHER_PARTLY_CLOUDY_DAY; return true; }
    if (strcmp(name, "partly-cloudy-night") == 0) { *out = WEATHER_PARTLY_CLOUDY_NIGHT; return true; }
    if (strcmp(name, "cloudy") == 0)              { *out = WEATHER_CLOUDY; return true; }
    if (strcmp(name, "fog") == 0)                 { *out = WEATHER_FOG; return true; }
    if (strcmp(name, "rain") == 0)                { *out = WEATHER_RAIN; return true; }
    if (strcmp(name, "heavy-rain") == 0 ||
        strcmp(name, "pouring") == 0)             { *out = WEATHER_HEAVY_RAIN; return true; }
    if (strcmp(name, "snow") == 0)                { *out = WEATHER_SNOW; return true; }
    if (strcmp(name, "thunderstorm") == 0)        { *out = WEATHER_THUNDERSTORM; return true; }
    if (strcmp(name, "sleet") == 0)               { *out = WEATHER_SLEET; return true; }
    if (strcmp(name, "hail") == 0)                { *out = WEATHER_HAIL; return true; }

    /* Back-compat aliases that the old CDC commands and main.c used. */
    if (strcmp(name, "sunny") == 0 || strcmp(name, "clear") == 0) {
        *out = WEATHER_CLEAR_DAY;
        return true;
    }
    if (strcmp(name, "cloud") == 0 || strcmp(name, "overcast") == 0) {
        *out = WEATHER_CLOUDY;
        return true;
    }
    if (strcmp(name, "rainy") == 0 || strcmp(name, "shower") == 0 ||
        strcmp(name, "drizzle") == 0) {
        *out = WEATHER_RAIN;
        return true;
    }
    if (strcmp(name, "mist") == 0 || strcmp(name, "haze") == 0) {
        *out = WEATHER_FOG;
        return true;
    }
    if (strcmp(name, "ice") == 0 || strcmp(name, "freezing-rain") == 0) {
        *out = WEATHER_SLEET;
        return true;
    }
    if (strcmp(name, "blizzard") == 0) {
        *out = WEATHER_SNOW;
        return true;
    }
    if (strcmp(name, "thunder") == 0 || strcmp(name, "storm") == 0) {
        *out = WEATHER_THUNDERSTORM;
        return true;
    }
    if (strcmp(name, "unknown") == 0) {
        *out = WEATHER_UNKNOWN;
        return true;
    }
    return false;
}

const char *precip_type_name(precip_type_t p)
{
    switch (p) {
    case PRECIP_NONE:        return "none";
    case PRECIP_DRIZZLE:     return "drizzle";
    case PRECIP_LIGHT_RAIN:  return "light-rain";
    case PRECIP_RAIN:        return "rain";
    case PRECIP_HEAVY_RAIN:  return "heavy-rain";
    case PRECIP_SNOW:        return "snow";
    case PRECIP_SLEET:       return "sleet";
    case PRECIP_HAIL:        return "hail";
    case PRECIP_THUNDER:     return "thunder";
    default:                 return "unknown";
    }
}

weather_kind_t derive_kind_from_raw(const weather_state_t *st, bool is_night)
{
    if (!st) return WEATHER_UNKNOWN;

    /* Precipitation dominates: if anything is falling, the screen shows that. */
    switch (st->precip_type) {
    case PRECIP_THUNDER:     return WEATHER_THUNDERSTORM;
    case PRECIP_HEAVY_RAIN:  return WEATHER_HEAVY_RAIN;
    case PRECIP_RAIN:
    case PRECIP_LIGHT_RAIN:
    case PRECIP_DRIZZLE:     return WEATHER_RAIN;
    case PRECIP_SNOW:        return WEATHER_SNOW;
    case PRECIP_SLEET:       return WEATHER_SLEET;
    case PRECIP_HAIL:        return WEATHER_HAIL;
    case PRECIP_NONE:        break;
    }

    /* Dry sky. Fog beats clouds beats clear. */
    if (st->fog_pct >= 60) return WEATHER_FOG;

    /* clearoutside Total Clouds is the most authoritative single number for
     * scene selection — it's how a human would describe the sky overall. */
    uint8_t total = st->cloud_total_pct;
    if (total >= 75) return WEATHER_CLOUDY;
    if (total >= 55) return is_night ? WEATHER_PARTLY_CLOUDY_NIGHT
                                     : WEATHER_PARTLY_CLOUDY_DAY;
    return is_night ? WEATHER_CLEAR_NIGHT : WEATHER_CLEAR_DAY;
}

/* NVS schema:
 *   "blob_v2" : raw bytes of weather_state_t (forward-compatible: NVS stores
 *               size separately, so adding fields will just leave new ones at
 *               their static default after restore).
 * The blob carries all current fields without per-key churn. */
static void persist_state(const weather_state_t *st)
{
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    (void)nvs_set_blob(h, "blob_v2", st, sizeof(*st));
    (void)nvs_commit(h);
    nvs_close(h);
}

static void restore_state(void)
{
    nvs_handle_t h;
    if (nvs_open("weather", NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    size_t sz = 0;
    esp_err_t err = nvs_get_blob(h, "blob_v2", NULL, &sz);
    if (err != ESP_OK || sz == 0) {
        nvs_close(h);
        ESP_LOGI(TAG, "no compatible saved weather state (err=%d sz=%u)",
                 (int)err, (unsigned)sz);
        return;
    }

    uint8_t *raw = (uint8_t *)malloc(sz);
    if (!raw) {
        nvs_close(h);
        ESP_LOGW(TAG, "weather restore skipped: alloc %u failed", (unsigned)sz);
        return;
    }

    size_t read_sz = sz;
    err = nvs_get_blob(h, "blob_v2", raw, &read_sz);
    nvs_close(h);
    if (err != ESP_OK) {
        free(raw);
        ESP_LOGI(TAG, "weather restore failed (err=%d sz=%u)", (int)err, (unsigned)sz);
        return;
    }

    weather_state_t restored = s_weather;
    size_t copy_sz = read_sz < sizeof(restored) ? read_sz : sizeof(restored);
    memcpy(&restored, raw, copy_sz);
    free(raw);
    s_weather = restored;

    /* Clamp anything that might be corrupt. */
    if (s_weather.kind < WEATHER_UNKNOWN || s_weather.kind >= WEATHER_KIND_COUNT) {
        s_weather.kind = WEATHER_UNKNOWN;
    }
    s_weather.temp_c = clamp_temp(s_weather.temp_c);
    s_weather.desc[sizeof(s_weather.desc) - 1] = '\0';

    ESP_LOGI(TAG, "restored %s %+dC \"%s\" clouds L=%u/M=%u/H=%u precip=%s",
             weather_kind_name(s_weather.kind), s_weather.temp_c, s_weather.desc,
             (unsigned)s_weather.cloud_low_pct, (unsigned)s_weather.cloud_mid_pct,
             (unsigned)s_weather.cloud_high_pct,
             precip_type_name(s_weather.precip_type));
}

void eva_weather_init(void)
{
    if (!s_weather_lock) {
        s_weather_lock = xSemaphoreCreateMutex();
    }
    restore_state();
    s_weather.weather_code = -1;
    s_weather.openmeteo_ts = 0;
    s_weather.clearoutside_ts = 0;
}

void eva_weather_set_update_cb(eva_weather_update_cb_t cb, void *user)
{
    s_update_cb = cb;
    s_update_user = user;
}

void eva_weather_set(const weather_state_t *st)
{
    if (!st || !s_weather_lock) return;

    weather_state_t copy = *st;
    copy.temp_c = clamp_temp(copy.temp_c);
    copy.feels_like_c = clamp_temp(copy.feels_like_c);
    if (copy.cloud_low_pct > 100)  copy.cloud_low_pct = 100;
    if (copy.cloud_mid_pct > 100)  copy.cloud_mid_pct = 100;
    if (copy.cloud_high_pct > 100) copy.cloud_high_pct = 100;
    if (copy.cloud_total_pct > 100) copy.cloud_total_pct = 100;
    if (copy.fog_pct > 100) copy.fog_pct = 100;
    if (copy.moon_phase_pct > 100) copy.moon_phase_pct = 100;
    copy.desc[sizeof(copy.desc) - 1] = '\0';

    /* Thunderstorm synthesis. Neither clearoutside nor wttr.in surface
     * "Thunder" reliably, so we infer it from heavy precipitation + nearly
     * total cloud cover + either a low cloud ceiling (unstable air, summer
     * convective storms) or strong wind (squall line). This runs for ALL
     * input paths — live fetch, CDC weatherraw, NVS restore. Conservative
     * thresholds keep us from upgrading drizzle to thunder.
     *
     * Skips if caller already set a non-rain-family precip (snow + hail
     * keep their own kind unless they're heavy snow with full overcast).
     *
     * Side effect for manual `weather <kind>` CDC: a caller cannot demote
     * thunder back to rain by passing kind=rain while raw cloud/precip data
     * still meets the storm thresholds — the state stays in sync with the
     * underlying conditions. To force a demote, also update raw via
     * weatherraw with weaker numbers. */
    /* PRECIP_THUNDER is included so a state that already has thunder set
     * (e.g. carried over from a previous fetch or weatherraw override) keeps
     * thunder when the storm conditions still hold — instead of decaying back
     * to plain rain just because the synthesis predicate is "wrong-typed". */
    bool rain_family = (copy.precip_type == PRECIP_LIGHT_RAIN ||
                        copy.precip_type == PRECIP_RAIN ||
                        copy.precip_type == PRECIP_HEAVY_RAIN ||
                        copy.precip_type == PRECIP_THUNDER);
    bool snow_thunder = (copy.precip_type == PRECIP_SNOW &&
                        copy.cloud_total_pct >= 95);
    bool heavy_precip = (copy.precip_mm_x10 >= 25);
    bool overcast    = (copy.cloud_total_pct >= 85);
    bool low_ceiling = (copy.cloud_low_pct >= 70);
    bool windy       = (copy.wind_kph >= 25);
    bool synthesised_thunder = false;
    if ((rain_family || snow_thunder) &&
        heavy_precip && overcast && (low_ceiling || windy)) {
        copy.precip_type = PRECIP_THUNDER;
        synthesised_thunder = true;
    }

    /* Force kind re-derivation when thunder was synthesised, regardless of
     * whether the caller passed an explicit kind — otherwise CDC `weather rain
     * 18 "..."` would stick at WEATHER_RAIN even though we just upgraded the
     * precip to thunder. Otherwise derive only when caller left kind blank.
     *
     * Day vs night is based on current local time being inside the sun window. */
    if (synthesised_thunder ||
        copy.kind == WEATHER_UNKNOWN || copy.kind >= WEATHER_KIND_COUNT) {
        bool is_night = true;
        if (copy.sunrise_min >= 0 && copy.sunset_min >= 0) {
            time_t now = time(NULL);
            struct tm tm_now = {0};
            if (now > 1700000000) {
                localtime_r(&now, &tm_now);
                int m = tm_now.tm_hour * 60 + tm_now.tm_min;
                is_night = (m < copy.sunrise_min || m >= copy.sunset_min);
            }
        }
        copy.kind = derive_kind_from_raw(&copy, is_night);
    }
    /* Force "Гроза" label when synthesis upgraded precip to thunder, even
     * if a caller-supplied desc was non-empty (it would have read "Дощ"). */
    if (synthesised_thunder) {
        strlcpy(copy.desc, weather_kind_label_uk(WEATHER_THUNDERSTORM),
                sizeof(copy.desc));
    } else if (!copy.desc[0]) {
        strlcpy(copy.desc, weather_kind_label_uk(copy.kind), sizeof(copy.desc));
    }
    if (copy.fetched_at == 0) {
        copy.fetched_at = time(NULL);
    }

    if (xSemaphoreTake(s_weather_lock, portMAX_DELAY) == pdTRUE) {
        s_weather = copy;
        xSemaphoreGive(s_weather_lock);
    }

    persist_state(&copy);
    ESP_LOGI(TAG, "set %s %+dC \"%s\" clouds L=%u/M=%u/H=%u tot=%u fog=%u precip=%s",
             weather_kind_name(copy.kind), copy.temp_c, copy.desc,
             (unsigned)copy.cloud_low_pct, (unsigned)copy.cloud_mid_pct,
             (unsigned)copy.cloud_high_pct, (unsigned)copy.cloud_total_pct,
             (unsigned)copy.fog_pct, precip_type_name(copy.precip_type));
    if (s_update_cb) {
        s_update_cb(&copy, s_update_user);
    }
}

const weather_state_t *eva_weather_get(void)
{
    return &s_weather;
}

bool eva_weather_copy(weather_state_t *out)
{
    if (!out || !s_weather_lock) return false;
    if (xSemaphoreTake(s_weather_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return false;
    }
    *out = s_weather;
    xSemaphoreGive(s_weather_lock);
    return true;
}
