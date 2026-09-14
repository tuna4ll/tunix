#ifndef TUNIX_RANDOM_H
#define TUNIX_RANDOM_H

#include <stddef.h>
#include <stdint.h>

void random_init(void);
void random_get_bytes(void *buffer, size_t length);
void random_mix(const void *buffer, size_t length);
int random_is_seeded(void);

size_t arch_entropy_collect(uint64_t *values, size_t room);
uint64_t arch_entropy_noise(void);

#endif
