#ifndef TUNIX_GDT_H
#define TUNIX_GDT_H

#include <stdint.h>

void gdt_init(void);
void set_kernel_stack(uint64_t stack_top);

#endif
