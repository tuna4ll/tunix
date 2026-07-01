#include <stdint.h>
#include "include/ata.h"
#include "include/build_config.h"
#include "include/boot_manifest.h"
#include "include/boot_framebuffer.h"
#include "include/devfs.h"
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
#include "include/tarfs.h"
#include "include/time.h"
#include "include/tty.h"
#include "include/vfs.h"
#include "include/terminal.h"
#include "include/vmm.h"

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
    uint64_t initramfs_size = load_initramfs(manifest);
#if TUNIX_BOOT_TIMINGS
    uint64_t initramfs_cycles = boot_read_tsc() - initramfs_started;
#endif
#if TUNIX_DEBUG_LOGS
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
    net_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("memory/framebuffer/network init", &stage_started);
#endif
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: GDT/TSS IDT PMM VMM heap ready\n");
#endif

    vfs_init();
    if (tarfs_unpack(INITRAMFS_PHYSICAL, initramfs_size) < 0) panic("initramfs unpack failed");
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("initramfs VFS indexing", &stage_started);
#endif
    if (terminal_init("/usr/share/tunix/wallpaper.twl") != 0)
        panic("framebuffer terminal initialization failed");
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("terminal initialization", &stage_started);
#endif
    tty_init();
    input_init();
    pic_unmask(1U);
    devfs_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: VFS ramfs tarfs devfs ready\n");
#endif

    process_init();
    procfs_init();
    syscall_init();
    if (!process_create_from_path("/sbin/init")) panic("cannot create /sbin/init");
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("devices/process/init ELF", &stage_started);
    boot_log_cycles("kernel boot total", boot_read_tsc() - boot_started);
#endif

#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: entering userspace\n");
#endif
    process_start_first();
}
