OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

KERNEL_VA_OFFSET = 0xFFFF000000000000;
KERNEL_PHYS_BASE = 0x40000000;

SECTIONS
{
    . = KERNEL_VA_OFFSET + KERNEL_PHYS_BASE;
    kernel_start = .;

    .text : {
        KEEP(*(.text.boot))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }

    . = ALIGN(4096);
    .data : { *(.data .data.*) }

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
        __bss_end = .;

        . = ALIGN(4096);
        __stack_bottom = .;
        . += 0x10000;
        __stack_top = .;
    }

    kernel_end = .;
    __image_end = .;

    /DISCARD/ : { *(.comment) *(.note.*) }
}
