#include "weather_fetch_clearoutside.h"
#include "weather_provider.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "eva_weather.h"

/* clearoutside.com gives a hourly forecast with 3 cloud layers (low/medium/
 * high %), precipitation type and amount, wind, temperature, fog %,
 * visibility, sunrise/sunset, and moon phase. The page is plain HTML over
 * HTTP — no TLS needed.
 *
 * The HTML is regular enough that strstr-based extraction is reliable:
 *
 *   <div class="fc_day" id="day_0">      <- today
 *     ...
 *     <span class="fc_detail_label"><span>LABEL</span></span>
 *     <div class="fc_hours">
 *       <ul> <li ...>VAL_h0</li> <li ...>VAL_h1</li> ... </ul>
 *     </div>
 *
 * We always read the first <li> (h0) which corresponds to the current hour
 * because the page auto-centres on the request time. */

#define WEATHER_URL "http://clearoutside.com/forecast/" CONFIG_EVA_WEATHER_LATITUDE "/" CONFIG_EVA_WEATHER_LONGITUDE
#define WEATHER_REFRESH_MS (13 * 60 * 1000)
#define WEATHER_RETRY_MS   (60 * 1000)
#define WEATHER_FIRST_FETCH_DELAY_MS (15 * 1000)
#define WEATHER_HTTP_MAX_BYTES (256 * 1024)

static const char *TAG = "weather_clearoutside";

