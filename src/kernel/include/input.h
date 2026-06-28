#ifndef TUNIX_INPUT_H
#define TUNIX_INPUT_H

#include <stddef.h>
#include <stdint.h>

void input_init(void);
void input_poll(void);
void input_irq(void);
void input_scancode_open(void);
void input_scancode_close(void);
int input_scancodes_ready(void);
int64_t input_read_scancodes(size_t size, void *buffer);

#endif
