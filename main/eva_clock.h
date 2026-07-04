#ifndef EVA_CLOCK_H
#define EVA_CLOCK_H

typedef struct eva_clock_s eva_clock_t;

eva_clock_t *eva_clock_create(void);
void eva_clock_destroy(eva_clock_t *self);

void eva_clock_set_hour_offset(eva_clock_t *self, int hours);

/* Sunrise/sunset in minutes-of-day (local). Drives the smooth backlight ramp:
 * 100% by day, fading to 10% over 3h after sunset, back up over the hour before
 * sunrise. Pass -1 for either to fall back to the fixed [01:00,06:00) schedule. */
void eva_clock_set_sun_times(eva_clock_t *self, int sunrise_min, int sunset_min);

void eva_clock_tick(eva_clock_t *self);

/* Last backlight percent pushed to the panel (-1 before the first apply).
 * Diagnostic for the CDC status command. */
int eva_clock_current_brightness(const eva_clock_t *self);
const char *eva_clock_text(const eva_clock_t *self);

#endif
