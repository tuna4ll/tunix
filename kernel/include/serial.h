#ifndef TUNIX_SERIAL_H
#define TUNIX_SERIAL_H

#include <stdint.h>

void serial_init(void);
void serial_attach_ns16550(uint64_t virtual_base, unsigned shift, unsigned width);
void serial_attach_pl011(uint64_t virtual_base);
int serial_present(void);
void serial_write_char(char c);
int serial_read_char(void);
unsigned serial_read_limit(void);

#endif
