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
#include "include/pmm.h"
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

static inline void outb_local(uint16_t port, uint8_t value) {
    __asm__ volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static void disable_legacy_pic(void) {
    outb_local(0x21, 0xFF);
    outb_local(0xA1, 0xFF);
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
    if (ata_pio_read28((uint32_t)manifest->initramfs_lba,
                       manifest->initramfs_sectors,
                       (void *)INITRAMFS_PHYSICAL) != 0) {
        panic("ATA PIO initramfs load failed");
    }
    return manifest->initramfs_size;
}

void kmain(uint32_t mmap_count, uint64_t mmap_address, uint64_t manifest_address,
           uint64_t framebuffer_info_address) {
    __asm__ volatile("cli");
    disable_legacy_pic();
    serial_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: boot mmap=%u manifest=%p\n", mmap_count, (void *)manifest_address);
#endif

    const struct boot_manifest *manifest = (const struct boot_manifest *)manifest_address;
    if (!manifest || manifest->magic != TUNIX_MANIFEST_MAGIC) {
        manifest = (const struct boot_manifest *)0x00020000ULL;
    }
    uint64_t initramfs_size = load_initramfs(manifest);
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: initramfs loaded from ATA, %u bytes\n", (unsigned)initramfs_size);
#endif

    gdt_init();
    idt_init();
    time_init();
    random_init();
    pmm_init(mmap_count, mmap_address, INITRAMFS_PHYSICAL, initramfs_size);
    vmm_init();
    if (!framebuffer_info_address) panic("missing framebuffer boot information");
    const struct boot_framebuffer_info *framebuffer_info =
        (const struct boot_framebuffer_info *)vmm_phys_to_virt(framebuffer_info_address);
    if (framebuffer_init(framebuffer_info) != 0) panic("framebuffer initialization failed");
    heap_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: GDT/TSS IDT PMM VMM heap ready\n");
#endif

    vfs_init();
    if (tarfs_unpack(INITRAMFS_PHYSICAL, initramfs_size) < 0) panic("initramfs unpack failed");
    if (terminal_init("/usr/share/tunix/wallpaper.twl") != 0)
        panic("framebuffer terminal initialization failed");
    tty_init();
    input_init();
    devfs_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: VFS ramfs tarfs devfs ready\n");
#endif

    process_init();
    procfs_init();
    syscall_init();
    if (!process_create_from_path("/sbin/init")) panic("cannot create /sbin/init");

#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: entering userspace\n");
#endif
    process_start_first();
}
