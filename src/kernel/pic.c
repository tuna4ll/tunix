#include <stdint.h>
#include "include/io.h"
#include "include/pic.h"

#define PIC1_COMMAND 0x20U
#define PIC1_DATA    0x21U
#define PIC2_COMMAND 0xA0U
#define PIC2_DATA    0xA1U

#define PIC_EOI      0x20U
#define PIC_ICW1_INIT 0x10U
#define PIC_ICW1_ICW4 0x01U
#define PIC_ICW4_8086 0x01U

static void pic_io_wait(void) {
    outb(0x80U, 0U);
}

void pic_init(void) {
    /* Remap the legacy PIC away from CPU exception vectors, then begin with
     * every IRQ masked. Drivers explicitly unmask the lines they own. */
    outb(PIC1_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    pic_io_wait();
    outb(PIC2_COMMAND, PIC_ICW1_INIT | PIC_ICW1_ICW4);
    pic_io_wait();

    outb(PIC1_DATA, PIC_MASTER_VECTOR);
    pic_io_wait();
    outb(PIC2_DATA, PIC_SLAVE_VECTOR);
    pic_io_wait();

    outb(PIC1_DATA, 1U << 2); /* Slave PIC is connected to master IRQ2. */
    pic_io_wait();
    outb(PIC2_DATA, 2U);
    pic_io_wait();

    outb(PIC1_DATA, PIC_ICW4_8086);
    pic_io_wait();
    outb(PIC2_DATA, PIC_ICW4_8086);
    pic_io_wait();

    outb(PIC1_DATA, 0xFFU);
    outb(PIC2_DATA, 0xFFU);
}

void pic_mask(unsigned irq) {
    if (irq >= 16U) return;
    uint16_t port = irq < 8U ? PIC1_DATA : PIC2_DATA;
    unsigned line = irq < 8U ? irq : irq - 8U;
    outb(port, (uint8_t)(inb(port) | (uint8_t)(1U << line)));
}

void pic_unmask(unsigned irq) {
    if (irq >= 16U) return;
    if (irq >= 8U) {
        outb(PIC1_DATA, (uint8_t)(inb(PIC1_DATA) & (uint8_t)~(1U << 2)));
    }
    uint16_t port = irq < 8U ? PIC1_DATA : PIC2_DATA;
    unsigned line = irq < 8U ? irq : irq - 8U;
    outb(port, (uint8_t)(inb(port) & (uint8_t)~(1U << line)));
}

void pic_send_eoi(unsigned vector) {
    if (vector >= PIC_SLAVE_VECTOR && vector < PIC_SLAVE_VECTOR + 8U)
        outb(PIC2_COMMAND, PIC_EOI);
    if (vector >= PIC_MASTER_VECTOR && vector < PIC_MASTER_VECTOR + 16U)
        outb(PIC1_COMMAND, PIC_EOI);
}
