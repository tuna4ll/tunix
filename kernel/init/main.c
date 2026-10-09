#include <stdint.h>
#include <tunix/workqueue.h>
#include <tunix/boot.h>
#include <tunix/build_config.h>
#include <tunix/block.h>
#include <tunix/cpu.h>
#include <tunix/kstring.h>
#include <tunix/devfs.h>
#include <tunix/sysfs.h>
#include <tunix/framebuffer.h>
#include <tunix/heap.h>
#include <tunix/hwreport.h>
#include <tunix/nv50.h>
#include <tunix/input.h>
#include <tunix/ehci.h>
#include <tunix/eventfs.h>
#include <tunix/net/net.h>
#include <tunix/platform.h>
#include <tunix/pmm.h>
#include <tunix/process.h>
#include <tunix/procfs.h>
#include <tunix/random.h>
#include <tunix/syscall.h>
#include <tunix/ext2.h>
#include <tunix/time.h>
#include <tunix/timer.h>
#include <tunix/tty.h>
#include <tunix/vt.h>
#include <tunix/vfs.h>
#include <tunix/terminal.h>
#include <tunix/vmm.h>
#include <tunix/acpi.h>
#include <tunix/smp.h>
#include <tunix/virtgpu.h>
#include <tunix/xhci.h>

extern void kprintf(const char *fmt, ...);
extern void panic(const char *message);

#if TUNIX_BOOT_TIMINGS
static void boot_log_cycles(const char *name, uint64_t cycles) {
    uint64_t hz = time_tsc_frequency();
    uint64_t milliseconds = hz ? (cycles * 1000ULL) / hz : 0;
    kprintf("BOOTPERF: %s %u ms\n", name, (unsigned)milliseconds);
}

static void boot_log_stage(const char *name, uint64_t *started) {
    uint64_t now = cpu_counter_ordered();
    boot_log_cycles(name, now - *started);
    *started = now;
}
#endif

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
    uint64_t boot_started = cpu_counter_ordered();
#endif
    cpu_irq_disable();
    arch_early_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: boot regions=%u cmdline=\"%s\"\n", boot->memory_count, boot->command_line);
#endif

    arch_cpu_init();
    process_enable_extended_fpu();
    time_init();
#if TUNIX_BOOT_TIMINGS
    uint64_t stage_started = cpu_counter_ordered();
#endif
    random_init();
    pmm_init(boot->memory, boot->memory_count);
    vmm_init();
#if defined(__x86_64__)
    if (!boot->framebuffer) panic("no framebuffer from the bootloader");
#endif
    static struct boot_framebuffer_info native;
    const struct boot_framebuffer_info *console = boot->framebuffer;
    if (console && nv50_early_init(console, &native) == 0) console = &native;
    if (console && framebuffer_init(console) != 0) panic("framebuffer initialization failed");
    heap_init();
    if (terminal_init() != 0) panic("framebuffer terminal initialization failed");
    vt_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("terminal initialization", &stage_started);
#endif
    arch_probe_buses();
    acpi_describe_machine();
    net_init();
    (void)xhci_init();
    (void)ehci_init();
    (void)virtgpu_init();
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("memory/framebuffer/network init", &stage_started);
#endif
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: GDT/TSS IDT PMM VMM heap ready\n");
#endif

    block_probe();
    block_select_root(root_device_index());

    vfs_init();
    if (ext2fs_mount_root() != 0) {
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
    vfs_mount_builtin(root ? source : "none", "/", ext2fs_journalled(vfs_root) ? "ext3" : "ext2",
                      vfs_root);
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("root filesystem mount", &stage_started);
#endif
    input_init();
    arch_route_legacy_interrupts();
    net_enable_interrupts();
    devfs_init();
    sysfs_init();
#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: VFS rootfs devfs ready\n");
#endif

    process_init();
    eventfs_init();
    procfs_init();
    syscall_init();
    static char init_path[VFS_PATH_MAX] = "/sbin/init";
    const char *requested = boot_command_line_value("init");
    if (requested) {
        size_t length = 0;
        while (requested[length] && requested[length] != ' ' && length < sizeof init_path - 1) {
            init_path[length] = requested[length];
            length++;
        }
        init_path[length] = '\0';
    }
    if (!process_create_from_path(init_path)) {
        kprintf("TUNIX: cannot start %s\n", init_path);
        panic("no init");
    }
    workqueue_init();
    vfs_start_writeback();
    ext2fs_start();
    timer_init();
    arch_route_timer();
    smp_init();
    sysfs_publish_cpus(percpu_online_count());
    hwreport_emit();
    kprintf("TUNIX: starting %s\n", init_path);
#if TUNIX_BOOT_TIMINGS
    boot_log_stage("devices/process/init ELF", &stage_started);
    boot_log_cycles("kernel boot total", cpu_counter_ordered() - boot_started);
#endif

#if TUNIX_DEBUG_LOGS
    kprintf("TUNIX: entering userspace\n");
#endif
    process_start_first();
}