static char *http_get_body(size_t *out_len)
{
    esp_http_client_config_t cfg = {
        .url = WEATHER_URL,
        .timeout_ms = 15000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        /* clearoutside seems to refuse the default no-UA request occasionally
         * via Cloudflare; identify ourselves so we look like a real browser. */
        .user_agent = "EvaWeather/1.0 (ESP32-P4; +https://github.com)",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return NULL;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return NULL;
    }

    int content_len = esp_http_client_fetch_headers(client);
    if (content_len > WEATHER_HTTP_MAX_BYTES) {
        ESP_LOGW(TAG, "body too large: %d", content_len);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    char *body = heap_caps_malloc(WEATHER_HTTP_MAX_BYTES + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return NULL;
    }

    int total = 0;
    int read = 0;
    while (total < WEATHER_HTTP_MAX_BYTES &&
           (read = esp_http_client_read(client, body + total,
                                        WEATHER_HTTP_MAX_BYTES - total)) > 0) {
        total += read;
    }
    body[total > 0 ? total : 0] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (total <= 0) {
        free(body);
        return NULL;
    }
    if (out_len) *out_len = (size_t)total;
    return body;
}

/* --- HTML scrapers -------------------------------------------------------- */

/* Find the day_0 forecast block. Returns pointer to start of that div,
 * or NULL if not found. */
static const char *find_day0(const char *body)
{
    return strstr(body, "id=\"day_0\"");
}

/* Same but stop at the next day_N block to avoid leaking into tomorrow. */
static const char *day0_end(const char *day0_start)
{
    if (!day0_start) return NULL;
    const char *next_day = strstr(day0_start + 1, "id=\"day_");
    return next_day ? next_day : (day0_start + strlen(day0_start));
}

/* Locate the <ul> following a fc_detail_label that contains `label` text.
 * Search window restricted to [start, end). Returns pointer to '<' of '<li'
 * inside that ul, or NULL. */
static const char *find_label_first_li(const char *start, const char *end,
                                        const char *label)
{
    char anchor[96];
    snprintf(anchor, sizeof(anchor), "<span>%s", label);
    const char *p = start;
    while (p && p < end) {
        const char *m = strstr(p, anchor);
        if (!m || m >= end) return NULL;
        const char *ul = strstr(m, "<ul>");
        if (!ul || ul >= end) return NULL;
        const char *li = strstr(ul, "<li");
        if (!li || li >= end) return NULL;
        return li;
    }
    return NULL;
}

/* Read integer between '>' and '</' inside a <li>...</li>. Returns INT_MIN
 * sentinel on parse failure. */
#define LI_BAD INT32_MIN
static int32_t li_read_int(const char *li_start)
{
    if (!li_start) return LI_BAD;
    const char *gt = strchr(li_start, '>');
    if (!gt) return LI_BAD;
    gt++;
    /* Some li wrap the value in <span>: skip past that. */
    if (*gt == '<') {
        const char *gt2 = strchr(gt, '>');
        if (!gt2) return LI_BAD;
        gt = gt2 + 1;
    }
    /* Allow leading sign + integer or decimal (we keep just the integer part). */
    while (*gt == ' ') gt++;
    bool neg = false;
    if (*gt == '-') { neg = true; gt++; }
    if (!isdigit((unsigned char)*gt)) return LI_BAD;
    int32_t v = 0;
    while (isdigit((unsigned char)*gt)) {
        v = v * 10 + (*gt - '0');
        gt++;
    }
    return neg ? -v : v;
}

/* Read first decimal as fixed-point milli (mm). Returns -1 on failure. */
static int32_t li_read_decimal_x10(const char *li_start)
{
    if (!li_start) return -1;
    const char *gt = strchr(li_start, '>');
    if (!gt) return -1;
    gt++;
    if (*gt == '<') {
        const char *gt2 = strchr(gt, '>');
        if (!gt2) return -1;
        gt = gt2 + 1;
    }
    while (*gt == ' ') gt++;
    int32_t whole = 0;
    while (isdigit((unsigned char)*gt)) {
        whole = whole * 10 + (*gt - '0');
        gt++;
    }
    int frac = 0;
    if (*gt == '.') {
        gt++;
        if (isdigit((unsigned char)*gt)) {
            frac = *gt - '0';
        }
    }
    return whole * 10 + frac;
}

/* Read attribute value, e.g. extract "Light Rain" from
 * <li ... title="Light Rain">. Buffer is filled (NUL-terminated). */
static bool li_read_attr(const char *li_start, const char *attr, char *out, size_t out_sz)
{
    if (!li_start || !out || out_sz == 0) return false;
    out[0] = '\0';
    /* Scan only within this single <li ...>. */
    const char *gt = strchr(li_start, '>');
    if (!gt) return false;
    char needle[24];
    snprintf(needle, sizeof(needle), " %s=\"", attr);
    const char *m = strstr(li_start, needle);
    if (!m || m >= gt) return false;
    m += strlen(needle);
    size_t i = 0;
    while (*m && *m != '"' && i + 1 < out_sz) {
        out[i++] = *m++;
    }
    out[i] = '\0';
    return i > 0;
}

/* Parse precipitation type from clearoutside's title="..." or text content. */
static precip_type_t map_precip(const char *s)
{
    if (!s || !*s || strcmp(s, "None") == 0) return PRECIP_NONE;
    if (strstr(s, "Heavy Rain"))       return PRECIP_HEAVY_RAIN;
    if (strstr(s, "Very Light Rain"))  return PRECIP_DRIZZLE;
    if (strstr(s, "Light Rain"))       return PRECIP_LIGHT_RAIN;
    if (strstr(s, "Rain"))             return PRECIP_RAIN;
    if (strstr(s, "Snow"))             return PRECIP_SNOW;
    if (strstr(s, "Sleet"))            return PRECIP_SLEET;
    if (strstr(s, "Hail"))             return PRECIP_HAIL;
    if (strstr(s, "Thunder"))          return PRECIP_THUNDER;
    return PRECIP_NONE;
}

/* "16mph from the North-North-West (336&deg;)" -> kph=26, deg=336 */
static void parse_wind_title(const char *title, int *out_kph, int *out_deg)
{
    *out_kph = -1;
    *out_deg = -1;
    if (!title || !*title) return;
    int mph = 0;
    if (sscanf(title, "%dmph", &mph) == 1) {
        /* mph -> kph rounded. */
        *out_kph = (int)((mph * 1609 + 500) / 1000);
    }
    /* Degrees in trailing "(NNN&deg;)" */
    const char *lp = strchr(title, '(');
    if (lp) {
        int deg = 0;
        if (sscanf(lp, "(%d", &deg) == 1) *out_deg = deg;
    }
}

/* "Sun - Rise: 05:30, Set: 21:04, ..."  ->  sunrise/sunset minutes.
 * The same fc_daylight span also carries
 *   "Moon - Rise: 11:20, Set: 01:54."
 * which is parsed into moonrise/moonset. Set < rise is legal here — the
 * moon arc straddles midnight; the canvas handles the wrap by adding 1440
 * to set when set < rise. */
static void parse_astronomy_text(const char *day0_start, const char *day0_end_p,
                                 int16_t *out_sr, int16_t *out_ss,
                                 int16_t *out_mr, int16_t *out_ms)
{
    *out_sr = -1; *out_ss = -1;
    *out_mr = -1; *out_ms = -1;
    if (!day0_start) return;

    const char *sun = strstr(day0_start, "Sun - Rise:");
    if (sun && sun < day0_end_p) {
        int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
        if (sscanf(sun, "Sun - Rise: %d:%d, Set: %d:%d", &h1, &m1, &h2, &m2) == 4) {
            if (h1 >= 0 && h1 <= 23 && m1 >= 0 && m1 <= 59) *out_sr = (int16_t)(h1 * 60 + m1);
            if (h2 >= 0 && h2 <= 23 && m2 >= 0 && m2 <= 59) *out_ss = (int16_t)(h2 * 60 + m2);
        }
    }

    const char *moon = strstr(day0_start, "Moon - Rise:");
    if (moon && moon < day0_end_p) {
        int h1 = 0, m1 = 0, h2 = 0, m2 = 0;
        if (sscanf(moon, "Moon - Rise: %d:%d, Set: %d:%d", &h1, &m1, &h2, &m2) == 4) {
            if (h1 >= 0 && h1 <= 23 && m1 >= 0 && m1 <= 59) *out_mr = (int16_t)(h1 * 60 + m1);
            if (h2 >= 0 && h2 <= 23 && m2 >= 0 && m2 <= 59) *out_ms = (int16_t)(h2 * 60 + m2);
        }
    }
}

/* Moon: <span class="fc_moon_phase">Waxing Crescent</span>
 *       <span class="fc_moon_percentage">30%</span> */
static void parse_moon(const char *day0_start, const char *day0_end_p,
                       uint8_t *out_pct, uint8_t *out_waning)
{
    *out_pct = 0; *out_waning = 0;
    if (!day0_start) return;
    const char *p = strstr(day0_start, "fc_moon_phase\">");
    if (!p || p >= day0_end_p) return;
    p += strlen("fc_moon_phase\">");
    char phase[40] = {0};
    size_t i = 0;
    while (*p && *p != '<' && i + 1 < sizeof(phase)) phase[i++] = *p++;
    phase[i] = '\0';
    /* clearoutside uses labels like "Waxing Crescent", "First Quarter",
     * "Waning Gibbous", and "Last Quarter". Treat anything explicitly
     * waning, plus the last-quarter aliases, as the darkening half of the
     * cycle so the renderer can place the lit side correctly. */
    *out_waning = (strstr(phase, "Waning") != NULL ||
                   strstr(phase, "Last Quarter") != NULL ||
                   strstr(phase, "Third Quarter") != NULL) ? 1 : 0;
    const char *q = strstr(day0_start, "fc_moon_percentage\">");
    if (q && q < day0_end_p) {
        q += strlen("fc_moon_percentage\">");
        int pct = 0;
        if (sscanf(q, "%d", &pct) == 1) {
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            *out_pct = (uint8_t)pct;
        }
    }
}

static const char *uk_for_precip(precip_type_t p)
{
    switch (p) {
    case PRECIP_NONE:        return NULL;
    case PRECIP_DRIZZLE:     return "Мряка";
    case PRECIP_LIGHT_RAIN:  return "Легкий дощ";
    case PRECIP_RAIN:        return "Дощ";
    case PRECIP_HEAVY_RAIN:  return "Злива";
    case PRECIP_SNOW:        return "Сніг";
    case PRECIP_SLEET:       return "Мокрий сніг";
    case PRECIP_HAIL:        return "Град";
    case PRECIP_THUNDER:     return "Гроза";
    default:                 return NULL;
    }
}

static const char *uk_for_clouds(uint8_t total_pct, bool is_night)
{
    if (total_pct >= 75) return "Хмарно";
    if (total_pct >= 55) return "Мінлива хмарність";
    return is_night ? "Ясна ніч" : "Ясно";
}

/* --- top-level scrape ---------------------------------------------------- */

static bool scrape(const char *body, weather_state_t *out)
{
    const char *day = find_day0(body);
    if (!day) {
        ESP_LOGW(TAG, "day_0 block not found");
        return false;
    }
    const char *end = day0_end(day);

    memset(out, 0, sizeof(*out));
    out->kind = WEATHER_UNKNOWN;
    out->wind_kph = -1;
    out->wind_dir_deg = -1;
    out->sunrise_min = -1;
    out->sunset_min = -1;
    out->moonrise_min = -1;
    out->moonset_min = -1;
    out->precip_type = PRECIP_NONE;

    int32_t v;

    v = li_read_int(find_label_first_li(day, end, "Total Clouds"));
    if (v >= 0 && v <= 100) out->cloud_total_pct = (uint8_t)v;

    v = li_read_int(find_label_first_li(day, end, "Low Clouds"));
    if (v >= 0 && v <= 100) out->cloud_low_pct = (uint8_t)v;

    v = li_read_int(find_label_first_li(day, end, "Medium Clouds"));
    if (v >= 0 && v <= 100) out->cloud_mid_pct = (uint8_t)v;

    v = li_read_int(find_label_first_li(day, end, "High Clouds"));
    if (v >= 0 && v <= 100) out->cloud_high_pct = (uint8_t)v;

    v = li_read_int(find_label_first_li(day, end, "Fog (%)"));
    if (v >= 0 && v <= 100) out->fog_pct = (uint8_t)v;

    /* Visibility comes in miles; clearoutside reports an integer 0..40 ish.
     * Convert to deci-km: km = miles * 1.609, then *10. Cap at 255. */
    v = li_read_int(find_label_first_li(day, end, "Visibility"));
    if (v >= 0) {
        int dk = (v * 1609 + 50) / 100;   /* miles -> deci-km */
        if (dk > 255) dk = 255;
        out->visibility_km_x10 = (uint8_t)dk;
    }

    v = li_read_int(find_label_first_li(day, end, "Temperature"));
    if (v != LI_BAD) out->temp_c = (int8_t)v;

    v = li_read_int(find_label_first_li(day, end, "Feels Like"));
    if (v != LI_BAD) out->feels_like_c = (int8_t)v;

    int32_t mmx10 = li_read_decimal_x10(
        find_label_first_li(day, end, "Precipitation Amount"));
    if (mmx10 >= 0 && mmx10 < 65535) out->precip_mm_x10 = (uint16_t)mmx10;

    /* Precipitation Type: the first <li> has class="climacon XXX" + title="..".
     * Prefer title text, fall back to span content. */
    const char *p_li = find_label_first_li(day, end, "Precipitation Type");
    if (p_li) {
        char title[40] = {0};
        if (li_read_attr(p_li, "title", title, sizeof(title))) {
            out->precip_type = map_precip(title);
        }
    }

    /* Wind: title attr has "16mph from the West (270&deg;)" */
    const char *w_li = find_label_first_li(day, end, "Wind Speed/Direction");
    if (w_li) {
        char title[80] = {0};
        if (li_read_attr(w_li, "title", title, sizeof(title))) {
            int kph = -1, deg = -1;
            parse_wind_title(title, &kph, &deg);
            out->wind_kph = (int16_t)kph;
            out->wind_dir_deg = (int16_t)deg;
        }
    }

    parse_astronomy_text(day, end,
                         &out->sunrise_min, &out->sunset_min,
                         &out->moonrise_min, &out->moonset_min);
    parse_moon(day, end, &out->moon_phase_pct, &out->moon_waning);

    out->fetched_at = time(NULL);

    /* Compose desc (UK). Precipitation overrides cloud description. */
    bool is_night = true;
    if (out->sunrise_min >= 0 && out->sunset_min >= 0) {
        time_t now = time(NULL);
        struct tm tm_now = {0};
        if (now > 1700000000) {
            localtime_r(&now, &tm_now);
            int m = tm_now.tm_hour * 60 + tm_now.tm_min;
            is_night = (m < out->sunrise_min || m >= out->sunset_min);
        }
    }
    /* Thunderstorm synthesis is centralised in eva_weather_set() so it
     * applies uniformly to live fetches, CDC weatherraw overrides, and any
     * future input source. */
    const char *uk = uk_for_precip(out->precip_type);
    if (!uk) {
        if (out->fog_pct >= 60) uk = "Туман";
        else uk = uk_for_clouds(out->cloud_total_pct, is_night);
    }
    strlcpy(out->desc, uk, sizeof(out->desc));

    /* Sanity: if we got nothing, fail loudly so the caller can retry. */
    if (out->cloud_total_pct == 0 && out->cloud_low_pct == 0 &&
        out->cloud_mid_pct == 0 && out->cloud_high_pct == 0 &&
        out->temp_c == 0 && out->precip_type == PRECIP_NONE) {
        ESP_LOGW(TAG, "scrape returned all-zero — labels probably moved");
        return false;
    }
    return true;
}

/* HTTP-download + scrape into a caller-supplied weather_state_t. No global
 * state writes — the coordinator (weather_fetch.c) is responsible for
 * publishing into eva_weather_set(). */
static bool fetch_into(weather_state_t *out)
{
    size_t len = 0;
    char *body = http_get_body(&len);
    if (!body) return false;
    ESP_LOGI(TAG, "downloaded %u bytes", (unsigned)len);

    bool ok = scrape(body, out);
    free(body);
    return ok;
}

/* Provider entry point — wraps the existing HTML scrape + HTTP fetch in
 * the weather_partial_t contract. The coordinator (weather_fetch.c) calls
 * this on its clearoutside timer. */
esp_err_t weather_provider_clearoutside_fetch(weather_partial_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    out->weather_code = -1;
    out->sunrise_min = -1;
    out->sunset_min = -1;
    out->moonrise_min = -1;
    out->moonset_min = -1;

    weather_state_t legacy = {0};
    if (!fetch_into(&legacy)) return ESP_FAIL;

    /* Translate legacy weather_state_t into weather_partial_t. */
    out->has_clouds = true;
    out->cloud_low_pct   = legacy.cloud_low_pct;
    out->cloud_mid_pct   = legacy.cloud_mid_pct;
    out->cloud_high_pct  = legacy.cloud_high_pct;
    out->cloud_cover_pct = legacy.cloud_total_pct;

    out->has_temp = true;
    out->temp_c = legacy.temp_c;
    out->feels_like_c = legacy.feels_like_c;

    if (legacy.wind_kph >= 0) {
        out->has_wind = true;
        out->wind_kph = (uint16_t)legacy.wind_kph;
        out->wind_dir_deg = (legacy.wind_dir_deg >= 0)
                          ? (uint16_t)legacy.wind_dir_deg : 0;
    }

    out->has_precip = true;
    out->precip = legacy.precip_type;
    out->precip_mm_x10 = legacy.precip_mm_x10;

    if (legacy.sunrise_min >= 0 && legacy.sunset_min >= 0) {
        out->has_sun = true;
        out->sunrise_min = legacy.sunrise_min;
        out->sunset_min = legacy.sunset_min;
    }

    if (legacy.moonrise_min >= 0 && legacy.moonset_min >= 0) {
        out->has_moon = true;
        out->moonrise_min = legacy.moonrise_min;
        out->moonset_min = legacy.moonset_min;
        out->moon_phase_pct = legacy.moon_phase_pct;
        out->moon_waning = legacy.moon_waning;
    }

    out->has_visibility = true;
    out->visibility_km_x10 = legacy.visibility_km_x10;
    out->fog_pct = legacy.fog_pct;

    return ESP_OK;
}
