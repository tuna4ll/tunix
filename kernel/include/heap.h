#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

void heap_init(void);
void* kmalloc(size_t size);
void kfree(void* ptr);

/* True once the heap is close enough to its ceiling that the next large
   allocation is likely to fail. */
int heap_under_pressure(void);

/*
 * What the kernel's own allocator is holding, in bytes.
 *
 * Worth reporting because it is a second ceiling, and the one that bites
 * first: file contents live in kmalloc'd buffers, so a program that has read
 * a lot can exhaust the heap while the physical allocator still has hundreds
 * of megabytes free. Without this, that failure looks like "out of memory" on
 * a machine whose /proc/meminfo says there is plenty.
 */
void heap_stats(uint64_t *reserved, uint64_t *allocated, uint64_t *limit);

#endif
