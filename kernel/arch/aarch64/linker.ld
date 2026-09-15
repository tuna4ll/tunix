OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

KERNEL_VIRTUAL_BASE = 0xFFFFFFFF80000000;

SECTIONS
{
    . = KERNEL_VIRTUAL_BASE;
    kernel_image_start = .;

    .text : {
        KEEP(*(.text.head))
        *(.text .text.*)
    }

    . = ALIGN(4096);
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }

    . = ALIGN(4096);
    .bss (NOLOAD) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        . = ALIGN(16);
        __bss_end = .;
        kernel_end = .;
        . += 0x800000;
        kernel_reserve_end = .;
        . = ALIGN(4096);
        boot_stack_bottom = .;
        . += 0x10000;
        boot_stack_top = .;
    }

    . = ALIGN(4096);
    __image_end = .;
    __image_size = __image_end - KERNEL_VIRTUAL_BASE;

    /DISCARD/ : { *(.comment) *(.note .note.*) *(.eh_frame .eh_frame_hdr) }
}
