#include <stdint.h>
#include "../../include/acpi.h"
#include "../../include/input.h"
#include "../../include/power.h"
#include "../../include/interrupt.h"
#include "../../include/irq.h"
#include "../../include/klock.h"
#include "../../include/percpu.h"
#include "../../include/pic.h"
#include "../../include/apic.h"
#include "../../include/boot.h"
#include "../../include/file.h"
#include "../../include/process.h"
#include "../../include/vmm.h"
#include "../../include/vfs.h"
#include "../../include/signal.h"
#include "../../include/smp.h"
#include "../../include/timer.h"

extern void kprintf(const char *fmt, ...);
extern void panic(const char *msg);

#define VERBOSE_FAULT_LIMIT 24U

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

static int fault_signal(uint64_t vector) {
    switch (vector) {
        case 0:  return SIGFPE;
        case 6:  return SIGILL;
        case 16:
        case 19: return SIGFPE;
        case 17: return SIGBUS;
        default: return SIGSEGV;
    }
}

static void interrupt_acknowledge(unsigned vector) {
    if (apic_is_active()) apic_send_eoi();
    else pic_send_eoi(vector);
}

static void isr_dispatch(struct interrupt_frame *regs) {
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
    if (regs->int_no == ACPI_SCI_VECTOR) {
        apic_send_eoi();
        if (acpi_sci_interrupt()) power_button_pressed();
        return;
    }
    if (regs->int_no >= IRQ_VECTOR_FIRST &&
        regs->int_no < IRQ_VECTOR_FIRST + IRQ_VECTOR_COUNT) {
        apic_send_eoi();
        irq_dispatch((unsigned)regs->int_no);
        return;
    }
    if (regs->int_no < 32) {
        uint64_t fault_rip = regs->rip;
        uint64_t fault_error = regs->err_code;
        uint64_t fault_cs = regs->cs;
        uint64_t fault_address = 0;
        if (regs->int_no == 14)
            __asm__ volatile("mov %%cr2, %0" : "=r"(fault_address));

        if (boot_verbose() && (regs->cs & 3U) == 3U) {
            static unsigned traced;
            if (traced < VERBOSE_FAULT_LIMIT) {
                traced++;
                kprintf("fault: pid %d rip %p addr %p error %x\n",
                        (int)process_current_pid(), (void *)fault_rip,
                        (void *)fault_address, (unsigned)fault_error);
            }
        }

        if (regs->int_no == 14 && (regs->cs & 3U) == 3U &&
            !(regs->err_code & 1U) && process_grow_user_stack(fault_address)) {
            return;
        }

        if (regs->int_no == 14 && (regs->cs & 3U) == 3U &&
            !(regs->err_code & 1U) && process_commit_area(fault_address)) {
            return;
        }

        if (regs->int_no == 14 && (regs->cs & 3U) == 3U &&
            (regs->err_code & 1U) && (regs->err_code & 2U) &&
            process_handle_cow_fault(fault_address)) {
            return;
        }

        if (process_signal_has_handler(fault_signal(regs->int_no)) &&
            process_fault_from_interrupt(regs, fault_signal(regs->int_no)))
            return;

        struct process *faulted = process_current();
        char faulted_name[32];
        int faulted_pid = (int)process_current_pid();
        for (unsigned i = 0; i < sizeof(faulted_name) - 1U; i++) {
            faulted_name[i] = faulted && faulted->name[i] ? faulted->name[i] : 0;
            if (!faulted_name[i]) break;
        }
        faulted_name[sizeof(faulted_name) - 1U] = 0;
        if (!faulted) faulted_name[0] = 0;

        struct vm_area *area = process_find_area(fault_rip);
        const char *object = "?";
        uint64_t within = fault_rip;
        if (area) {
            within = fault_rip - area->start + area->offset;
            object = area->file && area->file->node ? area->file->node->name : "anon";
        }

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

void isr_handler(struct interrupt_frame *regs) {
    if (regs->int_no == VECTOR_DOUBLE_FAULT) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        kprintf("DOUBLE FAULT: rip %p cs %x rsp %p ss %x cr2 %p rflags %x\n",
                (void *)regs->rip, (unsigned)regs->cs, (void *)regs->rsp,
                (unsigned)regs->ss, (void *)cr2, (unsigned)regs->rflags);
        uint64_t gs = read_msr(IA32_GS_BASE);
        uint64_t kernel_gs = read_msr(IA32_KERNEL_GS_BASE);
        kprintf("  gs %p kernelgs %p\n", (void *)gs, (void *)kernel_gs);
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
    klock_note(KLOCK_NOTE_INTERRUPT | (uint32_t)regs->int_no);
    kernel_lock_from_isr();
    isr_dispatch(regs);
    if ((regs->cs & 3U) == 3U) {
        uint64_t stack_top = cpu_current()->kernel_rsp;
        if (stack_top) {
            struct interrupt_frame *resumed =
                (struct interrupt_frame *)(stack_top - sizeof(*regs));
            if (resumed != regs) *resumed = *regs;
        }
    }
}
