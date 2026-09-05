/*
 * The local APIC and the IOAPIC, which is how interrupts arrive on anything
 * newer than the 8259 pair this kernel started on.
 *
 * The difference that matters is routing. With the PIC, a device's interrupt
 * line *is* its IRQ number. With an IOAPIC the firmware decides which of its
 * inputs a line is wired to, and says so in the MADT -- so IRQ 0 commonly
 * arrives on input 2, and a line can be active-low or level-triggered where
 * the PIC's were neither. Getting that wrong does not produce a wrong
 * interrupt; it produces silence, which is much harder to read.
 *
 * Vectors are unchanged: the PIC was already remapped to 32..47, so the same
 * handlers answer and only the delivery path underneath them moves.
 */
#include <stdint.h>
#include <stddef.h>

#include "../include/acpi.h"
#include "../include/apic.h"
#include "../include/percpu.h"
#include "../include/pic.h"
#include "../include/time.h"
#include "../include/vmm.h"

extern void kprintf(const char *fmt, ...);

/* Their own slices of the device window, after the ACPI tables' 2 MiB. */
#define APIC_WINDOW_OFFSET 0x00400000ULL
#define IO_APIC_WINDOW_OFFSET 0x00401000ULL
#define APIC_PAGE_BYTES 4096ULL

/* Local APIC registers, as offsets from its base. */
#define LAPIC_ID 0x020U
#define LAPIC_EOI 0x0B0U
#define LAPIC_SPURIOUS 0x0F0U
/* The interrupt command register, which is how one processor talks to
   another. The high half names the destination and must be written first:
   writing the low half is what sends. */
#define LAPIC_ICR_LOW 0x300U
#define LAPIC_ICR_HIGH 0x310U
#define LAPIC_LVT_TIMER 0x320U
#define LAPIC_TIMER_INITIAL 0x380U
#define LAPIC_TIMER_CURRENT 0x390U
#define LAPIC_TIMER_DIVIDE 0x3E0U

#define ICR_DELIVERY_PENDING (1U << 12)
#define ICR_LEVEL_ASSERT (1U << 14)
#define ICR_MODE_INIT (5U << 8)
#define ICR_MODE_STARTUP (6U << 8)
#define ICR_SHORTHAND_OTHERS (3U << 18)
#define ICR_DESTINATION_SHIFT 24U

#define LVT_MASKED (1U << 16)
#define LVT_PERIODIC (1U << 17)
/* Divide by 16. The encoding scatters its three bits, which is why it is not
   the number it looks like. */
#define TIMER_DIVIDE_16 0x3U
#define TIMER_DIVISOR 16U
#define CALIBRATION_MS 20ULL
#define LAPIC_SPURIOUS_ENABLE (1U << 8)
/* The vector delivered when an interrupt is withdrawn before it is taken. It
   must have its low four bits set on some older parts, so 0xFF it is. */
#define LAPIC_SPURIOUS_VECTOR 0xFFU
#define LAPIC_ID_SHIFT 24U

/* The IOAPIC is two registers: one selects, the other reads or writes. */
#define IO_APIC_SELECT 0x00U
#define IO_APIC_WINDOW 0x10U
#define IO_APIC_VERSION_REGISTER 0x01U
#define IO_APIC_REDIRECTION_BASE 0x10U
#define IO_APIC_MAX_ENTRY_SHIFT 16U
#define IO_APIC_MAX_ENTRY_MASK 0xFFU

#define REDIRECTION_ACTIVE_LOW (1U << 13)
#define REDIRECTION_LEVEL_TRIGGERED (1U << 15)
#define REDIRECTION_MASKED (1U << 16)
#define REDIRECTION_DESTINATION_SHIFT 24U

#define IRQ_VECTOR_BASE 32U
#define LEGACY_IRQ_COUNT 16U

static volatile uint32_t *local_apic;
static volatile uint32_t *io_apic;
static uint32_t io_apic_global_base;
static uint32_t io_apic_entries;
static uint32_t local_apic_id;
static int active;

static volatile uint32_t *map_registers(uint64_t physical, uint64_t offset) {
    uint64_t virtual_address = DEVICE_MMIO_VIRTUAL_BASE + offset;
    if (vmm_map_page_in(vmm_kernel_cr3(), virtual_address, physical,
                        PAGE_WRITE | PAGE_DEVICE | PAGE_UNCACHED | PAGE_NX) != 0)
        return NULL;
    return (volatile uint32_t *)virtual_address;
}

