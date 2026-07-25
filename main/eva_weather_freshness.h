#pragma once

/* Monotonic freshness check for clearoutside (and similar) success stamps.
 * Uses esp_timer-style microseconds so wall-clock jumps cannot flip stale. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* have: provider has succeeded at least once.
 * last_ok_us / now_us: monotonic microseconds (esp_timer_get_time).
 * fresh_us: max age in microseconds. */
static inline bool eva_provider_is_fresh_mono(bool have,
                                             int64_t last_ok_us,
                                             int64_t now_us,
                                             int64_t fresh_us)
{
    if (!have || last_ok_us <= 0 || fresh_us <= 0) return false;
    int64_t age = now_us - last_ok_us;
    if (age < 0) return false;
    return age < fresh_us;
}

#ifdef __cplusplus
}
#endif
