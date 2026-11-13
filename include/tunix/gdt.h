#ifndef TUNIX_GDT_H
#define TUNIX_GDT_H

#include <stdint.h>

void gdt_init(void);

void gdt_init_cpu(unsigned index);
int gdt_prepare_cpu(unsigned index);
void set_kernel_stack(uint64_t stack_top);

#endif
