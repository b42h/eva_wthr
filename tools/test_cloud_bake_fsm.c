/* Host test: cloud bake state machine (mirrors eva_weather_canvas.c logic).
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_cloud_bake_fsm tools/test_cloud_bake_fsm.c && /tmp/test_cloud_bake_fsm
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

#define BAKE_IDLE      0
#define BAKE_REQUESTED 1
#define BAKE_RUNNING   2
#define BAKE_DONE      3

typedef struct {
    uint8_t active_variant;
    bool morphing;
    float morph_t;
    float morph_clock;
    float morph_hold_s;
    uint8_t bake_state;
    uint8_t bake_variant;
} strip_fsm_t;

static bool should_start_morph(const strip_fsm_t *s)
{
    return !s->morphing && s->bake_state == BAKE_IDLE && s->morph_clock >= s->morph_hold_s;
}

static void render_tick(strip_fsm_t *s, float dt, bool *notify_out)
{
    *notify_out = false;
    if (s->bake_state == BAKE_DONE) {
        s->bake_state = BAKE_IDLE;
    }
    s->morph_clock += dt;
    if (!s->morphing) {
        if (s->bake_state != BAKE_IDLE) return;
        if (s->morph_clock >= s->morph_hold_s) {
            s->morphing = true;
            s->morph_t = 0.0f;
            s->morph_clock = 0.0f;
        }
        return;
    }
    float duration = 1.0f;
    s->morph_t += dt / duration;
    if (s->morph_t >= 1.0f) {
        uint8_t hidden = s->active_variant ^ 1U;
        s->active_variant = hidden;
        s->morphing = false;
        s->morph_t = 0.0f;
        s->morph_clock = 0.0f;
        s->bake_variant = s->active_variant ^ 1U;
        s->bake_state = BAKE_REQUESTED;
        *notify_out = true;
    }
}

static void baker_tick(strip_fsm_t *s)
{
    if (s->bake_state != BAKE_REQUESTED) return;
    s->bake_state = BAKE_RUNNING;
    s->bake_state = BAKE_DONE;
}

static void expect_eq(int line, int got, int want, const char *label)
{
    if (got != want) {
        fprintf(stderr, "FAIL line %d: %s got %d want %d\n", line, label, got, want);
        exit(1);
    }
}

static void expect_true(int line, bool got, const char *label)
{
    if (!got) {
        fprintf(stderr, "FAIL line %d: %s expected true\n", line, label);
        exit(1);
    }
}

static void expect_false(int line, bool got, const char *label)
{
    if (got) {
        fprintf(stderr, "FAIL line %d: %s expected false\n", line, label);
        exit(1);
    }
}

int main(void)
{
    strip_fsm_t s = {
        .active_variant = 0,
        .morphing = false,
        .morph_t = 0.0f,
        .morph_clock = 0.0f,
        .morph_hold_s = 10.0f,
        .bake_state = BAKE_IDLE,
        .bake_variant = 0,
    };
    bool notify = false;

    expect_eq(__LINE__, s.bake_state, BAKE_IDLE, "init bake_state");
    expect_false(__LINE__, should_start_morph(&s), "hold not elapsed");

    s.morph_clock = 10.0f;
    expect_true(__LINE__, should_start_morph(&s), "hold elapsed idle");

    render_tick(&s, 0.0f, &notify);
    expect_true(__LINE__, s.morphing, "morph started");
    expect_eq(__LINE__, s.bake_state, BAKE_IDLE, "still idle during morph");

    render_tick(&s, 1.0f, &notify);
    expect_true(__LINE__, notify, "morph complete notifies baker");
    expect_eq(__LINE__, s.bake_state, BAKE_REQUESTED, "requested after morph");
    expect_eq(__LINE__, s.bake_variant, 0U, "rebake old active variant");
    expect_false(__LINE__, should_start_morph(&s), "blocked while bake pending");

    baker_tick(&s);
    expect_eq(__LINE__, s.bake_state, BAKE_DONE, "baker sets done");

    render_tick(&s, 0.0f, &notify);
    expect_eq(__LINE__, s.bake_state, BAKE_IDLE, "render clears done->idle");

    render_tick(&s, s.morph_hold_s, &notify);
    expect_true(__LINE__, s.morphing, "morph starts once hold elapses after bake");

    printf("OK cloud bake FSM (%d checks)\n", __LINE__);
    return 0;
}
