/* Host test: A8 glyph-run span table build + walk equivalence.
 * Build: cc -std=c11 -Wall -Wextra -o /tmp/test_text_spans tools/test_text_spans.c && /tmp/test_text_spans
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/eva_text_spans.h"

enum { W = 64, H = 8 };

static void fill_row(uint8_t *a8, int row, int from, int to, uint8_t v)
{
    for (int x = from; x < to; ++x) a8[row * W + x] = v;
}

static void test_empty_mask(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    eva_span_t spans[64];
    uint32_t row_start[H + 1];
    int n = eva_text_build_spans(a8, W, 0, H, spans, 64, row_start);
    assert(n == 0);
    for (int r = 0; r <= H; ++r) assert(row_start[r] == 0);
}

static void test_runs_found(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    fill_row(a8, 2, 4, 10, 200);      /* one run */
    fill_row(a8, 3, 0, 2, 50);        /* two runs */
    fill_row(a8, 3, 60, 64, 50);
    eva_span_t spans[64];
    uint32_t row_start[H + 1];
    int n = eva_text_build_spans(a8, W, 0, H, spans, 64, row_start);
    assert(n == 3);
    /* row 2: spans[row_start[2]..row_start[3]) */
    assert(row_start[3] - row_start[2] == 1);
    assert(spans[row_start[2]].x == 4 && spans[row_start[2]].len == 6);
    assert(row_start[4] - row_start[3] == 2);
    assert(spans[row_start[3]].x == 0 && spans[row_start[3]].len == 2);
    assert(spans[row_start[3] + 1].x == 60 && spans[row_start[3] + 1].len == 4);
}

static void test_cap_merges_to_single_span(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    /* 20 isolated pixels on row 1 → more than EVA_TEXT_MAX_SPANS_PER_ROW runs. */
    for (int i = 0; i < 20; ++i) a8[1 * W + i * 3] = 255;
    eva_span_t spans[128];
    uint32_t row_start[H + 1];
    int n = eva_text_build_spans(a8, W, 0, H, spans, 128, row_start);
    assert(row_start[2] - row_start[1] == 1);          /* merged */
    eva_span_t s = spans[row_start[1]];
    assert(s.x == 0 && s.x + s.len == 19 * 3 + 1);     /* covers first..last */
    (void)n;
}

/* Equivalence: walking spans must visit exactly the non-zero pixels. */
static void test_walk_equivalence(void)
{
    static uint8_t a8[W * H];
    memset(a8, 0, sizeof a8);
    fill_row(a8, 0, 1, 5, 10);
    fill_row(a8, 4, 30, 40, 99);
    fill_row(a8, 7, 63, 64, 1);
    eva_span_t spans[64];
    uint32_t row_start[H + 1];
    (void)eva_text_build_spans(a8, W, 0, H, spans, 64, row_start);

    static uint8_t visited[W * H];
    memset(visited, 0, sizeof visited);
    for (int r = 0; r < H; ++r) {
        for (uint32_t s = row_start[r]; s < row_start[r + 1]; ++s) {
            for (int x = spans[s].x; x < spans[s].x + spans[s].len; ++x) {
                visited[r * W + x] = 1;
            }
        }
    }
    for (int i = 0; i < W * H; ++i) {
        if (a8[i]) assert(visited[i]);          /* every glyph px covered */
    }
    /* Spans may include interior zeros only via the cap merge; none here. */
    for (int i = 0; i < W * H; ++i) {
        if (visited[i]) assert(a8[i]);
    }
}

int main(void)
{
    test_empty_mask();
    test_runs_found();
    test_cap_merges_to_single_span();
    test_walk_equivalence();
    printf("OK\n");
    return 0;
}
