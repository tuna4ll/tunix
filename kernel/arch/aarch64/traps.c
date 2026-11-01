#include <stdint.h>

#include <tunix/irq.h>
#include <tunix/kentry.h>
#include <tunix/percpu.h>
#include <tunix/process.h>
#include <tunix/process_arch.h>
#include <tunix/signal.h>
#include <tunix/smp.h>
#include <tunix/timer.h>
#include "aarch64.h"
#include <tunix/kprintf.h>
#include <tunix/oops.h>
#include <tunix/vmm.h>

#define EC_UNKNOWN      0x00U
#define EC_SVC64        0x15U
#define EC_SYS64        0x18U
#define EC_IABT_LOW     0x20U
#define EC_PC_ALIGN     0x22U
#define EC_DABT_LOW     0x24U
#define EC_SP_ALIGN     0x26U
#define EC_FP_EXCEPTION 0x2CU
#define EC_BRK64        0x3CU

#define FSC_TYPE_MASK   0x3CU
#define FSC_TRANSLATION 0x04U
#define FSC_ACCESS      0x08U
#define FSC_PERMISSION  0x0CU
#define ESR_WNR         (1U << 6)

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
    case EC_SP_ALIGN:     return SIGBUS;
    case EC_FP_EXCEPTION: return SIGFPE;
    case EC_BRK64:        return SIGTRAP;
    case EC_UNKNOWN:
    case EC_SYS64:        return SIGILL;
    default:              return SIGSEGV;
    }
}

static void user_fault(struct interrupt_frame *frame, uint64_t esr, uint64_t far) {
    uint32_t class = (uint32_t)(esr >> 26);
    if (class == EC_DABT_LOW || class == EC_IABT_LOW) {
        uint32_t status = (uint32_t)esr & FSC_TYPE_MASK;
        struct process *self = process_current();
        if (status == FSC_ACCESS && self && vmm_mark_young(self->cr3, far)) return;
        if ((status == FSC_TRANSLATION || status == FSC_ACCESS) &&
            (process_grow_user_stack(far) || process_commit_area(far)))
            return;
        if (status == FSC_PERMISSION && class == EC_DABT_LOW && (esr & ESR_WNR) &&
            process_handle_cow_fault(far))
            return;
        if (process_fatal_signal_pending()) return;
    }

    struct process_fault fault = {user_fault_signal(class), SI_KERNEL, 0, 0, (uint32_t)esr};
    switch (class) {
    case EC_DABT_LOW:
    case EC_IABT_LOW:
        fault.code = ((uint32_t)esr & FSC_TYPE_MASK) == FSC_PERMISSION ? SEGV_ACCERR : SEGV_MAPERR;
        fault.address = far;
        break;
    case EC_PC_ALIGN:
    case EC_SP_ALIGN:
        fault.code = BUS_ADRALN;
        fault.address = class == EC_PC_ALIGN ? far : frame->sp_el0;
        break;
    case EC_FP_EXCEPTION:
        fault.code = FPE_FLTINV;
        fault.address = frame->elr;
        break;
    case EC_BRK64:
        fault.code = TRAP_BRKPT;
        fault.address = frame->elr;
        break;
    case EC_UNKNOWN:
    case EC_SYS64:
        fault.code = ILL_ILLOPN;
        fault.address = frame->elr;
        break;
    default: break;
    }
    struct process *faulted = process_current();
    kprintf("fault: %s[%d] class %x at %p addr %p (esr %p)\n", faulted ? faulted->name : "?",
            (int)process_current_pid(), class, (void *)frame->elr, (void *)far, (void *)esr);
    if (process_fault_from_interrupt(frame, &fault)) return;
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
    kernel_enter_from_isr();
    user_fault(interrupted, esr, far);
    relocate_user_frame(interrupted);
    return 2;
}

#define EC_IABT_CUR 0x21U
#define EC_DABT_CUR 0x25U

