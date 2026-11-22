// The first 4 KiB of vmlinuz: a PE/COFF header, so UEFI firmware and EFI boot
// managers can start the file as an application, and the Linux x86 boot protocol's
// setup header, so GRUB, syslinux, QEMU's -kernel and other Linux loaders can load
// it. The real-mode code after the header is what loaders that start the setup in
// real mode run: it asks the BIOS for the memory map and a linear framebuffer, then
// enters the 32-bit entry the way GRUB would have.

.intel_syntax noprefix
.section .header, "ax"

// boot_params, the BIOS's answers, and the stack, all in the setup's own 64 KiB.
.set BOOT_PARAMS, 0x1000
.set VBE_INFO,    0x2000
.set MODE_INFO,   0x2200

.global _image_start
_image_start:
    .ascii "MZ"
    .org 0x3c
    .long pe_header

    .org 0x40
pe_header:
    .ascii "PE\0\0"
    .word 0x8664                        // Machine: x86-64
    .word 1                             // NumberOfSections
    .long 0                             // TimeDateStamp
    .long 0                             // PointerToSymbolTable
    .long 0                             // NumberOfSymbols
    .word optional_header_end - optional_header
    .word 0x0206                        // executable, line numbers and debug data stripped
optional_header:
    .word 0x020b                        // PE32+
    .byte 0, 0                          // linker version
    .long _payload_file_size            // SizeOfCode
    .long 0                             // SizeOfInitializedData
    .long 0                             // SizeOfUninitializedData
    .long efi_main                      // AddressOfEntryPoint: the image is linked at 0
    .long 0x1000                        // BaseOfCode
    .quad 0                             // ImageBase
    .long 0x1000                        // SectionAlignment
    .long 0x200                         // FileAlignment
    .word 0, 0, 0, 0, 0, 0              // OS, image and subsystem versions
    .long 0                             // Win32VersionValue
    .long _image_size                   // SizeOfImage
    .long 0x1000                        // SizeOfHeaders
    .long 0                             // CheckSum
    .word 10                            // Subsystem: EFI application
    .word 0                             // DllCharacteristics
    .quad 0x100000, 0x1000              // stack reserve, commit
    .quad 0x100000, 0x1000              // heap reserve, commit
    .long 0                             // LoaderFlags
    .long 6                             // NumberOfRvaAndSizes
    .quad 0, 0, 0, 0, 0                 // export, import, resource, exception, certificates
    .quad 0                             // base relocations: none, the code is position-independent
optional_header_end:
    .ascii ".text\0\0\0"
    .long _image_size - 0x1000          // VirtualSize: the kernel's .bss and the stub's tables too
    .long 0x1000                        // VirtualAddress
    .long _payload_file_size            // SizeOfRawData
    .long 0x1000                        // PointerToRawData
    .long 0, 0                          // relocations, line numbers
    .word 0, 0
    .long 0xe00000e0                    // code, data, bss; read, write, execute

// The Linux boot protocol's setup header, version 2.15.
    .org 0x1f1
    .byte 7                             // setup_sects: the setup is 4 KiB with this sector
    .word 0                             // root_flags
    .long _syssize                      // syssize, in 16-byte units
    .word 0                             // ram_size
    .word 0xffff                        // vid_mode: normal
    .word 0                             // root_dev
    .word 0xaa55                        // boot_flag
    .org 0x200
    .byte 0xeb, real_mode_start - jump_end
jump_end:
    .ascii "HdrS"
    .word 0x020f                        // protocol version
    .long 0                             // realmode_swtch
    .word 0x1000                        // start_sys_seg
    .word version_string - _image_start - 0x200
    .byte 0                             // type_of_loader
    .byte 0x01                          // loadflags: LOADED_HIGH
    .word 0x8000                        // setup_move_size
    .long 0x100000                      // code32_start
    .long 0                             // ramdisk_image
    .long 0                             // ramdisk_size
    .long 0                             // bootsect_kludge
    .word 0                             // heap_end_ptr
    .byte 0                             // ext_loader_ver
    .byte 0                             // ext_loader_type
    .long 0                             // cmd_line_ptr
    .long 0x7fffffff                    // initrd_addr_max
    .long 0x200000                      // kernel_alignment
    .byte 1                             // relocatable_kernel
    .byte 12                            // min_alignment: 4 KiB is enough
    .word 0x0003                        // xloadflags: 64-bit entry, may be loaded above 4 GiB
    .long 4095                          // cmdline_size
    .long 0                             // hardware_subarch: PC
    .quad 0                             // hardware_subarch_data
    .long 0                             // payload_offset: not compressed
    .long 0                             // payload_length
    .quad 0                             // setup_data
    .quad 0x1000000                     // pref_address
    .long _init_size                    // init_size: the payload with the kernel's .bss and the stub's tables
    .long 0                             // handover_offset: no EFI handover, use the PE entry
    .long 0                             // kernel_info_offset

version_string:
    .asciz "Tunix 0.1.0"

