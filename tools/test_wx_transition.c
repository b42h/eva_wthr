/* Host test: pure weather-transition math.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_wx_transition tools/test_wx_transition.c && /tmp/test_wx_transition
 */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include "../main/eva_wx_transition.h"

static int approx(float a, float b) { return fabsf(a - b) < 0.01f; }

int main(void)
{
    /* ease endpoints and midpoint */
    assert(approx(eva_wx_ease(0.0f), 0.0f));
    assert(approx(eva_wx_ease(1.0f), 1.0f));
    assert(approx(eva_wx_ease(0.5f), 0.5f));
    assert(eva_wx_ease(-1.0f) == 0.0f && eva_wx_ease(2.0f) == 1.0f);

    /* u8 lerp rounds and clamps */
    assert(eva_wx_lerp_u8(0, 100, 0.0f) == 0);
    assert(eva_wx_lerp_u8(0, 100, 1.0f) == 100);
    assert(eva_wx_lerp_u8(0, 100, 0.5f) == 50);
    assert(eva_wx_lerp_u8(20, 80, 0.5f) == 50);

    /* precip ramp: same family = always 1; cross-type dips to 0 at midpoint */
    assert(approx(eva_wx_precip_ramp(0.5f, true), 1.0f));
    assert(approx(eva_wx_precip_ramp(0.0f, false), 1.0f));
    assert(approx(eva_wx_precip_ramp(0.5f, false), 0.0f));
    assert(approx(eva_wx_precip_ramp(1.0f, false), 1.0f));
    assert(eva_wx_precip_ramp(0.25f, false) > 0.4f);  /* old fading */
    assert(eva_wx_precip_ramp(0.75f, false) > 0.4f);  /* new rising */

    /* precip identity flips at midpoint */
    eva_wx_transition_t t = {0};
    t.from_precip = 1; t.to_precip = 3;
    assert(eva_wx_precip_at(&t, 0.4f) == 1);
    assert(eva_wx_precip_at(&t, 0.6f) == 3);

    /* significance: kind change, precip change, big cover delta all count */
    assert(eva_wx_change_is_significant(0, 10, 0, 1, 10, 0));   /* kind */
    assert(eva_wx_change_is_significant(0, 10, 0, 0, 10, 2));   /* precip */
    assert(eva_wx_change_is_significant(0, 10, 0, 0, 80, 0));   /* cover */
    assert(!eva_wx_change_is_significant(0, 40, 0, 0, 42, 0));  /* tiny cover */
    assert(!eva_wx_change_is_significant(0, 40, 0, 0, 40, 0));  /* nothing */

    /* advance: finishes exactly once, at/after full duration */
    eva_wx_transition_t a = {0};
    a.active = true; a.progress = 0.0f; a.duration_s = 1.0f;
    assert(!eva_wx_advance(&a, 0.5f));       /* p=0.5, not done */
    assert(eva_wx_advance(&a, 0.6f));        /* p>=1, done, returns true once */
    assert(!a.active);
    assert(!eva_wx_advance(&a, 1.0f));       /* already inactive */

    /* duration 0 = instant finish */
    eva_wx_transition_t z = {0};
    z.active = true; z.duration_s = 0.0f;
    assert(eva_wx_advance(&z, 0.001f));
    assert(!z.active);

    printf("test_wx_transition: all passed\n");
    return 0;
}
