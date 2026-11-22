/* vmlinuz: 4 KiB of headers, then the payload a Linux loader puts at code32_start
   and UEFI loads at 0x1000 past the image base. Everything is linked at 0 and is
   position-independent, so the address in the file is also the PE RVA. */

OUTPUT_FORMAT(elf64-x86-64)
ENTRY(startup_32)

/* stub_kernel_entry, kernel_image_start and kernel_reserve_end, from kernel.elf */
INCLUDE kernel-symbols.ld

SECTIONS
{
    . = 0;
    .header : {
        KEEP(*(.header))
        . = 0x1000;
    }

    _payload_start = .;
    .head : { KEEP(*(.head)) }
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .got : { *(.got .got.*) }

    . = ALIGN(4096);
    .kernel : {
        _kernel_blob = .;
        KEEP(*(.kernel_blob))
        . = ALIGN(512);
    }
    _payload_file_end = .;

    /* The kernel's .bss follows its data where it runs. */
    . = _kernel_blob + (kernel_reserve_end - kernel_image_start);
    . = ALIGN(4096);
    .bss (NOLOAD) : {
        _stub_bss_start = .;
        *(.bss .bss.* COMMON)
        _stub_bss_end = .;
    }
    .stack (NOLOAD) : {
        . = ALIGN(16);
        . += 0x4000;
        stub_stack_top = .;
    }
    . = ALIGN(4096);
    _init_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.rela*) *(.dynamic) *(.interp) }
}

ASSERT(SIZEOF(.got) == 0, "the stub must not need a GOT: it is not relocated")
ASSERT(startup_64 - _payload_start == 0x200, "the 64-bit entry must be 0x200 into the payload")

_payload_file_size = _payload_file_end - _payload_start;
_syssize = (_payload_file_size + 15) / 16;
_init_size = _init_end - _payload_start;
_image_size = _init_end;

_stub_bss_offset = _stub_bss_start - _payload_start;
_stub_bss_size = _stub_bss_end - _stub_bss_start;
_stub_stack_top_offset = stub_stack_top - _payload_start;
_boot_tables_offset = boot_tables - _payload_start;
_stub_gdt_offset = stub_gdt - _payload_start;
_stub_gdt_pointer_offset = stub_gdt_pointer - _payload_start;
_from_32_offset = from_32 - _payload_start;
