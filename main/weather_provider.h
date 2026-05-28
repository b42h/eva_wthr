/* main/weather_provider.h
 *
 * Common contract for weather data providers. Each provider parses one
 * external source (HTTP API or HTML scrape) and returns a partial state
 * with `has_*` flags marking which fields are populated. The coordinator
 * in weather_fetch.c is responsible for merging results into the canonical
 * weather_state_t with per-field source priority rules.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "eva_weather.h"   /* weather_kind_t, precip_type_t */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Kind and raw WMO code (open-meteo only) */
    bool has_kind;
    weather_kind_t kind;
    int weather_code;            /* -1 if unavailable */

    /* Cloud cover — `cloud_cover_pct` is the lifestyle metric (open-meteo).
     * cloud_low/mid/high are the layer breakdown (either source). */
    bool has_clouds;
    uint8_t cloud_cover_pct;
    uint8_t cloud_low_pct;
    uint8_t cloud_mid_pct;
    uint8_t cloud_high_pct;

    bool has_temp;
    int16_t temp_c;
    int16_t feels_like_c;

    bool has_wind;
    uint16_t wind_kph;
    uint16_t wind_dir_deg;

    bool has_precip;
    precip_type_t precip;
    uint16_t precip_mm_x10;

    bool has_sun;
    int16_t sunrise_min;         /* minutes-of-day local time */
    int16_t sunset_min;

    bool has_moon;
    int16_t moonrise_min;
    int16_t moonset_min;
    uint8_t moon_phase_pct;
    uint8_t moon_waning;

    bool has_visibility;
    uint16_t visibility_km_x10;
    uint8_t fog_pct;

    bool has_sunshine;
    uint16_t sunshine_minutes;   /* current hour, 0..60 */
} weather_partial_t;

/* Each provider returns ESP_OK on parse success (some `has_*` may still be
 * false if the source lacks that field). Returns ESP_ERR_* on transport or
 * parse failure; caller treats *out as undefined. */
esp_err_t weather_provider_openmeteo_fetch(weather_partial_t *out);
esp_err_t weather_provider_clearoutside_fetch(weather_partial_t *out);

#ifdef __cplusplus
}
#endif
