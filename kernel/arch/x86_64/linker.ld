OUTPUT_FORMAT(elf64-x86-64)
ENTRY(kernel_entry)

/* One loadable segment per permission, which is what Limine maps the image
   from. The request block gets its own so that it is guaranteed to be inside a
   PT_LOAD and reachable by the loader's scan. */
PHDRS
{
    requests PT_LOAD;
    text     PT_LOAD;
    rodata   PT_LOAD;
    data     PT_LOAD;
}

SECTIONS
{
    /* -mcmodel=kernel puts every symbol in the top 2 GiB, which is also where
       Limine requires the image to be linked. */
    . = 0xFFFFFFFF80100000;
    kernel_image_start = .;

    .limine_requests : {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text : {
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data : {
        *(.data .data.*)
    } :data

    .bss : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        kernel_end = .;

        /* pmm_init puts its page bitmap and reference counts at kernel_end, so
           the kernel has to own that space rather than assume somebody mapped
           past its image. Being inside .bss is what makes the loader allocate
           it and report it as taken.
           The bound follows PMM_DIRECT_MAP_LIMIT, 8 GiB: at most 2097152 pages,
           costing one bit each for the bitmap and two bytes each for the
           counts. That is 4.25 MiB; eight megabytes leaves room for the
           alignment and for the limit to grow again. Raising the limit without
           raising this overruns the reserve silently, which is why the two are
           commented against each other. */
        . += 0x800000;
        kernel_reserve_end = .;
    } :data
}
