#ifndef TUNIX_OOPS_H
#define TUNIX_OOPS_H

#include <stdint.h>

unsigned oops_begin(void);
int oops_in_progress(void);
void oops_end(void);
void oops_print_hex(uint64_t value);
void oops_print_hex_width(uint64_t value, unsigned digits);
void oops_print_symbol(uint64_t address);
void oops_print_task(void);
void oops_print_trace(uint64_t frame, uint64_t stack_pointer);
void oops_report(const char *headline);
void dump_stack(void);
void warn_slowpath(const char *file, int line);

#define WARN_HERE() warn_slowpath(__FILE__, __LINE__)

#endif
