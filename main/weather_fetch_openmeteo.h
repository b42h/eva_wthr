/* main/weather_fetch_openmeteo.h
 *
 * open-meteo.com JSON provider. Primary source for lifestyle weather:
 * WMO weather code, cloud cover, temp, wind, precipitation, sunshine. */
#pragma once

#include "weather_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t weather_provider_openmeteo_fetch(weather_partial_t *out);

#ifdef __cplusplus
}
#endif
