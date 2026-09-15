    // The built user ELF, carried inside the kernel image until there is a
    // filesystem to read it from.
    .section .rodata.userimage, "a"
    .align 12
    .globl user_elf_start
user_elf_start:
    .incbin "hello.elf"
    .globl user_elf_end
user_elf_end:                       // the real end, before any padding
    .align 12
