#ifndef TUNIX_KLOG_H
#define TUNIX_KLOG_H

#include <stddef.h>
#include <stdint.h>

int64_t klog_read(uint64_t offset, size_t size, void *buffer);
int64_t klog_write(size_t size, const void *buffer);
size_t klog_size(void);

#endif
