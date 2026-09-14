#include <stdint.h>

#include "arch.h"

#define SYS_WRITE      64
#define SYS_EXIT       93
#define SYS_EXIT_GROUP 94

#define ENOSYS 38

void aarch64_syscall_handler(struct trap_frame *frame) {
    uint64_t number = frame->x[8];

    switch (number) {
    case SYS_WRITE: {
        uint64_t fd = frame->x[0];
        const char *buffer = (const char *)frame->x[1];
        uint64_t length = frame->x[2];
        if (fd != 1 && fd != 2) {
            frame->x[0] = (uint64_t)-9;              // -EBADF
            return;
        }
        for (uint64_t i = 0; i < length; i++) uart_putc(buffer[i]);
        frame->x[0] = length;
        return;
    }
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        kprintf("[aarch64] EL0 task exited with status %lu\n", frame->x[0]);
        aarch64_leave_user();
        return;                                      // not reached
    default:
        kprintf("[aarch64] unimplemented syscall %lu from EL0\n", number);
        frame->x[0] = (uint64_t)-ENOSYS;
        return;
    }
}
