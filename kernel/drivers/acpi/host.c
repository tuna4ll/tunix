#include <stddef.h>
#include <stdint.h>

#include <tunix/boot.h>
#include <tunix/cpu.h>
#include <tunix/heap.h>
#include <tunix/io.h>
#include <tunix/irq.h>
#include <tunix/apic.h>
#include <tunix/kstring.h>
#include <tunix/pci.h>
#include <tunix/percpu.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/time.h>
#include <tunix/vmm.h>
#include <uacpi/kernel_api.h>

#include "priv.h"

extern void kprintf(const char *fmt, ...);

#define PAGE_BYTES    4096ULL
#define MAPPINGS      128U
#define RING_ITEMS    128U
#define FOREVER       0xFFFFU
#define MILLISECOND   1000000ULL
#define POLL_SLICE_NS 1000000ULL

struct spinlock {
    volatile uint32_t taken;
};

struct sleeper {
    volatile uint32_t held;
    volatile uint32_t count;
};

struct mapping {
    uint64_t first;
    uint64_t end;
    uint64_t virtual_base;
};

struct work_item {
    uacpi_work_handler handler;
    uacpi_handle context;
};

struct work_ring {
    struct spinlock lock;
    struct work_item items[RING_ITEMS];
    unsigned head;
    unsigned count;
    volatile uint32_t busy;
};

struct irq_slot {
    unsigned vector;
    uacpi_interrupt_handler handler;
    uacpi_handle context;
};

struct pci_handle {
    uint8_t bus;
    uint8_t slot;
    uint8_t function;
};

static const char boot_thread;
static struct spinlock mapping_lock;
static struct mapping mappings[MAPPINGS];
static unsigned mapping_count;
static struct work_ring gpe_ring;
static struct work_ring notify_ring;
static volatile uint32_t interrupts_running;
static const char completion_channel;

static uint64_t spin_lock(struct spinlock *lock) {
    uint64_t flags = cpu_irq_save();
    while (__atomic_exchange_n(&lock->taken, 1U, __ATOMIC_ACQUIRE)) cpu_relax();
    return flags;
}

static void spin_unlock(struct spinlock *lock, uint64_t flags) {
    __atomic_store_n(&lock->taken, 0U, __ATOMIC_RELEASE);
    cpu_irq_restore(flags);
}

static int may_sleep(void) {
    struct process *self = process_current();
    return self && !cpu_current()->in_interrupt;
}

static void nap(const void *channel, uint64_t deadline) {
    if (!may_sleep()) {
        cpu_relax();
        return;
    }
    process_prepare_wait(channel, deadline);
    process_wait();
    process_finish_wait();
}

static uint64_t deadline_after(uacpi_u16 timeout) {
    if (timeout == FOREVER) return UINT64_MAX;
    return time_uptime_ns() + (uint64_t)timeout * MILLISECOND;
}

static uint64_t slice_until(uint64_t deadline) {
    uint64_t slice = time_uptime_ns() + POLL_SLICE_NS;
    return slice < deadline ? slice : deadline;
}

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out_rsdp_address) {
    uint64_t rsdp = boot_info()->rsdp;
    if (!rsdp) return UACPI_STATUS_NOT_FOUND;
    *out_rsdp_address = rsdp;
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    if (!len) len = 1;
    if (pmm_physical_range_managed(addr, len)) return vmm_phys_to_virt(addr);
    uint64_t first = addr & ~(PAGE_BYTES - 1U);
    uint64_t end = (addr + len + PAGE_BYTES - 1U) & ~(PAGE_BYTES - 1U);
    uint64_t flags = spin_lock(&mapping_lock);
    for (unsigned index = 0; index < mapping_count; index++) {
        const struct mapping *known = &mappings[index];
        if (first >= known->first && end <= known->end) {
            uint64_t virtual_base = known->virtual_base;
            uint64_t known_first = known->first;
            spin_unlock(&mapping_lock, flags);
            return (void *)(virtual_base + (addr - known_first));
        }
    }
    spin_unlock(&mapping_lock, flags);
    uint64_t virtual_base = vmm_map_device(first, end - first);
    if (!virtual_base) return UACPI_MAP_FAILED;
    flags = spin_lock(&mapping_lock);
    if (mapping_count < MAPPINGS)
        mappings[mapping_count++] = (struct mapping){first, end, virtual_base};
    spin_unlock(&mapping_lock, flags);
    return (void *)(virtual_base + (addr - first));
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {
    (void)addr;
    (void)len;
}

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *text) {
    const char *tag = level == UACPI_LOG_ERROR ? "error: "
        : level == UACPI_LOG_WARN              ? "warning: "
                                               : "";
    kprintf("ACPI: %s%s", tag, text);
}

uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address address, uacpi_handle *out_handle) {
    if (address.segment) return UACPI_STATUS_UNIMPLEMENTED;
    struct pci_handle *handle = kmalloc(sizeof(*handle));
    if (!handle) return UACPI_STATUS_OUT_OF_MEMORY;
    handle->bus = address.bus;
    handle->slot = address.device;
    handle->function = address.function;
    *out_handle = handle;
    return UACPI_STATUS_OK;
}

