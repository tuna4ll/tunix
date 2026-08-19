#include <stdint.h>
#include "../../include/acpi.h"
#include "../../include/input.h"
#include "../../include/power.h"
#include "../../include/interrupt.h"
#include "../../include/klock.h"
#include "../../include/percpu.h"
#include "../../include/pic.h"
#include "../../include/apic.h"
#include "../../include/file.h"
#include "../../include/process.h"
#include "../../include/vmm.h"
#include "../../include/vfs.h"
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

        /*
         * Everything the report needs is read *before* the fault is signalled.
         * A fatal fault tears the process down and switches away, so by the
         * time a message printed afterwards runs, process_current() is somebody
         * else -- which is exactly how this reporter came to name the wrong
         * process and read the wrong stack.
         */
        struct process *faulted = process_current();
        char faulted_name[32];
        int faulted_pid = (int)process_current_pid();
        for (unsigned i = 0; i < sizeof(faulted_name) - 1U; i++) {
            faulted_name[i] = faulted && faulted->name[i] ? faulted->name[i] : 0;
            if (!faulted_name[i]) break;
        }
        faulted_name[sizeof(faulted_name) - 1U] = 0;
        if (!faulted) faulted_name[0] = 0;

        /* Where the instruction is, said the way a person can act on it: a raw
           RIP in a shared library means nothing without knowing which mapping
           it landed in and how far into it. */
        struct vm_area *area = process_find_area(fault_rip);
        const char *object = "?";
        uint64_t within = fault_rip;
        if (area) {
            within = fault_rip - area->start + area->offset;
            object = area->file && area->file->node ? area->file->node->name : "anon";
        }

        /* Registers and a slice of the user stack: on a fault in a shared library
           they are the only way to tell which call went wrong. */
        uint64_t saved_rdi = regs->rdi, saved_rsi = regs->rsi, saved_rdx = regs->rdx;
        uint64_t saved_rax = regs->rax, saved_rbx = regs->rbx, saved_rcx = regs->rcx;
        uint64_t saved_rbp = regs->rbp, saved_rsp = regs->rsp;
        uint64_t saved_r8 = regs->r8, saved_r9 = regs->r9;
        uint64_t stack_words[12];
        int stack_ok = 1;
        for (unsigned i = 0; i < 12; i++) {
            if (!faulted || vmm_copy_from_space(faulted->cr3, &stack_words[i],
                                                saved_rsp + (uint64_t)i * 8U, 8) != 0) {
                stack_ok = 0;
                break;
            }
        }

        /* Reported before the fault is signalled, not after: signalling a
           fatal fault can switch away for good and never come back here,
           and what did come back came back as a different process. */
        kprintf("%s in %s[%d] at %s+%p (RIP %p) addr %p (error %x), signalling process\n",
                exception_messages[regs->int_no],
                faulted_name[0] ? faulted_name : "?", faulted_pid,
                object, (void *)within, (void *)fault_rip,
                (void *)fault_address, fault_error);
        kprintf("fault: rdi=%p rsi=%p rdx=%p rax=%p rbx=%p rcx=%p\n",
                (void *)saved_rdi, (void *)saved_rsi, (void *)saved_rdx,
                (void *)saved_rax, (void *)saved_rbx, (void *)saved_rcx);
        kprintf("fault: rbp=%p rsp=%p r8=%p r9=%p\n",
                (void *)saved_rbp, (void *)saved_rsp,
                (void *)saved_r8, (void *)saved_r9);
        if (stack_ok)
            for (unsigned i = 0; i < 12; i++) {
                uint64_t value = stack_words[i];
                struct vm_area *hit = NULL;
                if (faulted && faulted->memory)
                    for (struct vm_area *a = faulted->memory->areas; a; a = a->next) {
                        if (value < a->start) break;
                        if (value < a->end) { hit = a; break; }
                    }
                if (hit && hit->file && hit->file->node)
                    kprintf("fault: stack[%d] %p = %s+%p\n", (int)i,
                            (void *)value, hit->file->node->name,
                            (void *)(value - hit->start + hit->offset));
                else
                    kprintf("fault: stack[%d] %p\n", (int)i, (void *)value);
            }
        /* The bytes around the pointer the faulting call was given: when a
           heap header has been overwritten, what overwrote it is usually
           legible right there. */
        if (faulted) {
            uint64_t window = (saved_rdi & ~15ULL) - 64ULL;
            for (unsigned row = 0; row < 8; row++) {
                unsigned char bytes[16];
                if (vmm_copy_from_space(faulted->cr3, bytes,
                                        window + (uint64_t)row * 16U, 16) != 0) break;
                char text[17];
                for (unsigned i = 0; i < 16; i++)
                    text[i] = (bytes[i] >= 32 && bytes[i] < 127) ? (char)bytes[i] : '.';
                text[16] = 0;
                kprintf("fault: %p %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x |%s|\n",
                        (void *)(window + (uint64_t)row * 16U),
                        bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
                        bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
                        bytes[12], bytes[13], bytes[14], bytes[15], text);
            }
        }

        if (process_fault_from_interrupt(regs, fault_signal(regs->int_no))) return;

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
