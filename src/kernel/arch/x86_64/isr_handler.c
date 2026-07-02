#include <stdint.h>
#include "../../include/input.h"
#include "../../include/interrupt.h"
#include "../../include/pic.h"
#include "../../include/timer.h"

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

const char *exception_messages[] = {
    "Division By Zero", "Debug", "Non Maskable Interrupt", "Breakpoint",
    "Into Detected Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor",
    "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present",
    "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt",
    "Coprocessor Fault", "Alignment Check", "Machine Check", "SIMD Floating-Point",
    "Virtualization", "Control Protection", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor Injection", "VMM Communication", "Security", "Reserved"
};

void isr_handler(struct interrupt_frame *regs) {
    if (regs->int_no == PIC_MASTER_VECTOR) {
        timer_irq(regs);
        pic_send_eoi((unsigned)regs->int_no);
        return;
    }
    if (regs->int_no == PIC_MASTER_VECTOR + 1U) {
        input_irq();
        pic_send_eoi((unsigned)regs->int_no);
        return;
    }
    kprintf("Received interrupt: %d\n", regs->int_no);
    if (regs->int_no < 32) {
        kprintf("Exception: %s\n", exception_messages[regs->int_no]);
        kprintf("Error Code: %x\n", regs->err_code);
        kprintf("RIP: %p\n", (void*)regs->rip);
        panic(exception_messages[regs->int_no]);
    }
}