void uacpi_kernel_pci_device_close(uacpi_handle handle) { kfree(handle); }

uacpi_status uacpi_kernel_pci_read8(uacpi_handle device, uacpi_size offset, uacpi_u8 *value) {
    const struct pci_handle *pci = device;
    *value = pci_config_read8(pci->bus, pci->slot, pci->function, (uint8_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read16(uacpi_handle device, uacpi_size offset, uacpi_u16 *value) {
    const struct pci_handle *pci = device;
    *value = pci_config_read16(pci->bus, pci->slot, pci->function, (uint8_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read32(uacpi_handle device, uacpi_size offset, uacpi_u32 *value) {
    const struct pci_handle *pci = device;
    *value = pci_config_read32(pci->bus, pci->slot, pci->function, (uint8_t)offset);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write8(uacpi_handle device, uacpi_size offset, uacpi_u8 value) {
    const struct pci_handle *pci = device;
    pci_config_write8(pci->bus, pci->slot, pci->function, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write16(uacpi_handle device, uacpi_size offset, uacpi_u16 value) {
    const struct pci_handle *pci = device;
    pci_config_write16(pci->bus, pci->slot, pci->function, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_write32(uacpi_handle device, uacpi_size offset, uacpi_u32 value) {
    const struct pci_handle *pci = device;
    pci_config_write32(pci->bus, pci->slot, pci->function, (uint8_t)offset, value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out_handle) {
    if (base > 0xFFFFU || len > 0x10000U - base) return UACPI_STATUS_INVALID_ARGUMENT;
    *out_handle = (uacpi_handle)(uintptr_t)(base + 1U);
    return UACPI_STATUS_OK;
}

void uacpi_kernel_io_unmap(uacpi_handle handle) { (void)handle; }

static uint16_t io_port(uacpi_handle handle, uacpi_size offset) {
    return (uint16_t)((uintptr_t)handle - 1U + offset);
}

uacpi_status uacpi_kernel_io_read8(uacpi_handle handle, uacpi_size offset, uacpi_u8 *out_value) {
    *out_value = inb(io_port(handle, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read16(uacpi_handle handle, uacpi_size offset, uacpi_u16 *out_value) {
    *out_value = inw(io_port(handle, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_read32(uacpi_handle handle, uacpi_size offset, uacpi_u32 *out_value) {
    *out_value = inl(io_port(handle, offset));
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write8(uacpi_handle handle, uacpi_size offset, uacpi_u8 in_value) {
    outb(io_port(handle, offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write16(uacpi_handle handle, uacpi_size offset, uacpi_u16 in_value) {
    outw(io_port(handle, offset), in_value);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_io_write32(uacpi_handle handle, uacpi_size offset, uacpi_u32 in_value) {
    outl(io_port(handle, offset), in_value);
    return UACPI_STATUS_OK;
}

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size ? size : 1U); }

void uacpi_kernel_free(void *mem) {
    if (mem) kfree(mem);
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return time_uptime_ns(); }

void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t until = time_uptime_ns() + (uint64_t)usec * 1000ULL;
    while (time_uptime_ns() < until) cpu_relax();
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    uint64_t until = time_uptime_ns() + msec * MILLISECOND;
    static const char sleep_channel;
    while (time_uptime_ns() < until) nap(&sleep_channel, until);
}

uacpi_handle uacpi_kernel_create_mutex(void) {
    struct sleeper *mutex = kmalloc(sizeof(*mutex));
    if (mutex) memset(mutex, 0, sizeof(*mutex));
    return mutex;
}

void uacpi_kernel_free_mutex(uacpi_handle handle) { kfree(handle); }

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle handle, uacpi_u16 timeout) {
    struct sleeper *mutex = handle;
    uint64_t deadline = deadline_after(timeout);
    for (;;) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&mutex->held, &expected, 1U, 0, __ATOMIC_ACQUIRE,
                                        __ATOMIC_RELAXED))
            return UACPI_STATUS_OK;
        if (!timeout || time_uptime_ns() >= deadline) return UACPI_STATUS_TIMEOUT;
        nap(mutex, slice_until(deadline));
    }
}

void uacpi_kernel_release_mutex(uacpi_handle handle) {
    struct sleeper *mutex = handle;
    __atomic_store_n(&mutex->held, 0U, __ATOMIC_RELEASE);
    (void)process_wake_all(mutex);
}

uacpi_handle uacpi_kernel_create_event(void) { return uacpi_kernel_create_mutex(); }

void uacpi_kernel_free_event(uacpi_handle handle) { kfree(handle); }

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle handle, uacpi_u16 timeout) {
    struct sleeper *event = handle;
    uint64_t deadline = deadline_after(timeout);
    for (;;) {
        uint32_t count = __atomic_load_n(&event->count, __ATOMIC_ACQUIRE);
        while (count) {
            if (__atomic_compare_exchange_n(&event->count, &count, count - 1U, 0, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return UACPI_TRUE;
        }
        if (!timeout || time_uptime_ns() >= deadline) return UACPI_FALSE;
        nap(event, slice_until(deadline));
    }
}

void uacpi_kernel_signal_event(uacpi_handle handle) {
    struct sleeper *event = handle;
    __atomic_add_fetch(&event->count, 1U, __ATOMIC_RELEASE);
    (void)process_wake_all(event);
}

void uacpi_kernel_reset_event(uacpi_handle handle) {
    struct sleeper *event = handle;
    __atomic_store_n(&event->count, 0U, __ATOMIC_RELEASE);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    struct process *self = process_current();
    return self ? (uacpi_thread_id)self : (uacpi_thread_id)&boot_thread;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) { return cpu_irq_save(); }

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) { cpu_irq_restore(state); }

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        kprintf("ACPI: firmware reported a fatal error type %u code %x\n",
                (unsigned)request->fatal.type, (unsigned)request->fatal.code);
    return UACPI_STATUS_OK;
}

static void interrupt_trampoline(void *context) {
    struct irq_slot *slot = context;
    __atomic_add_fetch(&interrupts_running, 1U, __ATOMIC_ACQ_REL);
    (void)slot->handler(slot->context);
    __atomic_sub_fetch(&interrupts_running, 1U, __ATOMIC_ACQ_REL);
}

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler,
                                                    uacpi_handle ctx,
                                                    uacpi_handle *out_irq_handle) {
    struct irq_slot *slot = kmalloc(sizeof(*slot));
    if (!slot) return UACPI_STATUS_OUT_OF_MEMORY;
    slot->handler = handler;
    slot->context = ctx;
    slot->vector = irq_request("acpi", "IO-APIC", interrupt_trampoline, slot);
    if (!slot->vector) {
        kfree(slot);
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    if (apic_route_global(irq, slot->vector) != 0) {
        irq_release(slot->vector);
        kfree(slot);
        return UACPI_STATUS_INTERNAL_ERROR;
    }
    *out_irq_handle = slot;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler,
                                                      uacpi_handle irq_handle) {
    (void)handler;
    struct irq_slot *slot = irq_handle;
    irq_release(slot->vector);
    kfree(slot);
    return UACPI_STATUS_OK;
}

uacpi_handle uacpi_kernel_create_spinlock(void) {
    struct spinlock *lock = kmalloc(sizeof(*lock));
    if (lock) lock->taken = 0;
    return lock;
}

void uacpi_kernel_free_spinlock(uacpi_handle handle) { kfree(handle); }

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle handle) { return spin_lock(handle); }

void uacpi_kernel_unlock_spinlock(uacpi_handle handle, uacpi_cpu_flags flags) {
    spin_unlock(handle, flags);
}

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler handler,
                                        uacpi_handle ctx) {
    struct work_ring *ring = type == UACPI_WORK_GPE_EXECUTION ? &gpe_ring : &notify_ring;
    uint64_t flags = spin_lock(&ring->lock);
    if (ring->count == RING_ITEMS) {
        spin_unlock(&ring->lock, flags);
        return UACPI_STATUS_OUT_OF_MEMORY;
    }
    ring->items[(ring->head + ring->count) % RING_ITEMS] = (struct work_item){handler, ctx};
    ring->count++;
    spin_unlock(&ring->lock, flags);
    (void)process_wake_all(ring);
    return UACPI_STATUS_OK;
}

static int ring_take(struct work_ring *ring, struct work_item *out) {
    uint64_t flags = spin_lock(&ring->lock);
    int found = ring->count != 0;
    if (found) {
        *out = ring->items[ring->head];
        ring->head = (ring->head + 1U) % RING_ITEMS;
        ring->count--;
        ring->busy = 1;
    }
    spin_unlock(&ring->lock, flags);
    return found;
}

static int ring_idle(struct work_ring *ring) {
    uint64_t flags = spin_lock(&ring->lock);
    int idle = !ring->count && !ring->busy;
    spin_unlock(&ring->lock, flags);
    return idle;
}

static void ring_worker(void *argument) {
    struct work_ring *ring = argument;
    for (;;) {
        struct work_item item;
        process_prepare_wait(ring, 0);
        if (!ring_take(ring, &item)) {
            process_wait();
            process_finish_wait();
            continue;
        }
        process_finish_wait();
        item.handler(item.context);
        __atomic_store_n(&ring->busy, 0U, __ATOMIC_RELEASE);
        (void)process_wake_all(&completion_channel);
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    while (__atomic_load_n(&interrupts_running, __ATOMIC_ACQUIRE))
        nap(&completion_channel, time_uptime_ns() + POLL_SLICE_NS);
    while (!ring_idle(&gpe_ring) || !ring_idle(&notify_ring))
        nap(&completion_channel, time_uptime_ns() + POLL_SLICE_NS);
    return UACPI_STATUS_OK;
}

int acpi_host_start(void) {
    struct process *gpe = process_create_kthread("kacpi-gpe", ring_worker, &gpe_ring);
    struct process *notify = process_create_kthread("kacpi-notify", ring_worker, &notify_ring);
    if (!gpe || !notify) return -1;
    struct cpu_mask first = {0};
    cpu_mask_set(&first, 0);
    (void)process_set_affinity(gpe->pid, &first);
    return 0;
}
