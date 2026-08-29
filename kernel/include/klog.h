#ifndef TUNIX_KLOG_H
#define TUNIX_KLOG_H

#include <stddef.h>
#include <stdint.h>

int64_t klog_read(uint64_t offset, size_t size, void *buffer);
int64_t klog_write(size_t size, const void *buffer);
size_t klog_size(void);
/* The last `lines` lines of the log, written to the terminal. panic() uses it:
   see kprintf.c. */
void klog_print_tail(unsigned lines);
/* Whether the log is echoed to the console as it is written. On by default;
   see kprintf.c for why. */
void klog_console(int enabled);

#endif
