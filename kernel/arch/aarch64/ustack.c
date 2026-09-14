#include <stddef.h>
#include <stdint.h>

#include "arch.h"

// Mirrors the layout kernel/elf.c builds for x86-64, so both architectures
// hand a program the same shape of stack. It writes through user addresses,
// so the target address space must already be installed in TTBR0.

#define AT_NULL     0
#define AT_PHDR     3
#define AT_PHENT    4
#define AT_PHNUM    5
#define AT_PAGESZ   6
#define AT_BASE     7
#define AT_FLAGS    8
#define AT_ENTRY    9
#define AT_UID      11
#define AT_EUID     12
#define AT_GID      13
#define AT_EGID     14
#define AT_PLATFORM 15
#define AT_HWCAP    16
#define AT_CLKTCK   17
#define AT_SECURE   23
#define AT_RANDOM   25
#define AT_HWCAP2   26
#define AT_EXECFN   31

#define MAX_ARGC 64
#define MAX_ENVC 64

static uint64_t text_length(const char *text) {
    uint64_t length = 0;
    while (text[length]) length++;
    return length;
}

static void push_bytes(uint64_t *sp, const void *data, uint64_t size) {
    *sp -= size;
    const uint8_t *source = data;
    uint8_t *target = (uint8_t *)*sp;
    for (uint64_t i = 0; i < size; i++) target[i] = source[i];
}

static void push_word(uint64_t *sp, uint64_t value) {
    *sp -= 8;
    *(uint64_t *)*sp = value;
}

int user_stack_build(uint64_t stack_top, const char *const argv[],
                     const char *const envp[], const struct elf_image *image,
                     uint64_t *sp_out) {
    uint64_t argc = 0, envc = 0;
    while (argv && argv[argc]) {
        if (argc >= MAX_ARGC) return -1;
        argc++;
    }
    while (envp && envp[envc]) {
        if (envc >= MAX_ENVC) return -1;
        envc++;
    }

    uint64_t argv_address[MAX_ARGC];
    uint64_t env_address[MAX_ENVC];
    uint64_t sp = stack_top;

    // Not a cryptographic source yet; the counter is what this port has.
    uint8_t random_bytes[16];
    uint64_t seed = sysreg_read("cntpct_el0") | 1UL;
    for (int i = 0; i < 16; i++) {
        seed = seed * 6364136223846793005UL + 1442695040888963407UL;
        random_bytes[i] = (uint8_t)(seed >> 33);
    }
    push_bytes(&sp, random_bytes, sizeof(random_bytes));
    uint64_t random_address = sp;

    static const char platform[] = "aarch64";
    push_bytes(&sp, platform, sizeof(platform));
    uint64_t platform_address = sp;

    for (uint64_t i = envc; i > 0; i--) {
        push_bytes(&sp, envp[i - 1], text_length(envp[i - 1]) + 1);
        env_address[i - 1] = sp;
    }
    for (uint64_t i = argc; i > 0; i--) {
        push_bytes(&sp, argv[i - 1], text_length(argv[i - 1]) + 1);
        argv_address[i - 1] = sp;
    }

    sp &= ~15UL;

    uint64_t execfn = argc ? argv_address[0] : 0;
    const uint64_t auxv[][2] = {
        {AT_NULL, 0},
        {AT_EXECFN, execfn},
        {AT_HWCAP2, 0},
        {AT_RANDOM, random_address},
        {AT_SECURE, 0},
        {AT_CLKTCK, 100},
        {AT_HWCAP, 0},
        {AT_PLATFORM, platform_address},
        {AT_EGID, 0}, {AT_GID, 0}, {AT_EUID, 0}, {AT_UID, 0},
        {AT_ENTRY, image->entry},
        {AT_FLAGS, 0},
        {AT_BASE, 0},
        {AT_PHNUM, image->phnum},
        {AT_PHENT, image->phentsize},
        {AT_PHDR, image->phdr},
        {AT_PAGESZ, 4096}
    };
    const uint64_t aux_count = sizeof(auxv) / sizeof(auxv[0]);

    // The words below must land so that the final sp is 16-byte aligned.
    uint64_t words = aux_count * 2 + 1 + envc + 1 + argc + 1;
    if ((sp - words * 8) & 15UL) push_word(&sp, 0);

    for (uint64_t i = 0; i < aux_count; i++) {
        push_word(&sp, auxv[i][1]);
        push_word(&sp, auxv[i][0]);
    }

    push_word(&sp, 0);
    for (uint64_t i = envc; i > 0; i--) push_word(&sp, env_address[i - 1]);
    push_word(&sp, 0);
    for (uint64_t i = argc; i > 0; i--) push_word(&sp, argv_address[i - 1]);
    push_word(&sp, argc);

    if (sp & 15UL) return -1;
    *sp_out = sp;
    return 0;
}
