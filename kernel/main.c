#include <stdint.h>
#include "include/ata.h"
#include "include/boot.h"
#include "include/build_config.h"
#include "include/block.h"
#include "include/kstring.h"
#include "include/devfs.h"
#include "include/sysfs.h"
#include "include/gdt.h"
#include "include/framebuffer.h"
#include "include/heap.h"
#include "include/hwreport.h"
#include "include/klock.h"
#include "include/input.h"
#include "include/ehci.h"
#include "include/idt.h"
#include "include/net/net.h"
#include "include/pmm.h"
#include "include/pic.h"
#include "include/process.h"
#include "include/procfs.h"
#include "include/random.h"
#include "include/syscall.h"
#include "include/ext2.h"
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

/* root= names the device the filesystem is on: LABEL=tunix-root asks for the
   disk whose ext2 label says so and is the only form that survives being
   plugged into a machine with disks of its own; /dev/sda2 names a position
   in the probe order. Without either, the first disk registered. */
static int root_device_index(void) {
    const char *value = boot_command_line_value("root");
    if (!value) return 0;

    char name[40];
    size_t length = 0;
    while (value[length] && value[length] != ' ' && length < sizeof name - 1) {
        name[length] = value[length];
        length++;
    }
    name[length] = '\0';

    int index;
    if (strncmp(name, "LABEL=", 6) == 0) index = ext2fs_find_label(name + 6);
    else index = block_device_index_by_name(name);
    if (index < 0) kprintf("TUNIX: root=%s names no device\n", name);
    return index < 0 ? 0 : index;
}

void kmain(const struct boot_info *boot) {
#if TUNIX_BOOT_TIMINGS
    uint64_t boot_started = boot_read_tsc();
#endif
    __asm__ volatile("cli");
    pic_init();
    serial_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: boot regions=%u cmdline=\"%s\"\n", boot->memory_count,
            boot->command_line);
#endif

    gdt_init();
    idt_init();
    time_init();
#if TUNIX_BOOT_TIMINGS
    uint64_t stage_started = boot_read_tsc();
#endif
    random_init();
    pmm_init(boot->memory, boot->memory_count);
    vmm_init();
    /* Fatal: the kernel draws its console into the framebuffer and has no
       other way to say anything to whoever is looking at the machine. */
    if (!boot->framebuffer) panic("no framebuffer from the bootloader");
    if (framebuffer_init(boot->framebuffer) != 0) panic("framebuffer initialization failed");
    heap_init();
    /* The console comes up here rather than after the root filesystem, and the
       reason is what a failure looks like from the outside. panic() writes to
       the terminal; with the terminal built later, every panic between this
       line and the mount -- no disk, no root, an unreadable superblock --
       reached the serial port and left the screen black. On a machine with no
       serial cable that is indistinguishable from a hang. */
    if (terminal_init() != 0)
        panic("framebuffer terminal initialization failed");
    /* The first virtual terminal, and the display handed to it. Everything
       written to a console from here on lands on a terminal that exists. */
    vt_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("terminal initialization", &stage_started);
#endif
    acpi_describe_machine();
    net_init();
    /* A machine with no PS/2 port has its keyboard here; one that has both
       ends up with two, which the input layer already copes with. Absent or
       broken is not fatal -- the rest of the system does not depend on it. */
    (void)xhci_init();
    /* The other one. A machine has xHCI or EHCI or both, and on the machines
       that have both the disks are usually behind the newer one -- but the
       stick this kernel was booted from is behind whichever the firmware used,
       so neither can be assumed away. */
    (void)ehci_init();
    /* Absent on a machine with a plain VGA adapter, in which case drm.c keeps
       blitting into the framebuffer the bootloader handed over. */
    (void)virtgpu_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("memory/framebuffer/network init", &stage_started);
#endif
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: GDT/TSS IDT PMM VMM heap ready\n");
#endif

    /* Every storage controller at once, and only here: two of the three are
       memory mapped, so they need the page tables and the allocator that the
       lines above just finished building, and the USB disks hang off the
       controller started a few lines earlier. */
    block_probe();
    block_select_root(root_device_index());

    vfs_init();
    /* Offset zero: a partition is its own device here, so the filesystem
       starts where the device does. */
    if (ext2fs_mount_root(0) != 0) {
        /* The inventory, not just the failure. On real hardware the answer is
           nearly always that nothing here can read the disk the root is on --
           a USB stick behind a controller with no driver, say -- and the way
           to tell that apart from a corrupt filesystem is which devices the
           probe did register. */
        terminal_print("\nblock devices:");
        int found = block_device_count();
        for (int index = 0; index < found; index++) {
            const struct block_device *device = block_device_at(index);
            terminal_print(" ");
            terminal_print(device->dev_name);
        }
        if (!found) terminal_print(" none");
        terminal_print("\n");
        panic("root filesystem mount failed");
    }
    const struct block_device *root = block_root();
    char source[5 + BLOCK_NAME_BYTES] = "/dev/";
    if (root) memcpy(source + 5, root->dev_name, sizeof root->dev_name);
    vfs_mount_builtin(root ? source : "none", "/", "ext2", vfs_root);
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("root filesystem mount", &stage_started);
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
    /* And the network adapter's own line, for the same reason: it is routed
       through the controller this just brought up. */
    net_enable_interrupts();
    devfs_init();
    /* After devfs: the entries describe the devices it just attached. */
    sysfs_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: VFS rootfs devfs ready\n");
#endif

    process_init();
    procfs_init();
    syscall_init();
    /* init= names the first program, so a machine that will not finish booting
       can be pointed at a shell or a single command instead of its init. */
    char init_path[128] = "/sbin/init";
    const char *requested = boot_command_line_value("init");
    if (requested) {
        size_t length = 0;
        while (requested[length] && requested[length] != ' ' &&
               length < sizeof init_path - 1) {
            init_path[length] = requested[length];
            length++;
        }
        init_path[length] = '\0';
    }
    if (!process_create_from_path(init_path)) {
        kprintf("TUNIX: cannot start %s\n", init_path);
        panic("no init");
    }
    timer_init();
    if (apic_is_active()) apic_route_legacy_irq(0U); else pic_unmask(0U);
    /* Last, and after the timer: the processors this starts come up idle and
       start taking work off the queue immediately, so everything they might
       touch has to already exist. */
    smp_init();
    /* After the processors are up, because most of what it has to say is about
       them, and before init, because a machine that will not get that far is
       exactly the one worth asking. */
    hwreport_emit();
    /* Before init, because the holds worth catching are the ones a startup
       makes: by the time a shell exists to ask for the measurement, the part
       that froze is over. */
    if (boot_command_line_flag("klockstat")) klock_statistics_start();
    /* The last thing the kernel says on its own behalf. Everything after it
       on the console comes from init. */
    kprintf("TUNIX: starting %s\n", init_path);
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("devices/process/init ELF", &stage_started);
    boot_log_cycles("kernel boot total", boot_read_tsc() - boot_started);
#endif

#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: entering userspace\n");
#endif
    process_start_first();
}