static uint32_t io_apic_read(uint32_t index) {
    io_apic[IO_APIC_SELECT / sizeof(uint32_t)] = index;
    return io_apic[IO_APIC_WINDOW / sizeof(uint32_t)];
}

static void io_apic_write(uint32_t index, uint32_t value) {
    io_apic[IO_APIC_SELECT / sizeof(uint32_t)] = index;
    io_apic[IO_APIC_WINDOW / sizeof(uint32_t)] = value;
}

/* Where a legacy IRQ actually arrives, and how it is wired, once the
   firmware's overrides are taken into account. */
static uint32_t global_for_irq(const struct acpi_machine *machine, unsigned irq,
                               int *active_low, int *level) {
    *active_low = 0;
    *level = 0;
    for (uint32_t i = 0; i < machine->override_count; i++) {
        if (machine->overrides[i].source != irq) continue;
        *active_low = machine->overrides[i].active_low;
        *level = machine->overrides[i].level_triggered;
        return machine->overrides[i].global;
    }
    return irq;
}

/* The input to program and the vector to deliver are different numbers, and
   confusing them is the whole trap: IRQ 0 arrives on input 2, but the timer
   handler is on vector 32, not 34. */
int apic_route_irq(unsigned global, unsigned vector, int active_low,
                   int level_triggered) {
    if (!active) return -1;
    if (global < io_apic_global_base) return -1;
    uint32_t entry = global - io_apic_global_base;
    if (entry >= io_apic_entries) return -1;

    uint32_t low = vector;
    if (active_low) low |= REDIRECTION_ACTIVE_LOW;
    if (level_triggered) low |= REDIRECTION_LEVEL_TRIGGERED;

    /* High half first: it names the processor, and writing the low half is
       what unmasks the entry. The other order delivers the first interrupt to
       whichever processor the register happened to be pointing at. */
    io_apic_write(IO_APIC_REDIRECTION_BASE + entry * 2U + 1U,
                  local_apic_id << REDIRECTION_DESTINATION_SHIFT);
    io_apic_write(IO_APIC_REDIRECTION_BASE + entry * 2U, low);
    return 0;
}

int apic_route_global(unsigned global, unsigned vector) {
    /* Overrides are listed by the legacy line they replace, but the entry also
       carries the global number that line really arrives on, and that is what
       matches here: the caller has a global number and no legacy line to look
       up. On a machine where the two are equal -- which is every line but the
       timer -- the lookup finds the same entry either way. */
    int active_low = 1;
    int level_triggered = 1;
    const struct acpi_machine *machine = acpi_describe_machine();
    if (machine) {
        for (uint32_t i = 0; i < machine->override_count; i++) {
            if (machine->overrides[i].global != global) continue;
            active_low = machine->overrides[i].active_low;
            level_triggered = machine->overrides[i].level_triggered;
            break;
        }
    }
    return apic_route_irq(global, vector, active_low, level_triggered);
}

void apic_send_eoi(void) {
    if (active) local_apic[LAPIC_EOI / sizeof(uint32_t)] = 0;
}

int apic_is_active(void) { return active; }

static void lapic_write(uint32_t offset, uint32_t value) {
    local_apic[offset / sizeof(uint32_t)] = value;
}

static uint32_t lapic_read(uint32_t offset) {
    return local_apic[offset / sizeof(uint32_t)];
}

/* Whichever processor asks -- the register is one address that answers
   differently on each of them, which is the whole point of it. */
uint32_t apic_local_id(void) {
    return active ? lapic_read(LAPIC_ID) >> LAPIC_ID_SHIFT : 0;
}

/* Until this bit is set the processor accepts nothing at all, and every
   processor has to set its own. */
