#include <stdint.h>
#include "../../include/acpi.h"
#include "../../include/input.h"
#include "../../include/power.h"
#include "../../include/interrupt.h"
#include "../../include/klock.h"
#include "../../include/percpu.h"
#include "../../include/pic.h"
#include "../../include/apic.h"
#include "../../include/process.h"
#include "../../include/signal.h"
#include "../../include/smp.h"
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

/* Vector to signal, following the usual Unix mapping. */
static int fault_signal(uint64_t vector) {
    switch (vector) {
        case 0:  return SIGFPE;   /* divide by zero */
        case 6:  return SIGILL;   /* invalid opcode */
        case 16:
        case 19: return SIGFPE;   /* x87 / SIMD floating point */
        case 17: return SIGBUS;   /* alignment check */
        default: return SIGSEGV;  /* page fault, GP fault, everything else */
    }
}

/* Whoever is delivering is who must be told the interrupt is finished. The
   two are never both live: apic_init masks the 8259s as it takes over. */
static void interrupt_acknowledge(unsigned vector) {
    if (apic_is_active()) apic_send_eoi();
    else pic_send_eoi(vector);
}

static void isr_dispatch(struct interrupt_frame *regs) {
    /* Acknowledged before it is handled, not after: a tick that ends up
       parking the processor -- the last process on it exited, say -- never
       comes back here, and a controller still waiting to be told the last
       interrupt finished will not send another. Interrupts are off throughout,
       so nothing can arrive in the gap this opens. */
    if (regs->int_no == PIC_MASTER_VECTOR) {
        interrupt_acknowledge((unsigned)regs->int_no);
        timer_irq(regs);
        return;
    }
    if (regs->int_no == SMP_TIMER_VECTOR) {
        apic_send_eoi();
        process_timer_interrupt(regs);
        return;
    }
    if (regs->int_no == PIC_MASTER_VECTOR + 1U ||
        regs->int_no == PIC_SLAVE_VECTOR + 4U) {
        interrupt_acknowledge((unsigned)regs->int_no);
        input_irq();
        return;
    }
    /* The SCI. Acknowledged before it is acted on, like the tick above and for
       the same reason: acting on it does not come back here. The controller is
       always the APIC, because the SCI is only ever routed once the IOAPIC has
       taken over. */
    if (regs->int_no == ACPI_SCI_VECTOR) {
        apic_send_eoi();
        if (acpi_sci_interrupt()) power_button_pressed();
        return;
    }
    if (regs->int_no < 32) {
        /* Capture before handling: terminating the faulting process switches
           context and overwrites regs with the next process's state, so
           reading afterwards would report the wrong RIP. */
        uint64_t fault_rip = regs->rip;
        uint64_t fault_error = regs->err_code;
        uint64_t fault_cs = regs->cs;
        uint64_t fault_address = 0;
        if (regs->int_no == 14) /* page fault: CR2 holds the bad address */
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));

        /* A not-present write inside the stack window is the stack growing,
           not a crash. Bit 0 of the error code clear means the page is absent;
           a protection fault on a mapped page must never be papered over. */
        if (regs->int_no == 14 && (regs->cs & 3U) == 3U &&
            !(regs->err_code & 1U) && process_grow_user_stack(fault_address)) {
            return; /* page mapped; retry the faulting instruction */
        }

        /* Same shape, for the first touch of an anonymous mapping. */
        if (regs->int_no == 14 && (regs->cs & 3U) == 3U &&
            !(regs->err_code & 1U) && process_commit_area(fault_address)) {
            return;
        }

        /* A write protection fault on a *present* page is how a copy-on-write
           page announces its first write after fork. Error code bit 0 set means
           present, bit 1 set means it was a write; anything else here is a real
           access violation and falls through to the signal path. */
        if (regs->int_no == 14 && (regs->cs & 3U) == 3U &&
            (regs->err_code & 1U) && (regs->err_code & 2U) &&
            process_handle_cow_fault(fault_address)) {
            return; /* page is private and writable now; retry the instruction */
        }

        if (process_fault_from_interrupt(regs, fault_signal(regs->int_no))) {
            kprintf("%s in user mode at RIP %p addr %p (error %x), signalling process\n",
                    exception_messages[regs->int_no], (void *)fault_rip,
                    (void *)fault_address, fault_error);
            return;
        }
        /* The captured values, not the ones still in the frame: a handler that
           switched process left the next process's registers there, and
           reporting those sends the reader looking in the wrong place. */
        kprintf("Exception: %s\n", exception_messages[regs->int_no]);
        kprintf("Error Code: %x  CS: %x\n", (unsigned)fault_error, (unsigned)fault_cs);
        kprintf("RIP: %p  addr: %p\n", (void *)fault_rip, (void *)fault_address);
        panic(exception_messages[regs->int_no]);
    }
    kprintf("Received interrupt: %d\n", regs->int_no);
}

