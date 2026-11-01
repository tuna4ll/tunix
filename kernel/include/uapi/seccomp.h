#ifndef TUNIX_UAPI_SECCOMP_H
#define TUNIX_UAPI_SECCOMP_H

#include <stdint.h>

#define SECCOMP_MODE_DISABLED 0
#define SECCOMP_MODE_STRICT   1
#define SECCOMP_MODE_FILTER   2

#define SECCOMP_SET_MODE_STRICT  0
#define SECCOMP_SET_MODE_FILTER  1
#define SECCOMP_GET_ACTION_AVAIL 2
#define SECCOMP_GET_NOTIF_SIZES  3

#define SECCOMP_FILTER_FLAG_TSYNC        (1UL << 0)
#define SECCOMP_FILTER_FLAG_LOG          (1UL << 1)
#define SECCOMP_FILTER_FLAG_SPEC_ALLOW   (1UL << 2)
#define SECCOMP_FILTER_FLAG_NEW_LISTENER (1UL << 3)
#define SECCOMP_FILTER_FLAG_TSYNC_ESRCH  (1UL << 4)

#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_KILL         SECCOMP_RET_KILL_THREAD
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_USER_NOTIF   0x7fc00000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U

#define SECCOMP_RET_ACTION_FULL 0xffff0000U
#define SECCOMP_RET_ACTION      0x7fff0000U
#define SECCOMP_RET_DATA        0x0000ffffU

struct seccomp_data {
    int32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
};

struct sock_filter {
    uint16_t code;
    uint8_t jt;
    uint8_t jf;
    uint32_t k;
};

struct sock_fprog {
    uint16_t len;
    uint64_t filter;
};

#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_LD          0x00
#define BPF_LDX         0x01
#define BPF_ST          0x02
#define BPF_STX         0x03
#define BPF_ALU         0x04
#define BPF_JMP         0x05
#define BPF_RET         0x06
#define BPF_MISC        0x07

#define BPF_SIZE(code) ((code) & 0x18)
#define BPF_W          0x00
#define BPF_H          0x08
#define BPF_B          0x10

#define BPF_MODE(code) ((code) & 0xe0)
#define BPF_IMM        0x00
#define BPF_ABS        0x20
#define BPF_IND        0x40
#define BPF_MEM        0x60
#define BPF_LEN        0x80
#define BPF_MSH        0xa0

#define BPF_OP(code) ((code) & 0xf0)
#define BPF_ADD      0x00
#define BPF_SUB      0x10
#define BPF_MUL      0x20
#define BPF_DIV      0x30
#define BPF_OR       0x40
#define BPF_AND      0x50
#define BPF_LSH      0x60
#define BPF_RSH      0x70
#define BPF_NEG      0x80
#define BPF_MOD      0x90
#define BPF_XOR      0xa0
#define BPF_JA       0x00
#define BPF_JEQ      0x10
#define BPF_JGT      0x20
#define BPF_JGE      0x30
#define BPF_JSET     0x40

#define BPF_SRC(code) ((code) & 0x08)
#define BPF_K         0x00
#define BPF_X         0x08

#define BPF_RVAL(code) ((code) & 0x18)
#define BPF_A          0x10

#define BPF_MISCOP(code) ((code) & 0xf8)
#define BPF_TAX          0x00
#define BPF_TXA          0x80

#define BPF_MEMWORDS 16
#define BPF_MAXINSNS 4096

#define AUDIT_ARCH_X86_64  0xC000003EU
#define AUDIT_ARCH_AARCH64 0xC00000B7U

#endif
