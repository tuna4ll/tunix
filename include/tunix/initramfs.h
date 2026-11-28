#ifndef TUNIX_INITRAMFS_H
#define TUNIX_INITRAMFS_H

// Loads the modules of the bootloader's initramfs that this machine's devices
// ask for, and those rd.driver.pre= names; returns how many loaded.
int initramfs_load_modules(void);

#endif
