#include <stdint.h>

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

struct registers {
    uint64_t ds;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

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

void isr_handler(struct registers *regs) {
    kprintf("Received interrupt: %d\n", regs->int_no);
    if (regs->int_no < 32) {
        kprintf("Exception: %s\n", exception_messages[regs->int_no]);
        kprintf("Error Code: %x\n", regs->err_code);
        kprintf("RIP: %p\n", (void*)regs->rip);
        panic(exception_messages[regs->int_no]);
    }
}
