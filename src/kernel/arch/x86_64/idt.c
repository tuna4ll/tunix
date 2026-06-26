#include <stdint.h>
#include "../../include/idt.h"

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t ist;
    uint8_t flags;
    uint16_t base_mid;
    uint32_t base_high;
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void idt_load(uint64_t);

static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].base_mid = (base >> 16) & 0xFFFF;
    idt[num].base_high = (base >> 32) & 0xFFFFFFFF;
    idt[num].sel = sel;
    idt[num].ist = ist;
    idt[num].flags = flags;
    idt[num].reserved = 0;
}

extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

void idt_init(void) {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1;
    idtp.base = (uint64_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0, 0);
    }

    idt_set_gate(0, (uint64_t)isr0, 0x08, 0x8E, 0);
    idt_set_gate(1, (uint64_t)isr1, 0x08, 0x8E, 0);
    idt_set_gate(2, (uint64_t)isr2, 0x08, 0x8E, 0);
    idt_set_gate(3, (uint64_t)isr3, 0x08, 0x8E, 0);
    idt_set_gate(4, (uint64_t)isr4, 0x08, 0x8E, 0);
    idt_set_gate(5, (uint64_t)isr5, 0x08, 0x8E, 0);
    idt_set_gate(6, (uint64_t)isr6, 0x08, 0x8E, 0);
    idt_set_gate(7, (uint64_t)isr7, 0x08, 0x8E, 0);
    idt_set_gate(8, (uint64_t)isr8, 0x08, 0x8E, 0);
    idt_set_gate(9, (uint64_t)isr9, 0x08, 0x8E, 0);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E, 0);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E, 0);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E, 0);
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E, 0);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E, 0);
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E, 0);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E, 0);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E, 0);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E, 0);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E, 0);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E, 0);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E, 0);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E, 0);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E, 0);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E, 0);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E, 0);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E, 0);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E, 0);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E, 0);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E, 0);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E, 0);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E, 0);

    idt_load((uint64_t)&idtp);
}
