#ifndef TUNIX_RANDOM_H
#define TUNIX_RANDOM_H

#include <stddef.h>

void random_init(void);
void random_get_bytes(void *buffer, size_t length);
void random_mix(const void *buffer, size_t length);
int random_is_seeded(void);

#endif
