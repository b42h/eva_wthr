/* Pure weather-transition math: easing, lerp, significance test, progress.
 * Host-testable — no rendering, no globals. The canvas owns one instance
 * and drives the live render fields from it each frame.
 * See docs/superpowers/specs/2026-07-19-smooth-weather-transitions-design.md */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* We only need the enum values' identity/equality here, not their meaning,
 * so accept them as plain ints to keep this header dependency-free. Callers
 * pass (int)kind / (int)precip_type. */

typedef struct {
    bool     active;
    float    progress;              /* 0..1 */
    float    duration_s;            /* default EVA_WX_TRANSITION_DEFAULT_S */

    uint8_t  start_cover,  target_cover;
    uint8_t  start_cloud_pct[3], target_cloud_pct[3];
    uint8_t  start_fog,    target_fog;
    float    start_density, target_density;

    int      from_kind, to_kind;     /* sky palette lerp endpoints */
    int      from_precip, to_precip;
} eva_wx_transition_t;

#define EVA_WX_TRANSITION_DEFAULT_S 15.0f
#define EVA_WX_COVER_SIGNIFICANT     5   /* |Δcover| below this = not significant */

static inline float eva_wx_clamp01(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

/* smoothstep ease-in-out */
static inline float eva_wx_ease(float p)
{
    p = eva_wx_clamp01(p);
    return p * p * (3.0f - 2.0f * p);
}

static inline uint8_t eva_wx_lerp_u8(uint8_t a, uint8_t b, float t)
{
    float v = (float)a + ((float)b - (float)a) * t;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (uint8_t)(v + 0.5f);
}

static inline float eva_wx_lerp_f(float a, float b, float t)
{
    return a + (b - a) * t;
}

/* Precip intensity multiplier during a transition. First half fades the OLD
 * precip out (from-precip still active); second half fades the NEW in. When
 * from==to (same precip family) there is nothing to fade → always 1.0. */
static inline float eva_wx_precip_ramp(float p, bool same_precip)
{
    if (same_precip) return 1.0f;
    p = eva_wx_clamp01(p);
    if (p < 0.5f) return 1.0f - p * 2.0f;   /* 1 → 0 over [0,0.5] */
    return (p - 0.5f) * 2.0f;               /* 0 → 1 over [0.5,1] */
}

/* Which precip identity should be shown at progress p (flips at midpoint). */
static inline int eva_wx_precip_at(const eva_wx_transition_t *t, float p)
{
    return (p < 0.5f) ? t->from_precip : t->to_precip;
}

/* A change is significant (worth animating) if the kind changes, cover moves
 * by >= EVA_WX_COVER_SIGNIFICANT, or the precip type changes. Temp/text-only
 * updates are not significant → apply instantly. */
static inline bool eva_wx_change_is_significant(
    int cur_kind, uint8_t cur_cover, int cur_precip,
    int new_kind, uint8_t new_cover, int new_precip)
{
    if (cur_kind != new_kind) return true;
    if (cur_precip != new_precip) return true;
    int d = (int)new_cover - (int)cur_cover;
    if (d < 0) d = -d;
    return d >= EVA_WX_COVER_SIGNIFICANT;
}

/* Advance progress by dt. Returns true when the transition just finished
 * (this call crossed 1.0). Caller finalizes (swaps kind, clears counters). */
static inline bool eva_wx_advance(eva_wx_transition_t *t, float dt)
{
    if (!t->active) return false;
    if (t->duration_s <= 0.0f) { t->progress = 1.0f; t->active = false; return true; }
    t->progress += dt / t->duration_s;
    if (t->progress >= 1.0f) {
        t->progress = 1.0f;
        t->active = false;
        return true;
    }
    return false;
}