void apic_enable_local(void) {
    if (!active) return;
    lapic_write(LAPIC_SPURIOUS, LAPIC_SPURIOUS_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

static void wait_for_delivery(void) {
    while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING) __asm__ volatile("pause");
}

static void send_command(uint32_t destination, uint32_t command) {
    lapic_write(LAPIC_ICR_HIGH, destination << ICR_DESTINATION_SHIFT);
    lapic_write(LAPIC_ICR_LOW, command);
    wait_for_delivery();
}

void apic_send_init(uint32_t apic_id) {
    if (active) send_command(apic_id, ICR_MODE_INIT | ICR_LEVEL_ASSERT);
}

/* The vector is a page number, not an address: the processor starts executing
   in real mode at vector * 4096. */
void apic_send_startup(uint32_t apic_id, uint8_t page) {
    if (active) send_command(apic_id, ICR_MODE_STARTUP | ICR_LEVEL_ASSERT | page);
}

void apic_send_ipi_to_others(uint8_t vector) {
    if (!active) return;
    lapic_write(LAPIC_ICR_LOW, ICR_SHORTHAND_OTHERS | ICR_LEVEL_ASSERT | vector);
    wait_for_delivery();
}

/*
 * The local timer, which is the only clock an application processor has: the
 * PIT is a single device wired to one processor, so every other one is
 * preempted by this instead.
 *
 * Its rate is the bus clock divided down, and nothing reports what that is, so
 * it is measured against the TSC -- counting down from the top for a known
 * number of milliseconds and seeing how far it got.
 */
static uint64_t timer_measured_hz[SMP_MAX_CPUS];
static uint32_t timer_initial_count[SMP_MAX_CPUS];

void apic_timer_start(uint32_t hz, uint8_t vector) {
    if (!active || !hz) return;

    lapic_write(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INITIAL, 0xFFFFFFFFU);

    uint64_t deadline = time_uptime_ns() + CALIBRATION_MS * 1000000ULL;
    while (time_uptime_ns() < deadline) __asm__ volatile("pause");

    uint32_t remaining = lapic_read(LAPIC_TIMER_CURRENT);
    lapic_write(LAPIC_TIMER_INITIAL, 0);

    uint64_t elapsed = 0xFFFFFFFFULL - remaining;
    uint64_t per_second = (elapsed * 1000ULL) / CALIBRATION_MS;
    uint32_t count = per_second > hz ? (uint32_t)(per_second / hz) : 1U;

    /* Kept so a report can say what this processor measured, because a
       calibration that went wrong is invisible from anywhere else. */
    unsigned index = cpu_current() ? cpu_current()->index : 0;
    if (index < SMP_MAX_CPUS) {
        timer_measured_hz[index] = per_second;
        timer_initial_count[index] = count;
    }

    lapic_write(LAPIC_TIMER_DIVIDE, TIMER_DIVIDE_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | vector);
    lapic_write(LAPIC_TIMER_INITIAL, count);
}

void apic_timer_calibration(unsigned index, uint64_t *measured_hz,
                            uint32_t *initial_count) {
    if (index >= SMP_MAX_CPUS) return;
    if (measured_hz) *measured_hz = timer_measured_hz[index];
    if (initial_count) *initial_count = timer_initial_count[index];
}

int apic_init(void) {
    const struct acpi_machine *machine = acpi_describe_machine();
    if (!machine) return -1;

    local_apic = map_registers(machine->local_apic, APIC_WINDOW_OFFSET);
    io_apic = map_registers(machine->io_apics[0].address, IO_APIC_WINDOW_OFFSET);
    if (!local_apic || !io_apic) {
        kprintf("APIC: could not map the controllers\n");
        return -1;
    }
    io_apic_global_base = machine->io_apics[0].global_base;

    local_apic_id = local_apic[LAPIC_ID / sizeof(uint32_t)] >> LAPIC_ID_SHIFT;
    io_apic_entries =
        ((io_apic_read(IO_APIC_VERSION_REGISTER) >> IO_APIC_MAX_ENTRY_SHIFT) &
         IO_APIC_MAX_ENTRY_MASK) + 1U;

    /* Enabling the local APIC is a single bit, and until it is set the
       processor will not accept anything the IOAPIC sends. */
    local_apic[LAPIC_SPURIOUS / sizeof(uint32_t)] =
        LAPIC_SPURIOUS_ENABLE | LAPIC_SPURIOUS_VECTOR;

    /* Mask every input before routing any: whatever the firmware left enabled
       would otherwise arrive on a vector this kernel has not claimed. */
    for (uint32_t entry = 0; entry < io_apic_entries; entry++)
        io_apic_write(IO_APIC_REDIRECTION_BASE + entry * 2U, REDIRECTION_MASKED);

    active = 1;

    /* The 8259s stay initialised but silent. Leaving them delivering as well
       would double every interrupt, and their vectors are the same ones. */
    for (unsigned irq = 0; irq < LEGACY_IRQ_COUNT; irq++) pic_mask(irq);

    kprintf("APIC: local apic %u, ioapic with %u inputs from global %u\n",
            (unsigned)local_apic_id, (unsigned)io_apic_entries,
            (unsigned)io_apic_global_base);
    return 0;
}

int apic_route_legacy_irq(unsigned irq) {
    const struct acpi_machine *machine = acpi_describe_machine();
    if (!machine || !active) return -1;
    int active_low = 0;
    int level = 0;
    uint32_t global = global_for_irq(machine, irq, &active_low, &level);
    return apic_route_irq(global, IRQ_VECTOR_BASE + irq, active_low, level);
}
