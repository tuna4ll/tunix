/*
 * Bringing up the processors the firmware said are there.
 *
 * The sequence is fixed by the architecture and unforgiving: a processor is
 * reset with an INIT, then told where to start with a startup IPI naming a
 * page below 1 MiB, and it begins in 16-bit real mode with nothing set up. The
 * second startup is not superstition -- the manual asks for two, and some
 * processors ignore the first.
 *
 * The awkward part is the address space. The trampoline runs at a physical
 * address the kernel proper maps nowhere: everything the kernel owns is in the
 * top half. So the page it lives in has to be reachable at its own address for
 * as long as the bring-up lasts.
 *
 * Processors are started one at a time because there is one parameter block,
 * and because a failure is easier to attribute that way.
 */
#include <stddef.h>
#include <stdint.h>

#include "include/acpi.h"
#include "include/apic.h"
#include "include/gdt.h"
#include "include/idt.h"
#include "include/klock.h"
#include "include/kstring.h"
#include "include/percpu.h"
#include "include/pmm.h"
#include "include/process.h"
#include "include/smp.h"
#include "include/syscall.h"
#include "include/time.h"
#include "include/timer.h"
#include "include/vmm.h"

extern void kprintf(const char *fmt, ...);

extern uint8_t smp_trampoline_start[];
extern uint8_t smp_trampoline_end[];
extern uint8_t smp_trampoline_data[];

/* Must match TRAMPOLINE_BASE in trampoline.S: the blob has that address
   compiled into its own jumps. */
#define TRAMPOLINE_PHYSICAL 0x8000ULL
#define TRAMPOLINE_PAGE ((uint8_t)(TRAMPOLINE_PHYSICAL >> 12))

#define DATA_CR3 0
#define DATA_STACK 8
#define DATA_ENTRY 16
#define DATA_INDEX 24

#define STARTUP_TIMEOUT_MS 200ULL
#define INIT_SETTLE_MS 10ULL
#define STARTUP_SETTLE_US 200ULL

static unsigned online_cpus = 1;

unsigned smp_cpu_count(void) { return online_cpus; }

static void reload_cr3(void) {
    uint64_t value;
    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

void smp_service_flush(void) {
    struct cpu *self = cpu_current();
    if (!__atomic_load_n(&self->flush_pending, __ATOMIC_ACQUIRE)) return;
    reload_cr3();
    __atomic_store_n(&self->flush_pending, 0, __ATOMIC_RELEASE);
}

/* What the interrupt itself runs. Deliberately short and lock-free. */
void smp_flush_interrupt(void) {
    apic_send_eoi();
    smp_service_flush();
}

void smp_flush_address_space(uint64_t cr3) {
    if (online_cpus < 2 || !cr3) return;

    unsigned self = cpu_current()->index;
    int asked = 0;
    /* Which processors are looking at this space cannot change underneath:
       only a processor inside the kernel changes its own, and the caller is
       the one holding the kernel lock. */
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (index == self || !cpu->online || cpu->address_space != cr3) continue;
        __atomic_store_n(&cpu->flush_pending, 1, __ATOMIC_RELEASE);
        asked = 1;
    }
    if (!asked) return;

    apic_send_ipi_to_others(SMP_INVALIDATE_VECTOR);
    for (unsigned index = 0; index < SMP_MAX_CPUS; index++) {
        struct cpu *cpu = percpu_slot(index);
        if (index == self || !cpu->online) continue;
        while (__atomic_load_n(&cpu->flush_pending, __ATOMIC_ACQUIRE))
            __asm__ volatile("pause");
    }
}

static void wait_ns(uint64_t nanoseconds) {
    uint64_t deadline = time_uptime_ns() + nanoseconds;
    while (time_uptime_ns() < deadline) __asm__ volatile("pause");
}

static void write_parameter(unsigned offset, uint64_t value) {
    uint64_t address = TRAMPOLINE_PHYSICAL +
                       (uint64_t)(smp_trampoline_data - smp_trampoline_start) + offset;
    *(volatile uint64_t *)vmm_phys_to_virt(address) = value;
}

static int trampoline_page_added;

/*
 * The trampoline's own page, mapped to itself.
 *
 * The instruction that turns paging on is followed by one that has to be
 * fetched through the tables it just installed, and the kernel maps nothing
 * down here. The loader's identity map usually survives in the tables the
 * kernel inherited, in which case there is nothing to do -- and it is a huge
 * page, so trying to map over it would fail rather than be redundant. Only
 * when it is absent is a mapping added, and then only this page.
 */
