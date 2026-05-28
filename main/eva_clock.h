#ifndef EVA_CLOCK_H
#define EVA_CLOCK_H

typedef struct eva_clock_s eva_clock_t;

eva_clock_t *eva_clock_create(void);
void eva_clock_destroy(eva_clock_t *self);

void eva_clock_set_hour_offset(eva_clock_t *self, int hours);
void eva_clock_tick(eva_clock_t *self);
const char *eva_clock_text(const eva_clock_t *self);

#endif
