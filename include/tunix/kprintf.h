#ifndef TUNIX_KPRINTF_H
#define TUNIX_KPRINTF_H

void kprintf(const char *fmt, ...);
void panic(const char *msg) __attribute__((noreturn));

#endif
