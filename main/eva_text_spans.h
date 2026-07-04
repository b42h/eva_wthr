/* Glyph-run span table for A8 text masks (pure, host-testable).
 * Built once per text rebake; the per-frame blit walks only these runs
 * instead of scanning the full bbox. See eva_weather_canvas.c. */
#pragma once
#include <stdint.h>

typedef struct {
    uint16_t x;
    uint16_t len;
} eva_span_t;

#define EVA_TEXT_MAX_SPANS_PER_ROW 16

/* Scan rows [y0, y1) of `a8` (stride w). For each row append its runs of
 * non-zero pixels to `spans`; `row_start[i]` = first span index of row
 * (y0+i), with a final sentinel row_start[y1-y0] = total count. A row with
 * more than EVA_TEXT_MAX_SPANS_PER_ROW runs is merged into one span from
 * its first to last non-zero pixel. Returns the total span count, or -1 if
 * `cap` would be exceeded (caller falls back to bbox blit). */
static inline int eva_text_build_spans(const uint8_t *a8, int w,
                                       int y0, int y1,
                                       eva_span_t *spans, int cap,
                                       uint32_t *row_start)
{
    int total = 0;
    for (int row = y0; row < y1; ++row) {
        row_start[row - y0] = (uint32_t)total;
        const uint8_t *m = &a8[row * w];
        int row_first = -1, row_last = -1;
        int row_begin = total;
        int in_run = 0, run_x = 0;
        for (int x = 0; x < w; ++x) {
            if (m[x] && !in_run) {
                in_run = 1;
                run_x = x;
                if (row_first < 0) row_first = x;
            } else if (!m[x] && in_run) {
                in_run = 0;
                row_last = x - 1;
                if (total >= cap) return -1;
                spans[total++] = (eva_span_t){ (uint16_t)run_x,
                                               (uint16_t)(x - run_x) };
            }
        }
        if (in_run) {
            row_last = w - 1;
            if (total >= cap) return -1;
            spans[total++] = (eva_span_t){ (uint16_t)run_x,
                                           (uint16_t)(w - run_x) };
        }
        if (total - row_begin > EVA_TEXT_MAX_SPANS_PER_ROW) {
            /* Too fragmented — merge the whole row into one span. */
            total = row_begin;
            spans[total++] = (eva_span_t){ (uint16_t)row_first,
                                           (uint16_t)(row_last - row_first + 1) };
        }
    }
    row_start[y1 - y0] = (uint32_t)total;
    return total;
}
