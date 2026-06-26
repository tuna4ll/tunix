ENTRY(kernel_entry)
SECTIONS
{
    . = 0xFFFFFFFF80100000;

    .text ALIGN(4K) : AT(ADDR(.text) - 0xFFFFFFFF80000000)
    {
        *(.text*)
    }

    .rodata ALIGN(4K) : AT(ADDR(.rodata) - 0xFFFFFFFF80000000)
    {
        *(.rodata*)
    }

    .data ALIGN(4K) : AT(ADDR(.data) - 0xFFFFFFFF80000000)
    {
        *(.data*)
    }

    .bss ALIGN(4K) : AT(ADDR(.bss) - 0xFFFFFFFF80000000)
    {
        __bss_start = .;
        *(COMMON)
        *(.bss*)
        kernel_end = .;
    }

    ASSERT((kernel_end - 0xFFFFFFFF80000000) < 0x02000000,
           "kernel image overlaps the 32 MiB initramfs staging area")
}