// ---- Real mode: the loader jumps to setup segment + 0x20, offset 0 ----
.code16
real_mode_start:
    cli
    mov ax, cs
    sub ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xfff0
    sti
    cld

    // boot_params is built at BOOT_PARAMS from the header the loader filled in.
    mov di, BOOT_PARAMS
    mov cx, 2048
    xor ax, ax
    rep stosw
    mov si, 0x1f1
    mov di, BOOT_PARAMS + 0x1f1
    mov cx, 0x290 - 0x1f1
    rep movsb

    call memory_map
    call framebuffer

    // Fast A20.
    in al, 0x92
    or al, 2
    and al, 0xfe
    out 0x92, al

    cli
    xor ebx, ebx
    mov bx, ds
    shl ebx, 4
    lea edx, [ebx + real_mode_gdt]
    mov dword ptr [real_mode_gdt_pointer + 2], edx
    lea edx, [ebx + protected_entry]
    mov dword ptr [protected_far], edx
    lgdt [real_mode_gdt_pointer]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp fword ptr [protected_far]

// int 15h, e820, into boot_params.e820_table.
memory_map:
    xor ebx, ebx
    mov di, BOOT_PARAMS + 0x2d0
    xor bp, bp
1:
    mov eax, 0xe820
    mov edx, 0x534d4150
    mov ecx, 20
    int 0x15
    jc 2f
    cmp eax, 0x534d4150
    jne 2f
    inc bp
    add di, 20
    cmp bp, 128
    jae 2f
    test ebx, ebx
    jnz 1b
2:
    mov ax, bp
    mov byte ptr [BOOT_PARAMS + 0x1e8], al
    ret

// The largest 32-bit linear-framebuffer VBE mode up to 1920x1200, set and described
// in boot_params.screen_info. Tunix draws its console on it.
framebuffer:
    mov di, VBE_INFO
    mov dword ptr [di], 0x32454256      // "VBE2"
    mov ax, 0x4f00
    int 0x10
    cmp ax, 0x004f
    jne 9f
    mov si, word ptr [VBE_INFO + 0x0e]
    mov ax, word ptr [VBE_INFO + 0x10]
    mov fs, ax
    mov word ptr [best_mode], 0xffff
    mov dword ptr [best_area], 0
1:
    mov cx, word ptr fs:[si]
    cmp cx, 0xffff
    je 3f
    add si, 2
    push si
    push fs
    mov di, MODE_INFO
    mov ax, 0x4f01
    int 0x10
    pop fs
    pop si
    cmp ax, 0x004f
    jne 1b
    mov ax, word ptr [MODE_INFO]
    and ax, 0x0099                      // supported, colour, graphics, linear
    cmp ax, 0x0099
    jne 1b
    cmp byte ptr [MODE_INFO + 0x19], 32
    jne 1b
    cmp byte ptr [MODE_INFO + 0x1b], 6  // direct colour
    jne 1b
    cmp word ptr [MODE_INFO + 0x12], 1920
    ja 1b
    cmp word ptr [MODE_INFO + 0x14], 1200
    ja 1b
    movzx eax, word ptr [MODE_INFO + 0x12]
    movzx edx, word ptr [MODE_INFO + 0x14]
    imul eax, edx
    cmp eax, dword ptr [best_area]
    jbe 1b
    mov dword ptr [best_area], eax
    mov word ptr [best_mode], cx
    jmp 1b
3:
    mov cx, word ptr [best_mode]
    cmp cx, 0xffff
    je 9f
    mov di, MODE_INFO
    mov ax, 0x4f01
    int 0x10
    cmp ax, 0x004f
    jne 9f
    mov bx, cx
    or bx, 0x4000                       // linear framebuffer
    mov ax, 0x4f02
    int 0x10
    cmp ax, 0x004f
    jne 9f

    mov byte ptr [BOOT_PARAMS + 0x0f], 0x23     // VIDEO_TYPE_VLFB
    mov ax, word ptr [MODE_INFO + 0x12]
    mov word ptr [BOOT_PARAMS + 0x12], ax       // lfb_width
    mov ax, word ptr [MODE_INFO + 0x14]
    mov word ptr [BOOT_PARAMS + 0x14], ax       // lfb_height
    mov word ptr [BOOT_PARAMS + 0x16], 32       // lfb_depth
    mov eax, dword ptr [MODE_INFO + 0x28]
    mov dword ptr [BOOT_PARAMS + 0x18], eax     // lfb_base
    movzx eax, word ptr [VBE_INFO + 0x12]
    mov dword ptr [BOOT_PARAMS + 0x1c], eax     // lfb_size, in 64 KiB units
    mov ax, word ptr [MODE_INFO + 0x10]         // BytesPerScanLine
    cmp word ptr [VBE_INFO + 0x04], 0x0300
    jb 4f
    mov ax, word ptr [MODE_INFO + 0x32]         // LinBytesPerScanLine, VBE 3
4:
    mov word ptr [BOOT_PARAMS + 0x24], ax       // lfb_linelength
    mov si, MODE_INFO + 0x1f                    // red, green, blue, reserved: size, position
    mov di, BOOT_PARAMS + 0x26
    mov cx, 8
    rep movsb
9:
    ret

best_mode:
    .word 0
best_area:
    .long 0

.balign 8
real_mode_gdt:
    .quad 0
    .quad 0
    .quad 0x00cf9a000000ffff            // 0x10: 32-bit code, as the protocol's __BOOT_CS
    .quad 0x00cf92000000ffff            // 0x18: data, as __BOOT_DS
real_mode_gdt_pointer:
    .word 31
    .long 0
protected_far:
    .long 0
    .word 0x10

.code32
protected_entry:
    mov ax, 0x18
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    lea esi, [ebx + BOOT_PARAMS]
    jmp dword ptr [esi + 0x214]         // code32_start
