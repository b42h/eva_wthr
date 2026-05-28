#ifndef EVA_SETTINGS_H
#define EVA_SETTINGS_H

#include <stddef.h>

typedef struct eva_settings_s eva_settings_t;

eva_settings_t *eva_settings_create(void);
void eva_settings_destroy(eva_settings_t *self);

void eva_settings_load(eva_settings_t *self);
void eva_settings_save(const eva_settings_t *self);

const char *eva_settings_get_tz(const eva_settings_t *self);
void eva_settings_set_tz(eva_settings_t *self, const char *tz);
size_t eva_settings_tz_capacity(const eva_settings_t *self);

#endif
