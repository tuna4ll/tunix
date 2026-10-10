#include <stdint.h>

#include <tunix/acpi.h>
#if defined(__x86_64__)
#include <tunix/ata.h>
#endif
#include <tunix/cpu.h>
#include <tunix/ext2.h>
#include <tunix/input.h>
#include <tunix/vfs.h>
#include <tunix/platform.h>
#include <tunix/power.h>
#include <tunix/workqueue.h>

extern void kprintf(const char *fmt, ...);

static void flush_disks(void) {
    (void)vfs_sync();
    (void)ext2fs_shutdown();
#if defined(__x86_64__)
    (void)ata_flush_cache();
#endif
}

static void park(void) __attribute__((noreturn));
static void park(void) { cpu_halt_forever(); }

void power_off(void) {
    kprintf("POWER: flushing and powering off\n");
    flush_disks();
    arch_power_off();
    kprintf("POWER: the machine did not power off; halted\n");
    park();
}

void power_restart(void) {
    kprintf("POWER: flushing and restarting\n");
    flush_disks();
    arch_restart();
}

void power_halt(void) {
    kprintf("POWER: flushing and halting\n");
    flush_disks();
    kprintf("POWER: system halted\n");
    park();
}

static void power_off_from_work(void *unused) {
    (void)unused;
    power_off();
}

static void button_from_work(void *unused) {
    (void)unused;
    if (input_report_power_button()) return;
    kprintf("POWER: power button, nobody listens for it\n");
    power_off();
}

static struct work button_work = WORK_INITIALIZER(button_from_work, NULL);

static struct work critical_work = WORK_INITIALIZER(power_off_from_work, NULL);

void power_critical(void) {
    kprintf("POWER: critical temperature\n");
    work_queue(&critical_work);
}

void power_button_pressed(void) { work_queue(&button_work); }
