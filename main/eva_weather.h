#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WEATHER_UNKNOWN = 0,
    WEATHER_CLEAR_DAY,
    WEATHER_CLEAR_NIGHT,
    WEATHER_PARTLY_CLOUDY_DAY,
    WEATHER_PARTLY_CLOUDY_NIGHT,
    WEATHER_CLOUDY,
    WEATHER_FOG,
    WEATHER_RAIN,
    WEATHER_HEAVY_RAIN,
    WEATHER_SNOW,
    WEATHER_THUNDERSTORM,
    WEATHER_SLEET,
    WEATHER_HAIL,
    WEATHER_KIND_COUNT
} weather_kind_t;

/* Back-compat aliases for callers/CDC commands that used the old 3-kind set. */
#define WEATHER_SUNNY WEATHER_CLEAR_DAY

/* clearoutside.com gives explicit precipitation type per hour. We collapse the
 * vendor categories into this 4-bit enum; canvas picks a particle system from it.
 * Lightning is signalled out-of-band via desc text since clearoutside has no
 * dedicated thunder row — see precip_type_t mapping in weather_fetch.c. */
typedef enum {
    PRECIP_NONE = 0,
    PRECIP_DRIZZLE,        /* "Very Light Rain" */
    PRECIP_LIGHT_RAIN,     /* "Light Rain" */
    PRECIP_RAIN,           /* "Rain" */
    PRECIP_HEAVY_RAIN,     /* "Heavy Rain" */
    PRECIP_SNOW,           /* "Snow" / "Light Snow" / "Heavy Snow" */
    PRECIP_SLEET,          /* "Sleet" */
    PRECIP_HAIL,           /* "Hail" */
    PRECIP_THUNDER,        /* synthesised when desc contains "Thunder" */
} precip_type_t;

typedef struct {
    /* Derived/synthesised view kept for back-compat with existing scene code,
     * CDC commands, and the canvas debug path. derive_kind_from_raw() in
     * eva_weather.c rebuilds this from raw fields. */
    weather_kind_t kind;

    /* Compact human label (UK) used by the on-screen description.
     * Set by weather_fetch based on precip type + cloud %. */
    char desc[64];

    /* --- Raw scalar weather measurements from clearoutside.com day_0[0] --- */
    int8_t  temp_c;
    int8_t  feels_like_c;
    int16_t wind_kph;             /* -1 unknown. clearoutside reports mph -> we convert. */
    int16_t wind_dir_deg;         /* -1 unknown. 0..359 from "(NNN&deg;)" in wind title. */
    uint8_t cloud_low_pct;        /* 0..100. Stratus/cumulus near surface. */
    uint8_t cloud_mid_pct;        /* 0..100. Altocumulus mid-height. */
    uint8_t cloud_high_pct;       /* 0..100. Cirrus high streaks. */
    uint8_t cloud_total_pct;      /* 0..100. clearoutside "Total Clouds" (sometimes != max of layers). */
    uint8_t fog_pct;              /* 0..100. clearoutside "Fog (%)". */
    uint8_t visibility_km_x10;    /* deci-km from miles*16, capped 255 => 25.5 km. */
    precip_type_t precip_type;
    uint16_t precip_mm_x10;       /* mm * 10. 0..655 mm range. */

    /* --- Astronomy --- */
    int16_t sunrise_min;          /* minutes-of-day, -1 unknown */
    int16_t sunset_min;           /* minutes-of-day, -1 unknown */
    /* Moon transit: rise/set can straddle midnight (e.g. rise 23:50, set 04:10
     * next day). When set < rise, the visible-arc window is [rise, set+1440)
     * with current minute wrapped accordingly. -1 means unknown. */
    int16_t moonrise_min;
    int16_t moonset_min;
    uint8_t moon_phase_pct;       /* 0..100 illumination */
    /* High bit set = waning (decreasing illumination), clear = waxing/full/new. */
    uint8_t moon_waning;

    time_t fetched_at;

    /* --- New fields for dual-source pipeline (2026-05-25) --- */
    /* Lifestyle cloud cover from open-meteo. Drives sky_cover_fraction()
     * for sun/moon visibility — NOT a multiplicative combine of layers. */
    uint8_t  cloud_cover_pct;
    /* Raw WMO weather code from open-meteo, for status/debug display.
     * -1 if unavailable. */
    int      weather_code;
    /* Sunshine minutes for the current day (open-meteo daily). Status only. */
    uint16_t sunshine_minutes;
    /* Per-source last-success timestamps (epoch seconds). 0 = never. */
    int64_t  openmeteo_ts;
    int64_t  clearoutside_ts;
} weather_state_t;

typedef void (*eva_weather_update_cb_t)(const weather_state_t *st, void *user);

void eva_weather_init(void);
void eva_weather_set_update_cb(eva_weather_update_cb_t cb, void *user);
void eva_weather_set(const weather_state_t *st);
const weather_state_t *eva_weather_get(void);
bool eva_weather_copy(weather_state_t *out);

const char *weather_kind_name(weather_kind_t kind);
const char *weather_kind_label_uk(weather_kind_t kind);
bool weather_kind_parse(const char *name, weather_kind_t *out);

/* Synthesise the derived weather_kind_t from raw clearoutside readings.
 * Called by eva_weather_set() so callers can ignore raw fields if they only
 * care about the categorical kind. Pure function; takes is_night to pick
 * day-vs-night variants of clear/partly-cloudy. */
weather_kind_t derive_kind_from_raw(const weather_state_t *st, bool is_night);

const char *precip_type_name(precip_type_t p);

#ifdef __cplusplus
}
#endif
