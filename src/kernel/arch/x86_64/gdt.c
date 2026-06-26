#include <stdint.h>
#include "../../include/gdt.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed));

struct tss_entry {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct gdt_entry gdt[7];
static struct tss_entry tss;
static struct gdt_ptr gp;

extern void gdt_flush(uint64_t);
extern void tss_flush(void);

void set_kernel_stack(uint64_t stack) {
    tss.rsp0 = stack;
}

static void gdt_set_gate(int num, uint64_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access = access;
}

static void gdt_set_tss(int num, uint64_t base, uint32_t limit) {
    gdt_set_gate(num, base, limit, 0x89, 0x00);
    struct gdt_entry *tss_high = (struct gdt_entry *)&gdt[num + 1];
    uint64_t base_high = base >> 32;
    tss_high->limit_low = base_high & 0xFFFF;
    tss_high->base_low = (base_high >> 16) & 0xFFFF;
    tss_high->base_middle = 0;
    tss_high->access = 0;
    tss_high->granularity = 0;
    tss_high->base_high = 0;
}

void gdt_init(void) {
    gp.limit = (sizeof(struct gdt_entry) * 7) - 1;
    gp.base = (uint64_t)&gdt;

    gdt_set_gate(0, 0, 0, 0, 0);
    gdt_set_gate(1, 0, 0xFFFFF, 0x9A, 0x20);
    gdt_set_gate(2, 0, 0xFFFFF, 0x92, 0x00);
    gdt_set_gate(3, 0, 0xFFFFF, 0xF2, 0x00);
    gdt_set_gate(4, 0, 0xFFFFF, 0xFA, 0x20);

    uint64_t tss_base = (uint64_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;
    
    __builtin_memset(&tss, 0, sizeof(tss));
    tss.iopb_offset = sizeof(tss);
    
    gdt_set_tss(5, tss_base, tss_limit);

    gdt_flush((uint64_t)&gp);
    tss_flush();
}