static const char *exception_class_name(uint32_t class) {
    switch (class) {
    case EC_UNKNOWN:      return "Unknown/Uncategorized";
    case EC_SVC64:        return "SVC (AArch64)";
    case EC_SYS64:        return "MSR/MRS (AArch64)";
    case EC_IABT_LOW:     return "IABT (lower EL)";
    case EC_IABT_CUR:     return "IABT (current EL)";
    case EC_PC_ALIGN:     return "PC Alignment";
    case EC_DABT_LOW:     return "DABT (lower EL)";
    case EC_DABT_CUR:     return "DABT (current EL)";
    case EC_SP_ALIGN:     return "SP Alignment";
    case EC_FP_EXCEPTION: return "FP/SIMD";
    case EC_BRK64:        return "BRK (AArch64)";
    default:              return "unexpected";
    }
}

void aarch64_el1_sync(struct syscall_frame *frame) {
    uint64_t esr = read_esr();
    uint64_t far = read_far();
    uint32_t class = (uint32_t)(esr >> 26);
    uint64_t sp = (uint64_t)frame + sizeof(*frame);
    unsigned depth = oops_begin();
    if (depth > 2U) panic("recursive fault while reporting a fault");
    if (class == EC_DABT_CUR || class == EC_IABT_CUR) {
        kprintf("Unable to handle kernel %s at virtual address ",
                far < 4096U ? "NULL pointer dereference" : "paging request");
        oops_print_hex(far);
        kprintf("\nMem abort info:\n  ESR = 0x");
        oops_print_hex(esr);
        kprintf("\n  EC = 0x%x: %s, IL = %u bits\n", class, exception_class_name(class),
                (esr & (1U << 25)) ? 32U : 16U);
        kprintf("Internal error: Oops: ");
    } else {
        kprintf("Internal error: Oops - %s: ", exception_class_name(class));
    }
    oops_print_hex(esr);
    kprintf(" [#%u] SMP\n", depth);
    oops_print_task();
    kprintf("pc : ");
    oops_print_symbol(frame->elr);
    kprintf("\nlr : ");
    oops_print_symbol(frame->x[30]);
    kprintf("\nsp : ");
    oops_print_hex(sp);
    kprintf("\n");
    for (int index = 29; index >= 0; index -= 2) {
        kprintf("x%d: ", index);
        oops_print_hex(frame->x[index]);
        if (index > 0) {
            kprintf(" x%d: ", index - 1);
            oops_print_hex(frame->x[index - 1]);
        }
        kprintf("\n");
    }
    if (depth == 1U) oops_print_trace(frame->x[29], sp);
    panic("Oops: Fatal exception");
}

void aarch64_unexpected(struct syscall_frame *frame, uint64_t kind) {
    static const char *const names[] = {"EL1t", "FIQ", "SError", "AArch32"};
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

    if (intid == AARCH64_SGI_RESCHEDULE) {
        gic_end_of_interrupt(intid);
        kernel_enter_from_isr();
        cpu_current()->in_interrupt++;
        process_reschedule_interrupt(frame);
        cpu_current()->in_interrupt--;
        relocate_user_frame(frame);
        return 1;
    }
    int timer = intid == aarch64_platform.timer_interrupt;
    unsigned due = timer ? timer_local_expired() : 0;
    gic_end_of_interrupt(intid);
    if (timer && !(due & TIMER_LOCAL_DEADLINE) && !cpu_current()->current &&
        !process_ready_pending() && !(cpu_current()->index == 0 && (due & TIMER_LOCAL_TICK))) {
        smp_service_flush();
        return 0;
    }
    kernel_enter_from_isr();
    cpu_current()->in_interrupt++;
    if (timer) {
        if (due & TIMER_LOCAL_DEADLINE) timer_run_deadlines();
        if (!(due & TIMER_LOCAL_TICK)) process_deadline_interrupt(frame);
        else if (cpu_current()->index == 0) timer_irq(frame);
        else process_timer_interrupt(frame);
    } else if (intid >= 8192U) {
        irq_dispatch(IRQ_VECTOR_FIRST + (intid - 8192U));
    } else {
        irq_dispatch(IRQ_VECTOR_FIRST + intid);
    }
    cpu_current()->in_interrupt--;
    relocate_user_frame(frame);
    return 1;
}
