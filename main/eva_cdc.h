#ifndef EVA_CDC_H
#define EVA_CDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct eva_cdc_s eva_cdc_t;

typedef void (*eva_cdc_line_fn)(void *user, char *line);
typedef void (*eva_cdc_handler_fn)(void *user, const char *args);

eva_cdc_t *eva_cdc_create(eva_cdc_line_fn line_fn, void *user);
void eva_cdc_destroy(eva_cdc_t *self);

bool eva_cdc_register(eva_cdc_t *self, const char *cmd, eva_cdc_handler_fn fn, void *user);
bool eva_cdc_dispatch(eva_cdc_t *self, char *line);

void eva_cdc_send(eva_cdc_t *self, const char *line);
void eva_cdc_sendf(eva_cdc_t *self, const char *fmt, ...);
void eva_cdc_send_binary(eva_cdc_t *self, const uint8_t *data, size_t size);
bool eva_cdc_host_open(const eva_cdc_t *self);

#endif
