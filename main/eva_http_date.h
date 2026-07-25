#pragma once

/* Parse RFC 1123 Date headers ("Thu, 21 May 2026 14:30:25 GMT") to UTC
 * epoch seconds without touching the process TZ (no setenv). Shared by
 * eva_wifi.c and host tests. */

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Days from civil (y, m, d) → days since 1970-01-01. Algorithm from
 * Howard Hinnant (public domain). y is full year; m is 1..12. */
static inline int64_t eva_days_from_civil(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3u : 9u)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

/* Convert a broken-down UTC tm to epoch seconds. Does not call mktime /
 * timegm and does not mutate TZ. tm_year is years since 1900, tm_mon 0..11. */
static inline time_t eva_timegm(const struct tm *tm)
{
    int64_t days = eva_days_from_civil(tm->tm_year + 1900,
                                       (unsigned)tm->tm_mon + 1,
                                       (unsigned)tm->tm_mday);
    int64_t secs = days * 86400LL
                 + (int64_t)tm->tm_hour * 3600
                 + (int64_t)tm->tm_min * 60
                 + (int64_t)tm->tm_sec;
    return (time_t)secs;
}

static inline bool eva_parse_http_date(const char *hdr, time_t *out)
{
    if (!hdr || !out) return false;
    struct tm tm = {0};
    char *end = strptime(hdr, "%a, %d %b %Y %H:%M:%S", &tm);
    if (!end) return false;
    time_t t = eva_timegm(&tm);
    if (t == (time_t)-1) return false;
    *out = t;
    return true;
}

#ifdef __cplusplus
}
#endif
