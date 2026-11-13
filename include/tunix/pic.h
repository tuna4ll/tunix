#ifndef TUNIX_PIC_H
#define TUNIX_PIC_H

#include <stdint.h>

#define PIC_MASTER_VECTOR 0x20U
#define PIC_SLAVE_VECTOR  0x28U

void pic_init(void);
void pic_mask(unsigned irq);
void pic_unmask(unsigned irq);
void pic_send_eoi(unsigned vector);

#endif
