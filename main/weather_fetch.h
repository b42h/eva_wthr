/* main/weather_fetch.h
 *
 * Dual-source weather coordinator. Owns two timers (open-meteo every
 * FIB_13 min, clearoutside every FIB_89 min) and merges results into
 * weather_state_t with per-field source priority. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Start the coordinator. Boots both timers; first fetches fire immediately.
 * Idempotent — safe to call once at app_main. */
void weather_fetch_start(void);

/* Force an immediate refresh of BOTH sources, ignoring their regular cadence.
 * Used by the CDC `weather refresh` command. */
void weather_fetch_request(void);

/* Provenance accessors for the `status` CDC command. Returns 0 if a source
 * has never succeeded. Epoch seconds (local time epoch via gettimeofday). */
int64_t weather_fetch_openmeteo_last_ts(void);
int64_t weather_fetch_clearoutside_last_ts(void);

/* Whether the *next* scheduled fire of each source is a retry (backoff)
 * rather than the regular cadence. For the status display. */
bool weather_fetch_openmeteo_retrying(void);
bool weather_fetch_clearoutside_retrying(void);

#ifdef __cplusplus
}
#endif
