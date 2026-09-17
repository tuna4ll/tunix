#include <stdint.h>

#include "../../include/irq.h"
#include "../../include/klock.h"
#include "../../include/percpu.h"
#include "../../include/process.h"
#include "../../include/process_arch.h"
#include "../../include/signal.h"
#include "../../include/smp.h"
#include "../../include/timer.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);
extern void panic(const char *message) __attribute__((noreturn));

#define EC_UNKNOWN 0x00U
#define EC_SVC64 0x15U
#define EC_SYS64 0x18U
#define EC_IABT_LOW 0x20U
#define EC_PC_ALIGN 0x22U
#define EC_DABT_LOW 0x24U
#define EC_SP_ALIGN 0x26U
#define EC_FP_EXCEPTION 0x2CU
#define EC_BRK64 0x3CU

#define FSC_TYPE_MASK 0x3CU
#define FSC_TRANSLATION 0x04U
#define FSC_ACCESS 0x08U
#define FSC_PERMISSION 0x0CU
#define ESR_WNR (1U << 6)

#define SIGTRAP 5

static void relocate_user_frame(struct interrupt_frame *frame) {
    if (!arch_interrupt_from_user(frame)) return;
    uint64_t top = cpu_current()->kernel_rsp;
    if (!top) return;
    struct interrupt_frame *resumed = (struct interrupt_frame *)(top - sizeof(*frame));
    if (resumed != frame) *resumed = *frame;
}

static uint64_t read_esr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, esr_el1" : "=r"(value));
    return value;
}

static uint64_t read_far(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, far_el1" : "=r"(value));
    return value;
}

static int user_fault_signal(uint32_t class) {
    switch (class) {
    case EC_PC_ALIGN:
    case EC_SP_ALIGN: return SIGBUS;
    case EC_FP_EXCEPTION: return SIGFPE;
    case EC_BRK64: return SIGTRAP;
    case EC_UNKNOWN:
    case EC_SYS64: return SIGILL;
    default: return SIGSEGV;
    }
}

static void user_fault(struct interrupt_frame *frame, uint64_t esr, uint64_t far) {
    uint32_t class = (uint32_t)(esr >> 26);
    if (class == EC_DABT_LOW || class == EC_IABT_LOW) {
        uint32_t status = (uint32_t)esr & FSC_TYPE_MASK;
        if ((status == FSC_TRANSLATION || status == FSC_ACCESS) &&
            (process_grow_user_stack(far) || process_commit_area(far))) return;
        if (status == FSC_PERMISSION && class == EC_DABT_LOW && (esr & ESR_WNR) &&
            process_handle_cow_fault(far)) return;
    }

    int signal_number = user_fault_signal(class);
    struct process *faulted = process_current();
    kprintf("fault: %s[%d] class %x at %p addr %p (esr %p)\n",
            faulted ? faulted->name : "?", (int)process_current_pid(), class,
            (void *)frame->elr, (void *)far, (void *)esr);
    if (process_fault_from_interrupt(frame, signal_number)) return;
    panic("unhandled user exception");
}

int aarch64_el0_sync(struct syscall_frame *frame) {
    uint64_t esr = read_esr();
    uint32_t class = (uint32_t)(esr >> 26);
    if (class == EC_SVC64) {
        frame->reserved[0] = aarch64_syscall_number(frame->x[8]);
        syscall_dispatch(frame);
        return 1;
    }

    uint64_t far = read_far();
    struct interrupt_frame *interrupted = (struct interrupt_frame *)frame;
    klock_note(KLOCK_NOTE_INTERRUPT | class);
    kernel_lock_from_isr();
    user_fault(interrupted, esr, far);
    relocate_user_frame(interrupted);
    return 2;
}

void aarch64_el1_sync(struct syscall_frame *frame) {
    uint64_t esr = read_esr();
    kprintf("\nKERNEL EXCEPTION: class %x esr %p elr %p far %p sp %p\n",
            (unsigned)(esr >> 26), (void *)esr, (void *)frame->elr, (void *)read_far(),
            (void *)((uint64_t)frame + sizeof(*frame)));
    kprintf("x0 %p x1 %p x2 %p x3 %p x30 %p\n", (void *)frame->x[0], (void *)frame->x[1],
            (void *)frame->x[2], (void *)frame->x[3], (void *)frame->x[30]);
    panic("kernel exception");
}

void aarch64_unexpected(struct syscall_frame *frame, uint64_t kind) {
    static const char *const names[] = { "EL1t", "FIQ", "SError", "AArch32" };
    kprintf("\nUNEXPECTED %s exception: esr %p elr %p far %p\n", names[kind & 3U],
            (void *)read_esr(), (void *)frame->elr, (void *)read_far());
    panic("unexpected exception");
}

int aarch64_irq(struct interrupt_frame *frame) {
    uint32_t intid = gic_acknowledge();
    if (intid >= 1020U && intid < 8192U) return 0;
    if (intid == AARCH64_SGI_FLUSH) {
        gic_end_of_interrupt(intid);
        smp_flush_interrupt();
        return 0;
    }

    klock_note(KLOCK_NOTE_INTERRUPT | (intid & 0xFFFFU));
    kernel_lock_from_isr();
    if (intid == aarch64_platform.timer_interrupt) {
        aarch64_timer_rearm();
        gic_end_of_interrupt(intid);
        if (cpu_current()->index == 0) timer_irq(frame);
        else process_timer_interrupt(frame);
    } else if (intid >= 8192U) {
        gic_end_of_interrupt(intid);
        irq_dispatch(IRQ_VECTOR_FIRST + (intid - 8192U));
    } else {
        gic_end_of_interrupt(intid);
        irq_dispatch(IRQ_VECTOR_FIRST + intid);
    }
    relocate_user_frame(frame);
    return 1;
}
