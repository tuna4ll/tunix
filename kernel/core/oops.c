#include <stddef.h>
#include <stdint.h>
#include <tunix/cpu.h>
#include <tunix/kallsyms.h>
#include <tunix/kprintf.h>
#include <tunix/module.h>
#include <tunix/oops.h>
#include <tunix/percpu.h>
#include <tunix/process.h>
#include <tunix/uts.h>

#define TRACE_FRAMES     64U
#define TRACE_SCAN_WORDS 1024U
#define TRACE_SCAN_SHOWN 48U

static volatile uint32_t oops_owner;
static uint32_t oops_depth;

static uint32_t cpu_token(void) {
    struct cpu *self = cpu_current();
    return self ? self->index + 1U : 1U;
}

unsigned oops_begin(void) {
    uint32_t me = cpu_token();
    if (__atomic_load_n(&oops_owner, __ATOMIC_ACQUIRE) == me) return ++oops_depth;
    uint32_t expected = 0;
    while (!__atomic_compare_exchange_n(&oops_owner, &expected, me, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED)) {
        expected = 0;
        cpu_relax();
    }
    oops_depth = 1;
    return 1;
}

int oops_in_progress(void) { return __atomic_load_n(&oops_owner, __ATOMIC_ACQUIRE) == cpu_token(); }

void oops_end(void) {
    if (__atomic_load_n(&oops_owner, __ATOMIC_RELAXED) != cpu_token()) return;
    if (--oops_depth) return;
    __atomic_store_n(&oops_owner, 0, __ATOMIC_RELEASE);
}

void oops_print_hex_width(uint64_t value, unsigned digits) {
    char text[17];
    if (digits > 16U) digits = 16U;
    for (unsigned index = 0; index < digits; index++)
        text[index] = "0123456789abcdef"[(value >> ((digits - 1U - index) * 4U)) & 0xFU];
    text[digits] = '\0';
    kprintf("%s", text);
}

void oops_print_hex(uint64_t value) { oops_print_hex_width(value, 16U); }

void oops_print_symbol(uint64_t address) {
    char text[160];
    kallsyms_format(address, text, sizeof(text));
    kprintf("%s", text);
}

void oops_print_task(void) {
    struct cpu *self = cpu_current();
    struct process *task = self ? self->current : NULL;
    kprintf("CPU: %u UID: %u PID: %d Comm: %s Not tainted %s #1\n", self ? self->index : 0U,
            task ? (unsigned)task->cred.euid : 0U, task ? (int)task->pid : 0,
            task ? task->name : "swapper", UTS_RELEASE);
}

static int text_address(uint64_t address) {
    return kallsyms_is_text(address) || module_is_text(address);
}

static int stack_bounds(uint64_t pointer, uint64_t *low, uint64_t *high) {
    struct cpu *self = cpu_current();
    struct process *task = self ? self->current : NULL;
    if (task && task->kernel_stack_base && pointer >= task->kernel_stack_base &&
        pointer < task->kernel_stack_top) {
        *low = task->kernel_stack_base;
        *high = task->kernel_stack_top;
        return 1;
    }
    if (self && self->idle_stack_top && pointer < self->idle_stack_top &&
        pointer >= self->idle_stack_top - PERCPU_IDLE_STACK_BYTES) {
        *low = self->idle_stack_top - PERCPU_IDLE_STACK_BYTES;
        *high = self->idle_stack_top;
        return 1;
    }
    return 0;
}

void oops_print_trace(uint64_t frame, uint64_t stack_pointer) {
    kprintf("Call Trace:\n <TASK>\n");
    uint64_t low = 0, high = 0;
#if defined(TUNIX_FRAME_POINTERS)
    (void)stack_pointer;
    if (stack_bounds(frame, &low, &high)) {
        for (unsigned depth = 0; depth < TRACE_FRAMES; depth++) {
            if ((frame & 7U) || frame < low || frame > high - 16U) break;
            const uint64_t *record = (const uint64_t *)frame;
            uint64_t next = record[0];
            uint64_t ret = record[1];
            if (!text_address(ret)) break;
            kprintf(" ");
            oops_print_symbol(ret);
            kprintf("\n");
            if (next <= frame) break;
            frame = next;
        }
    }
#else
    (void)frame;
    if (stack_bounds(stack_pointer, &low, &high)) {
        unsigned shown = 0;
        uint64_t at = stack_pointer & ~7ULL;
        for (unsigned words = 0; words < TRACE_SCAN_WORDS && at + 8U <= high; words++, at += 8U) {
            uint64_t value = *(const uint64_t *)at;
            if (!text_address(value)) continue;
            kprintf(" ? ");
            oops_print_symbol(value);
            kprintf("\n");
            if (++shown >= TRACE_SCAN_SHOWN) break;
        }
    }
#endif
    kprintf(" </TASK>\n");
}

__attribute__((noinline)) void dump_stack(void) {
    oops_print_task();
    uint64_t here = (uint64_t)__builtin_frame_address(0);
    oops_print_trace(here, here);
}

__attribute__((noinline)) void oops_report(const char *headline) {
    unsigned depth = oops_begin();
    kprintf("%s\n", headline);
    oops_print_task();
    uint64_t here = (uint64_t)__builtin_frame_address(0);
    if (depth == 1U) oops_print_trace(here, here);
    oops_end();
}

__attribute__((noinline)) void warn_slowpath(const char *file, int line) {
    unsigned depth = oops_begin();
    struct cpu *self = cpu_current();
    struct process *task = self ? self->current : NULL;
    kprintf("WARNING: CPU: %u PID: %d at %s:%d ", self ? self->index : 0U,
            task ? (int)task->pid : 0, file, line);
    oops_print_symbol((uint64_t)__builtin_return_address(0));
    kprintf("\n");
    oops_print_task();
    uint64_t here = (uint64_t)__builtin_frame_address(0);
    if (depth == 1U) oops_print_trace(here, here);
    kprintf("---[ end trace 0000000000000000 ]---\n");
    oops_end();
}
