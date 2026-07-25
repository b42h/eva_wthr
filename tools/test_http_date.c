/* Host test: RFC 1123 HTTP Date → UTC epoch (eva_http_date.h).
 * Build: cc -std=c11 -Wall -Wextra -I main -o /tmp/test_http_date tools/test_http_date.c && /tmp/test_http_date
 */
#define _XOPEN_SOURCE 700
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "eva_http_date.h"

int main(void)
{
    time_t t = 0;

    /* Known pair: Thu, 21 May 2026 14:30:25 GMT → 1779373825
     * Verified via: date -u -j -f "%Y-%m-%d %H:%M:%S" "2026-05-21 14:30:25" +%s  (BSD)
     *            or date -u -d "2026-05-21 14:30:25" +%s                       (GNU) */
    assert(eva_parse_http_date("Thu, 21 May 2026 14:30:25 GMT", &t));
    assert(t == (time_t)1779373825);

    /* Epoch day boundary */
    assert(eva_parse_http_date("Thu, 01 Jan 1970 00:00:00 GMT", &t));
    assert(t == (time_t)0);

    /* Leap day */
    assert(eva_parse_http_date("Tue, 29 Feb 2000 12:00:00 GMT", &t));
    assert(t == (time_t)951825600);

    /* Reject garbage */
    assert(!eva_parse_http_date("not a date", &t));
    assert(!eva_parse_http_date(NULL, &t));
    assert(!eva_parse_http_date("Thu, 21 May 2026 14:30:25 GMT", NULL));

    /* Round-trip via eva_timegm */
    struct tm tm = { .tm_year = 126, .tm_mon = 4, .tm_mday = 21,
                     .tm_hour = 14, .tm_min = 30, .tm_sec = 25 };
    assert(eva_timegm(&tm) == (time_t)1779373825);

    printf("ok\n");
    return 0;
}
