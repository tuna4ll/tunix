#include <stdint.h>

#include "../../include/boot.h"
#include "../../include/cpu.h"
#include "../../include/klock.h"
#include "../../include/percpu.h"
#include "../../include/process.h"
#include "../../include/smp.h"
#include "../../include/time.h"
#include "../../include/timer.h"
#include "../../include/vmm.h"
#include "../../include/vmm_arch.h"
#include "../../include/hwcap.h"
#include "aarch64.h"

extern void kprintf(const char *fmt, ...);
extern char secondary_entry[];

#define PSCI_CPU_ON 0xC4000003ULL
#define STARTUP_TIMEOUT_NS 1000000000ULL
#define FLUSH_TIMEOUT_NS 2000000000ULL
#define MPIDR_AFFINITY_MASK 0xFF00FFFFFFULL

uint64_t secondary_root;
uint64_t secondary_stack_top;
uint64_t secondary_index;

static unsigned online_cpus = 1;

unsigned smp_cpu_count(void) { return online_cpus; }

static uint64_t current_mpidr(void) {
    uint64_t value;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(value));
    return value & MPIDR_AFFINITY_MASK;
}

void smp_service_flush(void) {
    struct cpu *self = cpu_current();
    if (!__atomic_load_n(&self->flush_pending, __ATOMIC_ACQUIRE)) return;
    vmm_arch_write_root(vmm_arch_read_root());
    __atomic_store_n(&self->flush_pending, 0, __ATOMIC_RELEASE);
}

void smp_flush_interrupt(void) {
    smp_service_flush();
}

static void flush_others(uint64_t cr3, int everywhere) {
    if (online_cpus < 2 || (!cr3 && !everywhere)) return;

    unsigned self = cpu_current()->index;
    int asked = 0;
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (index == self || !cpu->online) continue;
        if (!everywhere && cpu->address_space != cr3) continue;
        __atomic_store_n(&cpu->flush_pending, 1, __ATOMIC_RELEASE);
        asked = 1;
    }
    if (!asked) return;

    gic_send_flush_ipi();
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (index == self || !cpu->online) continue;
        uint64_t deadline = time_uptime_ns() + FLUSH_TIMEOUT_NS;
        while (__atomic_load_n(&cpu->flush_pending, __ATOMIC_ACQUIRE)) {
            if (time_uptime_ns() >= deadline) {
                static volatile uint8_t reported[SMP_MAX_CPUS];
                if (!reported[index]) {
                    reported[index] = 1;
                    kprintf("SMP: cpu %u did not answer a flush\n", index);
                }
                __atomic_store_n(&cpu->flush_pending, 0, __ATOMIC_RELEASE);
                break;
            }
            cpu_relax();
        }
    }
}

void smp_flush_address_space(uint64_t cr3) {
    flush_others(cr3, 0);
}

void smp_flush_kernel_mappings(void) {
    flush_others(0, 1);
}

void aarch64_secondary_start(uint64_t index) {
    unsigned cpu = (unsigned)index;
    percpu_activate(cpu);
    time_mark_processor(cpu);
    process_enable_extended_fpu();
    arch_note_cpu_features();
    gic_init_secondary(cpu);
    arch_timer_start(TIMER_FREQUENCY_HZ);
    gic_enable_interrupt(aarch64_platform.timer_interrupt);
    percpu_slot(cpu)->apic_id = (uint32_t)current_mpidr();
    percpu_mark_online(cpu);
    process_run_idle();
}

static int start_processor(unsigned index, const struct aarch64_cpu *target) {
    struct cpu *cpu = percpu_slot(index);
    if (!cpu) return -1;

    secondary_root = vmm_kernel_cr3();
    secondary_stack_top = percpu_boot_stack(index);
    secondary_index = index;
    uint64_t entry = (uint64_t)secondary_entry + aarch64_platform.load_offset;

    uint64_t before = time_uptime_ns();
    if (target->release_address) {
        *(volatile uint64_t *)vmm_phys_to_virt(target->release_address) = entry;
        __asm__ volatile("sev");
    } else if (aarch64_platform.psci_method != PSCI_NONE) {
        if (psci_call(PSCI_CPU_ON, target->mpidr, entry, index) != 0) return -1;
    } else {
        return -1;
    }

    uint64_t deadline = time_uptime_ns() + STARTUP_TIMEOUT_NS;
    while (!cpu->online && time_uptime_ns() < deadline) cpu_relax();
    if (!cpu->online) return -1;
    time_check_processor(index, before, time_uptime_ns());
    return 0;
}

void smp_init(void) {
    uint64_t self = current_mpidr();
    percpu_mark_online(0);
    percpu_slot(0)->apic_id = (uint32_t)self;

    if (boot_command_line_flag("nosmp")) {
        kprintf("SMP: one processor, nosmp\n");
        return;
    }
    if (aarch64_platform.cpu_count < 2) {
        kprintf("SMP: one processor\n");
        return;
    }

    kernel_lock();
    unsigned index = 1;
    unsigned described = aarch64_platform.cpu_count;
    if (described > AARCH64_MAX_CPUS) described = AARCH64_MAX_CPUS;
    for (unsigned slot = 0; slot < described && index < SMP_MAX_CPUS; slot++) {
        const struct aarch64_cpu *target = &aarch64_platform.cpus[slot];
        if ((target->mpidr & MPIDR_AFFINITY_MASK) == self) continue;
        if (start_processor(index, target) != 0)
            kprintf("SMP: cpu with mpidr %x did not come up\n", (unsigned)target->mpidr);
        index++;
    }
    online_cpus = percpu_online_count();
    kernel_unlock();
    kprintf("SMP: %u of %u processors running\n", online_cpus, aarch64_platform.cpu_count);
}
