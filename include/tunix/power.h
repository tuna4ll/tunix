#ifndef TUNIX_POWER_H
#define TUNIX_POWER_H

void power_off(void) __attribute__((noreturn));
void power_restart(void) __attribute__((noreturn));
void power_halt(void) __attribute__((noreturn));

void power_button_pressed(void);
void power_critical(void);

#endif