#define VECTOR_DOUBLE_FAULT 8U
#define IA32_GS_BASE 0xC0000101U
#define IA32_KERNEL_GS_BASE 0xC0000102U

static uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

/* Returns with the lock still held; isr.S drops it once rsp is somewhere the
   next process cannot be standing on. See syscall_dispatch. */
void isr_handler(struct interrupt_frame *regs) {
    /*
     * Before the lock and without taking it. A double fault means the
     * processor could not deliver some earlier fault, and it may well have
     * been holding the lock when that happened -- waiting for it here would
     * turn a reportable failure into a hang. It runs on the IST stack, which
     * is the one thing the fault cannot have broken.
     */
    if (regs->int_no == VECTOR_DOUBLE_FAULT) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("DOUBLE FAULT: rip %p cs %x rsp %p ss %x cr2 %p rflags %x\n",
                (void *)regs->rip, (unsigned)regs->cs, (void *)regs->rsp,
                (unsigned)regs->ss, (void *)cr2, (unsigned)regs->rflags);
        /*
         * Which stack it was supposed to be on, and whose -- printed a step at
         * a time, each line reading one thing further from the processor. If
         * the report stops, where it stopped is the answer: everything here is
         * a dereference of something the failure may have taken away.
         */
        uint64_t gs = read_msr(IA32_GS_BASE);
        uint64_t kernel_gs = read_msr(IA32_KERNEL_GS_BASE);
        kprintf("  gs %p kernelgs %p\n", (void *)gs, (void *)kernel_gs);
        /* Whichever half holds the block. In kernel mode it should be GS, and
           a failure that arrives with them the other way round is itself the
           finding -- but the rest of the report still has to be printable. */
        struct cpu *self = (struct cpu *)(gs ? gs : kernel_gs);
        if (self) {
            kprintf("  cpu %u kernel_rsp %p current %p\n", self->index,
                    (void *)self->kernel_rsp, (void *)self->current);
            struct process *running = self->current;
            if (running)
                kprintf("  pid %u stack %p..%p\n", (unsigned)running->pid,
                        (void *)running->kernel_stack_base,
                        (void *)running->kernel_stack_top);
        }
        /* The words above the dead stack pointer, kernel text addresses only.
           A stack that ran away rather than merely ran deep says so here: the
           same few return addresses, over and over. */
        const uint64_t *word = (const uint64_t *)regs->rsp;
        unsigned shown = 0;
        for (unsigned index = 0; index < 512 && shown < 24; index++) {
            uint64_t value = word[index];
            if (value < 0xFFFFFFFF80100000ULL || value >= 0xFFFFFFFF80400000ULL)
                continue;
            kprintf("  [%u] %p\n", index, (void *)value);
            shown++;
        }
        panic("double fault");
    }
    /*
     * An exception this processor took while already inside the lock is a
     * kernel bug, and taking the lock again would be a worse one: a ticket
     * lock cannot be satisfied by its own holder, so the machine would stop
     * here with every other processor queued behind it -- no panic, no
     * output, nothing a signal could reach. isr_dispatch() panics on every
     * kernel-mode fault (each of the paths that recovers is gated on a fault
     * from user mode), so going in still holding the lock is safe: it does
     * not come back, and the unlock the entry stub would have done on the
     * way out is not owed to anyone.
     */
    if (!kernel_lock_held_here()) kernel_lock();
    isr_dispatch(regs);
    /* Same move as the syscall return makes, and for the same reason: the
       frame may be sitting on the kernel stack of a process this processor
       has just given up. Only frames going back to user mode need it; one
       going back to the idle loop is already on a stack of this processor's
       own. */
    if ((regs->cs & 3U) == 3U) {
        uint64_t stack_top = cpu_current()->kernel_rsp;
        if (stack_top) {
            struct interrupt_frame *resumed =
                (struct interrupt_frame *)(stack_top - sizeof(*regs));
            if (resumed != regs) *resumed = *regs;
        }
    }
}
