#include <stdint.h>

#include "include/acpi.h"
#include "include/ata.h"
#include "include/ext2.h"
#include "include/power.h"

extern void kprintf(const char *fmt, ...);

/*
 * The kernel acts on the power button by default.
 *
 * dinit calls reboot(RB_DISABLE_CAD) as it starts, which on Linux means "stop
 * turning ctrl-alt-del into a reset and send me a signal instead". There is no
 * signal for the power button here and nothing listening for one, so what that
 * turns off is the kernel acting on the press at all -- which is exactly what
 * a system with its own power management wants, and leaves the button inert
 * until something takes it over.
 */
static int button_handled = 1;

/*
 * Everything written but not yet on the disk.
 *
 * Writes reach ext2 as they happen, so this is the metadata and the drive's own
 * cache rather than a writeback cache of file contents -- which is why it is a
 * flush and not a walk of the tree, and why it finishes in milliseconds.
 */
static void flush_disks(void) {
    (void)ext2fs_sync();
    (void)ata_flush_cache();
}

/* Stop this processor for good. The others are in their idle loops, halted
   between interrupts, which is as stopped as they can be made without an
   interprocessor call this has no way to wait for. */
static void park(void) __attribute__((noreturn));
static void park(void) {
    for (;;) __asm__ volatile("cli; hlt");
}

void power_off(void) {
    kprintf("POWER: flushing and powering off\n");
    flush_disks();
    acpi_power_off();
    /* Only reached when the firmware declined or described no sleep state.
       Saying so matters: the machine is about to look like it hung. */
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
