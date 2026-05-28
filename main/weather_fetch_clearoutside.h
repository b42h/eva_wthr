/* main/weather_fetch_clearoutside.h
 *
 * Clearoutside.com HTML scraper. Provides astronomy (moon, sun) and
 * cloud-layer breakdown. See weather_provider.h for the contract. */
#pragma once

#include "weather_provider.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t weather_provider_clearoutside_fetch(weather_partial_t *out);

#ifdef __cplusplus
}
#endif
