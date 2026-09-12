#include <stdint.h>

#include "include/acpi.h"
#include "include/ata.h"
#include "include/ext2.h"
#include "include/power.h"

extern void kprintf(const char *fmt, ...);

static int button_handled = 1;

static void flush_disks(void) {
    (void)ext2fs_sync();
    (void)ext2fs_shutdown();
    (void)ata_flush_cache();
}

static void park(void) __attribute__((noreturn));
static void park(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

void power_off(void) {
    kprintf("POWER: flushing and powering off\n");
    flush_disks();
    acpi_power_off();
    kprintf("POWER: the machine did not power off; halted\n");
    park();
}

void power_restart(void) {
    kprintf("POWER: flushing and restarting\n");
    flush_disks();
    acpi_reset();
}

void power_halt(void) {
    kprintf("POWER: flushing and halting\n");
    flush_disks();
    kprintf("POWER: system halted\n");
    park();
}

void power_set_button_handled(int handled) {
    button_handled = handled != 0;
}

void power_button_pressed(void) {
    if (!button_handled) return;
    kprintf("POWER: power button\n");
    power_off();
}
