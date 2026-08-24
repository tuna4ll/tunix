#ifndef TUNIX_POWER_H
#define TUNIX_POWER_H

/*
 * What the machine does when it is asked to stop.
 *
 * Above ACPI rather than part of it: the tables say which port turns the
 * machine off, but not that the disk has to be flushed first or what to do
 * when the firmware declines. Two callers want that sequence -- the reboot(2)
 * syscall and the power button's interrupt -- which is why it is here and not
 * inside either of them.
 */

/* Flush what is not on the disk yet, then enter S5. Falls back to halting when
   the machine has no usable sleep state. */
void power_off(void) __attribute__((noreturn));
/* Flush, then reset: the ACPI register if the firmware describes one, the
   keyboard controller if not, and a triple fault as the last resort. */
void power_restart(void) __attribute__((noreturn));
/* Flush and stop, leaving the machine powered. */
void power_halt(void) __attribute__((noreturn));

/* Whether the power button should stop the machine. reboot(2)'s CAD_ON and
   CAD_OFF set it, following what those commands mean on Linux: the kernel
   handles the request itself, or leaves it to whoever is listening. */
void power_set_button_handled(int handled);
/* Called from the SCI. Acts on the press, or ignores it. */
void power_button_pressed(void);

#endif