static int map_trampoline_page(void) {
    uint64_t cr3 = vmm_kernel_cr3();
    uint64_t physical = 0;
    uint64_t flags = 0;
    if (vmm_translate(cr3, TRAMPOLINE_PHYSICAL, &physical, &flags) == 0 &&
        physical == TRAMPOLINE_PHYSICAL && (flags & PAGE_WRITE) && !(flags & PAGE_NX))
        return 0;
    if (vmm_map_page_in(cr3, TRAMPOLINE_PHYSICAL, TRAMPOLINE_PHYSICAL, PAGE_WRITE) != 0)
        return -1;
    trampoline_page_added = 1;
    return 0;
}

static void unmap_trampoline_page(void) {
    if (!trampoline_page_added) return;
    vmm_unmap_page_in(vmm_kernel_cr3(), TRAMPOLINE_PHYSICAL);
    trampoline_page_added = 0;
}

/* Runs on the new processor, on its own idle stack, with the kernel's page
   tables already loaded by the trampoline. */
void smp_ap_entry(uint64_t index) {
    gdt_init_cpu((unsigned)index);
    idt_activate();
    /* The syscall entry MSRs are per-processor; a processor that skipped this
       would take its first syscall as an invalid opcode. */
    syscall_init();
    apic_enable_local();
    percpu_slot((unsigned)index)->apic_id = apic_local_id();
    apic_timer_start(TIMER_FREQUENCY_HZ, SMP_TIMER_VECTOR);
    percpu_mark_online((unsigned)index);
    process_run_idle();
}

static int start_processor(unsigned index, uint32_t apic_id) {
    struct cpu *cpu = percpu_slot(index);
    if (!cpu) return -1;

    write_parameter(DATA_CR3, vmm_kernel_cr3());
    write_parameter(DATA_STACK, percpu_boot_stack(index));
    write_parameter(DATA_ENTRY, (uint64_t)smp_ap_entry);
    write_parameter(DATA_INDEX, index);

    apic_send_init(apic_id);
    wait_ns(INIT_SETTLE_MS * 1000000ULL);
    apic_send_startup(apic_id, TRAMPOLINE_PAGE);
    wait_ns(STARTUP_SETTLE_US * 1000ULL);
    apic_send_startup(apic_id, TRAMPOLINE_PAGE);

    uint64_t deadline = time_uptime_ns() + STARTUP_TIMEOUT_MS * 1000000ULL;
    while (!cpu->online && time_uptime_ns() < deadline) __asm__ volatile("pause");
    return cpu->online ? 0 : -1;
}

void smp_init(void) {
    percpu_mark_online(0);
    percpu_slot(0)->apic_id = apic_local_id();

    const struct acpi_machine *machine = acpi_describe_machine();
    if (!machine || machine->cpu_count < 2 || !apic_is_active()) {
        kprintf("SMP: one processor\n");
        return;
    }

    uint64_t bytes = (uint64_t)(smp_trampoline_end - smp_trampoline_start);
    if (bytes > 4096) {
        kprintf("SMP: trampoline does not fit in a page\n");
        return;
    }

    /*
     * Held for the whole of the bring-up. A processor that is already up is
     * taking timer interrupts and running processes by the time the next one
     * is started, and the page tables being edited here are the ones it is
     * running on.
     */
    kernel_lock();
    if (map_trampoline_page() != 0) {
        kernel_unlock();
        kprintf("SMP: could not map the trampoline\n");
        return;
    }
    memcpy(vmm_phys_to_virt(TRAMPOLINE_PHYSICAL), smp_trampoline_start, (size_t)bytes);

    unsigned index = 1;
    for (uint32_t i = 0; i < machine->cpu_count && index < SMP_MAX_CPUS; i++) {
        if (!machine->cpus[i].usable) continue;
        if (machine->cpus[i].apic_id == percpu_slot(0)->apic_id) continue;
        if (start_processor(index, machine->cpus[i].apic_id) != 0) {
            kprintf("SMP: apic %u did not come up\n", (unsigned)machine->cpus[i].apic_id);
            continue;
        }
        index++;
    }

    unmap_trampoline_page();
    online_cpus = percpu_online_count();
    kprintf("SMP: %u of %u processors running\n", online_cpus,
            (unsigned)machine->cpu_count);
    kernel_unlock();
}
