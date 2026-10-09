#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

void heap_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);

int heap_under_pressure(void);

void heap_stats(uint64_t *reserved, uint64_t *allocated, uint64_t *limit);

#endif
