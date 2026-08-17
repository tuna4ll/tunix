#include <stdint.h>
#include "include/ata.h"
#include "include/build_config.h"
#include "include/boot_manifest.h"
#include "include/boot_framebuffer.h"
#include "include/devfs.h"
#include "include/sysfs.h"
#include "include/sysfs.h"
#include "include/gdt.h"
#include "include/framebuffer.h"
#include "include/heap.h"
#include "include/input.h"
#include "include/idt.h"
#include "include/net/net.h"
#include "include/pmm.h"
#include "include/pic.h"
#include "include/process.h"
#include "include/procfs.h"
#include "include/random.h"
#include "include/syscall.h"
#include "include/ext2.h"
#include "include/tarfs.h"
#include "include/time.h"
#include "include/timer.h"
#include "include/tty.h"
#include "include/vt.h"
#include "include/vfs.h"
#include "include/terminal.h"
#include "include/vmm.h"
#include "include/acpi.h"
#include "include/apic.h"
#include "include/smp.h"
#include "include/virtgpu.h"
#include "include/xhci.h"

#define INITRAMFS_PHYSICAL 0x02000000ULL

extern void serial_init(void);
extern void kprintf(const char *fmt, ...);
extern void panic(const char *message);

static inline uint64_t boot_read_tsc(void) {
    uint32_t low;
    uint32_t high;
    __asm__ volatile("lfence; rdtsc" : "=a"(low), "=d"(high) : : "memory");
    return ((uint64_t)high << 32) | low;
}

#if TUNIX_BOOT_TIMINGS
static void boot_log_cycles(const char *name, uint64_t cycles) {
    uint64_t hz = time_tsc_frequency();
    uint64_t milliseconds = hz ? (cycles * 1000ULL) / hz : 0;
    kprintf("BOOTPERF: %s %u ms\n", name, (unsigned)milliseconds);
}

static void boot_log_stage(const char *name, uint64_t *started) {
    uint64_t now = boot_read_tsc();
    boot_log_cycles(name, now - *started);
    *started = now;
}
#endif

static int initramfs_used_dma;
static uint32_t data_region_lba;

static uint32_t compute_data_region_lba(const struct boot_manifest *manifest) {
    if (!manifest || manifest->magic != TUNIX_MANIFEST_MAGIC ||
        manifest->version != TUNIX_MANIFEST_VERSION) return 0;
    uint64_t end = manifest->initramfs_lba + manifest->initramfs_sectors;
    uint64_t aligned = (end + TUNIX_DATA_REGION_ALIGN_SECTORS - 1ULL) &
                       ~(TUNIX_DATA_REGION_ALIGN_SECTORS - 1ULL);
    return aligned > 0x0FFFFFFFULL ? 0 : (uint32_t)aligned;
}

static uint64_t load_initramfs(const struct boot_manifest *manifest) {
    if (!manifest || manifest->magic != TUNIX_MANIFEST_MAGIC ||
        manifest->version != TUNIX_MANIFEST_VERSION ||
        manifest->size < sizeof(*manifest)) {
        panic("invalid boot manifest");
    }
    if (!manifest->initramfs_size || manifest->initramfs_size > TUNIX_INITRAMFS_MAX_BYTES) {
        panic("invalid initramfs size");
    }
    if (manifest->initramfs_lba > 0x0FFFFFFFULL || manifest->initramfs_sectors > TUNIX_INITRAMFS_MAX_SECTORS) {
        panic("initramfs outside ATA28 limits");
    }
    initramfs_used_dma =
        ata_dma_read28((uint32_t)manifest->initramfs_lba,
                       manifest->initramfs_sectors,
                       (void *)INITRAMFS_PHYSICAL) == 0;
    if (!initramfs_used_dma &&
        ata_pio_read28((uint32_t)manifest->initramfs_lba,
                       manifest->initramfs_sectors,
                       (void *)INITRAMFS_PHYSICAL) != 0) {
        panic("ATA initramfs load failed");
    }
    return manifest->initramfs_size;
}

/*
 * Give the archive back once the disk can answer for what was in it.
 *
 * The initramfs is half a gigabyte on this image and every file in the tree
 * points into it rather than owning a copy, so it has to stay reserved until
 * those pointers are gone. Seeding gives each file an inode, which is what
 * makes cutting them loose safe: the next read fetches from the disk instead.
 * Only the first boot pays this at all -- once there is a root on the disk the
 * archive is never loaded.
 */
static void release_initramfs(uint64_t initramfs_size) {
    if (!initramfs_size) return;
    uint64_t detached = vfs_detach_static_data(vfs_root);
    uint64_t pages = pmm_release_reserved(INITRAMFS_PHYSICAL, initramfs_size);
    kprintf("TUNIX: released initramfs, %u MiB from %u files\n",
            (unsigned)(pages * 4096ULL / (1024 * 1024)), (unsigned)detached);
}

