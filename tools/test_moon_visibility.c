/* Host test: moon sky_factor / phase_factor / alpha threshold.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_moon_visibility tools/test_moon_visibility.c && /tmp/test_moon_visibility
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define FIB_13  13
#define FIB_233 233

static float sky_nightness(int m, int sr, int ss)
{
    if (m >= sr && m <= ss) {
        return 0.0f;
    }
    const float DUSK = 30.0f;
    float past = (m < sr) ? (float)(sr - m) : (float)(m - ss);
    float n = past / DUSK;
    if (n > 1.0f) n = 1.0f;
    return n;
}

static float moon_sky_factor(int m, int sr, int ss)
{
    float n = sky_nightness(m, sr, ss);
    return 0.30f + 0.70f * n;
}

static float moon_phase_factor(uint8_t phase_pct)
{
    return 0.35f + 0.65f * ((float)phase_pct * 0.01f);
}

static uint8_t moon_alpha(float vis, float sky_factor, float phase_factor)
{
    return (uint8_t)((float)FIB_233 * vis * sky_factor * phase_factor);
}

static void expect_true(int line, bool got, const char *label)
{
    if (!got) {
        fprintf(stderr, "FAIL line %d: %s expected true\n", line, label);
        exit(1);
    }
}

int main(void)
{
    const int sr = 360;   /* 06:00 */
    const int ss = 1080;  /* 18:00 */
    float prev;

    /* sky_factor: monotonic 0.30 (day) -> 1.0 (deep night). */
    prev = moon_sky_factor(sr + 60, sr, ss);
    expect_true(__LINE__, prev == 0.30f, "noon sky_factor");
    for (int m = ss + 1; m <= ss + 60; ++m) {
        float f = moon_sky_factor(m, sr, ss);
        expect_true(__LINE__, f >= prev, "sky_factor monotonic dusk");
        prev = f;
    }
    expect_true(__LINE__, moon_sky_factor(ss + 60, sr, ss) > 0.99f, "deep night sky_factor");

    /* phase_factor: monotonic 0.35 (new) -> 1.0 (full). */
    prev = moon_phase_factor(0);
    expect_true(__LINE__, prev == 0.35f, "new moon phase_factor");
    for (int pct = 1; pct <= 100; ++pct) {
        float f = moon_phase_factor((uint8_t)pct);
        expect_true(__LINE__, f >= prev, "phase_factor monotonic");
        prev = f;
    }
    expect_true(__LINE__, moon_phase_factor(100) == 1.0f, "full moon phase_factor");

    /* Young crescent at noon with heavy cloud cover: below draw threshold. */
    float vis_hazy = 0.15f;
    uint8_t young_day = moon_alpha(vis_hazy,
                                   moon_sky_factor(sr + 120, sr, ss),
                                   moon_phase_factor(5));
    expect_true(__LINE__, young_day < FIB_13, "young moon hidden by day haze");

    /* Clear day young crescent: faint but above threshold (draw_moon_phase
     * still skips phase_pct <= 2 for true new moon). */
    float vis_clear = 1.0f;
    uint8_t young_day_clear = moon_alpha(vis_clear,
                                         moon_sky_factor(sr + 120, sr, ss),
                                         moon_phase_factor(5));
    expect_true(__LINE__, young_day_clear >= FIB_13, "young moon faint clear day");

    /* Same phase at night: visible. */
    uint8_t young_night = moon_alpha(vis_clear,
                                     moon_sky_factor(ss + 90, sr, ss),
                                     moon_phase_factor(5));
    expect_true(__LINE__, young_night >= FIB_13, "young moon visible at night");

    /* Full moon at noon: faint but drawable. */
    uint8_t full_day = moon_alpha(vis_clear,
                                  moon_sky_factor(sr + 120, sr, ss),
                                  moon_phase_factor(100));
    expect_true(__LINE__, full_day >= FIB_13, "full moon faint by day");

    printf("OK test_moon_visibility\n");
    return 0;
}
