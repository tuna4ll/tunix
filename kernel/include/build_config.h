#ifndef TUNIX_BUILD_CONFIG_H
#define TUNIX_BUILD_CONFIG_H

/* Both are off in a shipped kernel and both are worth turning on from the
   command line -- make KERNEL_CFLAGS_EXTRA=-DTUNIX_DEBUG_LOGS=1 -- which is
   why they are defaults rather than definitions. */
#ifndef TUNIX_DEBUG_LOGS
#define TUNIX_DEBUG_LOGS 0
#endif

#ifndef TUNIX_BOOT_TIMINGS
#define TUNIX_BOOT_TIMINGS 0
#endif

#endif
