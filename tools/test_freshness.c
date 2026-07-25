/* Host test: monotonic provider freshness (eva_weather_freshness.h).
 * Build: cc -std=c11 -Wall -Wextra -I main -o /tmp/test_freshness tools/test_freshness.c && /tmp/test_freshness
 */
#include <assert.h>
#include <stdio.h>
#include "eva_weather_freshness.h"

/* 3 * 89 min in microseconds — matches CLEAROUTSIDE_FRESH_SEC after A-11. */
#define FRESH_US (3LL * 89LL * 60LL * 1000000LL)

int main(void)
{
    /* Mono base must exceed the fresh window so (now - FRESH_US) stays > 0. */
    int64_t now = 100LL * 1000000000LL;
    int64_t last = now - 60LL * 1000000LL; /* 60 s ago */

    assert(!eva_provider_is_fresh_mono(false, last, now, FRESH_US));
    assert(!eva_provider_is_fresh_mono(true, 0, now, FRESH_US));
    assert(eva_provider_is_fresh_mono(true, last, now, FRESH_US));

    /* Just inside the window */
    last = now - (FRESH_US - 1);
    assert(eva_provider_is_fresh_mono(true, last, now, FRESH_US));

    /* At / past the window */
    last = now - FRESH_US;
    assert(!eva_provider_is_fresh_mono(true, last, now, FRESH_US));
    last = now - FRESH_US - 1;
    assert(!eva_provider_is_fresh_mono(true, last, now, FRESH_US));

    /* Negative age (should not happen with mono, but guard) */
    assert(!eva_provider_is_fresh_mono(true, now + 1000, now, FRESH_US));

    /* Wall-clock jump simulation: mono age is independent of epoch.
     * A freshly stamped mono value stays fresh even if epoch went backwards. */
    last = now - 1000;
    assert(eva_provider_is_fresh_mono(true, last, now, FRESH_US));

    printf("ok\n");
    return 0;
}
