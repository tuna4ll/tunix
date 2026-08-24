#ifndef TUNIX_GDT_H
#define TUNIX_GDT_H

#include <stdint.h>

void gdt_init(void);
/* Build and load the tables belonging to one processor, and point its GS at
   its own block. Every processor calls this once, on itself. */
void gdt_init_cpu(unsigned index);
void set_kernel_stack(uint64_t stack_top);

#endif
