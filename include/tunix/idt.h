#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void idt_init(void);
void idt_activate(void);
void idt_set_handler(uint8_t vector, void (*handler)(void));

#endif
