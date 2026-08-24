#include <stdint.h>
#include "../../include/gdt.h"
#include "../../include/percpu.h"

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

/*
 * A GDT and a TSS per processor.
 *
 * The descriptors are identical everywhere and could be shared, but the TSS
 * cannot be: rsp0 is the kernel stack the processor switches to when an
 * interrupt arrives from user mode, and that is whichever process *this*
 * processor is running. One shared TSS would send two processors into the same
 * kernel stack the moment they both took an interrupt. The task register also
 * marks its TSS descriptor busy, and a second `ltr` on the same descriptor
 * faults, so the descriptor has to be private as well.
 *
 * The double-fault stack is here for the same reason and travels with them.
 *
 * A double fault is what the processor raises when it cannot deliver a fault,
 * and the usual reason for that is the stack it would have to push onto. If it
 * cannot deliver the double fault either, it gives up and resets -- which
 * looks like nothing at all: no message, no register dump, a machine that
 * silently reboots. Handing vector 8 a stack of its own through the IST is
 * what turns that into a diagnosis.
 */
#define FAULT_STACK_BYTES 8192
#define FAULT_STACK_IST 1

struct cpu_tables {
    struct gdt_entry gdt[7];
    struct tss_entry tss;
    struct gdt_ptr pointer;
    uint8_t fault_stack[FAULT_STACK_BYTES] __attribute__((aligned(16)));
};

static struct cpu_tables tables[SMP_MAX_CPUS];

extern void gdt_flush(uint64_t);
extern void tss_flush(void);

void set_kernel_stack(uint64_t stack) {
    tables[cpu_current()->index].tss.rsp0 = stack;
}

static void gdt_set_gate(struct gdt_entry *gdt, int num, uint64_t base,
                         uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF);
    gdt[num].base_middle = (base >> 16) & 0xFF;
    gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF);
    gdt[num].granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[num].access = access;
}

static void gdt_set_tss(struct gdt_entry *gdt, int num, uint64_t base, uint32_t limit) {
    gdt_set_gate(gdt, num, base, limit, 0x89, 0x00);
    struct gdt_entry *tss_high = (struct gdt_entry *)&gdt[num + 1];
    uint64_t base_high = base >> 32;
    tss_high->limit_low = base_high & 0xFFFF;
    tss_high->base_low = (base_high >> 16) & 0xFFFF;
    tss_high->base_middle = 0;
    tss_high->access = 0;
    tss_high->granularity = 0;
    tss_high->base_high = 0;
}

void gdt_init_cpu(unsigned index) {
    if (index >= SMP_MAX_CPUS) return;
    struct cpu_tables *self = &tables[index];
    struct gdt_entry *gdt = self->gdt;

    self->pointer.limit = (sizeof(struct gdt_entry) * 7) - 1;
    self->pointer.base = (uint64_t)gdt;

    gdt_set_gate(gdt, 0, 0, 0, 0, 0);
    gdt_set_gate(gdt, 1, 0, 0xFFFFF, 0x9A, 0x20);
    gdt_set_gate(gdt, 2, 0, 0xFFFFF, 0x92, 0x00);
    gdt_set_gate(gdt, 3, 0, 0xFFFFF, 0xF2, 0x00);
    gdt_set_gate(gdt, 4, 0, 0xFFFFF, 0xFA, 0x20);

    __builtin_memset(&self->tss, 0, sizeof(self->tss));
    self->tss.iopb_offset = sizeof(self->tss);
    self->tss.ist[FAULT_STACK_IST - 1] =
        (uint64_t)(self->fault_stack + FAULT_STACK_BYTES);
    /* The limit covers the TSS only: the fault stack sits after it in this
       struct and must not be inside the segment the processor is told about. */
    gdt_set_tss(gdt, 5, (uint64_t)&self->tss, sizeof(self->tss) - 1);

    gdt_flush((uint64_t)&self->pointer);
    tss_flush();
    /* After the flush, not before: a segment load clears the GS base. */
    percpu_activate(index);
}

void gdt_init(void) {
    gdt_init_cpu(0);
}
