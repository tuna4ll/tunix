#include <stdint.h>

#include "arch.h"

#define SYS_WRITE      64
#define SYS_EXIT       93
#define SYS_EXIT_GROUP 94

#define ENOSYS 38
#define EBADF  9

void aarch64_syscall_handler(struct syscall_frame *frame) {
    uint64_t number = SYSCALL_NR(frame);

    switch (number) {
    case SYS_WRITE: {
        uint64_t fd = SYSCALL_ARG0(frame);
        const char *buffer = (const char *)SYSCALL_ARG1(frame);
        uint64_t length = SYSCALL_ARG2(frame);
        if (fd != 1 && fd != 2) {
            SYSCALL_RET(frame) = (uint64_t)-EBADF;
            return;
        }
        for (uint64_t i = 0; i < length; i++) uart_putc(buffer[i]);
        SYSCALL_RET(frame) = length;
        return;
    }
    case SYS_EXIT:
    case SYS_EXIT_GROUP:
        kprintf("[aarch64] EL0 task exited with status %lu\n", SYSCALL_ARG0(frame));
        aarch64_leave_user();
        return;                                      // not reached
    default:
        kprintf("[aarch64] unimplemented syscall %lu from EL0\n", number);
        SYSCALL_RET(frame) = (uint64_t)-ENOSYS;
        return;
    }
}