void kmain(uint32_t mmap_count, uint64_t mmap_address, uint64_t manifest_address,
           uint64_t framebuffer_info_address) {
#if TUNIX_BOOT_TIMINGS
    uint64_t boot_started = boot_read_tsc();
    uint64_t initramfs_started = boot_started;
#endif
    __asm__ volatile("cli");
    pic_init();
    serial_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: boot mmap=%u manifest=%p\n", mmap_count, (void *)manifest_address);
#endif

    const struct boot_manifest *manifest = (const struct boot_manifest *)manifest_address;
    if (!manifest || manifest->magic != TUNIX_MANIFEST_MAGIC) {
        manifest = (const struct boot_manifest *)0x00020000ULL;
    }
    data_region_lba = compute_data_region_lba(manifest);
    int root_on_disk = data_region_lba && ext2fs_probe(data_region_lba) == 0;
    uint64_t initramfs_size = root_on_disk ? 0 : load_initramfs(manifest);
#if TUNIX_BOOT_TIMINGS
    uint64_t initramfs_cycles = boot_read_tsc() - initramfs_started;
#endif
#if TUNIX_DEBUG_LOGS
    if (root_on_disk)
        kprintf("TUNIX: ext2 root found on disk, skipping initramfs\n");
    else
        kprintf("TUNIX: initramfs loaded from ATA, %u bytes\n", (unsigned)initramfs_size);
#endif

    gdt_init();
    idt_init();
    time_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_cycles(initramfs_used_dma ? "initramfs ATA DMA load" :
                                      "initramfs ATA PIO fallback load",
                    initramfs_cycles);
    uint64_t stage_started = boot_read_tsc();
#endif
    random_init();
    pmm_init(mmap_count, mmap_address, INITRAMFS_PHYSICAL, initramfs_size);
    vmm_init();
    if (!framebuffer_info_address) panic("missing framebuffer boot information");
    const struct boot_framebuffer_info *framebuffer_info =
        (const struct boot_framebuffer_info *)vmm_phys_to_virt(framebuffer_info_address);
    if (framebuffer_init(framebuffer_info) != 0) panic("framebuffer initialization failed");
    heap_init();
    acpi_describe_machine();
    net_init();
    /* A machine with no PS/2 port has its keyboard here; one that has both
       ends up with two, which the input layer already copes with. Absent or
       broken is not fatal -- the rest of the system does not depend on it. */
    (void)xhci_init();
    /* Absent on a machine with a plain VGA adapter, in which case drm.c keeps
       blitting into the framebuffer the bootloader handed over. */
    (void)virtgpu_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("memory/framebuffer/network init", &stage_started);
#endif
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: GDT/TSS IDT PMM VMM heap ready\n");
#endif

    vfs_init();
    if (root_on_disk) {
        if (ext2fs_mount_root(data_region_lba) != 0)
            panic("persistent root filesystem load failed");
    } else {
        if (tarfs_unpack(INITRAMFS_PHYSICAL, initramfs_size) < 0)
            panic("initramfs unpack failed");
        /* Only a seed that worked lets the archive go: without one the tree is
           still the only copy of the files and still points into it. */
        if (data_region_lba && ext2fs_seed_root(data_region_lba) == 0)
            release_initramfs(initramfs_size);
        else
            kprintf("TUNIX: root persistence unavailable, running from RAM\n");
    }
    /* Declared once the outcome is known: the root is the ext2 volume only if
       mounting or seeding it actually worked. */
    vfs_mount_builtin(ext2fs_mounted() ? "/dev/sda" : "initramfs", "/",
                      ext2fs_mounted() ? "ext2" : "ramfs", vfs_root);
#if TUNIX_BOOT_TIMINGS
    boot_log_stage(root_on_disk ? "ext2 root load" : "initramfs VFS indexing + ext2 seed",
                   &stage_started);
#endif
    if (terminal_init() != 0)
        panic("framebuffer terminal initialization failed");
    /* The first virtual terminal, and the display handed to it. Everything
       written to a console from here on lands on a terminal that exists. */
    vt_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("terminal initialization", &stage_started);
#endif
    input_init();
    /* Delivery moves here, after the handlers exist and before anything is
       unmasked: the routing below has to go to whichever controller is live. */
    int apic = apic_init() == 0;
    if (apic) apic_route_legacy_irq(1U);
    else pic_unmask(1U);
    if (input_mouse_available()) {
        if (apic) apic_route_legacy_irq(12U); else pic_unmask(12U);
    }
    /* The power button, which is an ACPI event rather than a line the PIC ever
       carried: it needs the IOAPIC, so a machine that stayed on the 8259 pair
       simply does not get one. */
    if (apic) acpi_power_button_enable(ACPI_SCI_VECTOR);
    devfs_init();
    /* After devfs: the entries describe the devices it just attached. */
    sysfs_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: VFS rootfs devfs ready\n");
#endif

    process_init();
    procfs_init();
    syscall_init();
    if (!process_create_from_path("/sbin/init")) panic("cannot create /sbin/init");
    timer_init();
    if (apic_is_active()) apic_route_legacy_irq(0U); else pic_unmask(0U);
    /* Last, and after the timer: the processors this starts come up idle and
       start taking work off the queue immediately, so everything they might
       touch has to already exist. */
    smp_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("devices/process/init ELF", &stage_started);
    boot_log_cycles("kernel boot total", boot_read_tsc() - boot_started);
#endif

#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: entering userspace\n");
#endif
    process_start_first();
}
