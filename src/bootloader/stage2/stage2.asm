bits 16
org 0x7E00

stage2_entry:
    cli
    cld
    mov ax, cs
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    call check_a20
    cmp ax, 1
    je a20_ok

    in al, 0x92
    or al, 2
    out 0x92, al

    call check_a20
    cmp ax, 1
    je a20_ok

    jmp halt

a20_ok:
    call do_e820
    call copy_bios_font
    call do_vbe
    jc halt

    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, kernel_dap_first
    int 0x13
    jc halt
    xor ax, ax
    mov ds, ax

    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, kernel_dap_second
    int 0x13
    jc halt
    xor ax, ax
    mov ds, ax

    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, kernel_dap_third
    int 0x13
    jc halt
    xor ax, ax
    mov ds, ax

    cli
    lgdt [gdt32_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:pm32_entry

check_a20:
    pushf
    push ds
    push es
    push di
    push si
    cli
    xor ax, ax
    mov es, ax
    mov di, 0x0500
    mov ax, 0xFFFF
    mov ds, ax
    mov si, 0x0510
    mov al, [es:di]
    push ax
    mov al, [ds:si]
    push ax
    mov byte [es:di], 0x00
    mov byte [ds:si], 0xFF
    cmp byte [es:di], 0xFF
    pop ax
    mov [ds:si], al
    pop ax
    mov [es:di], al
    mov ax, 0
    je check_a20_exit
    mov ax, 1
check_a20_exit:
    pop si
    pop di
    pop es
    pop ds
    popf
    ret

do_e820:
    push es
    mov ax, 0x7000
    mov es, ax
    xor di, di
    xor ebx, ebx
    mov edx, 0x534D4150
    mov eax, 0xE820
    mov [mmap_ent], dword 0
    mov ecx, 24
.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    int 0x15
    jc .e820_fail
    mov edx, 0x534D4150
    cmp eax, edx
    jne .e820_fail
    jcxz .skip_ent
    cmp cl, 20
    jbe .ok_ent
    test byte [es:di + 20], 1
    je .skip_ent
.ok_ent:
    mov ecx, [es:di + 8]
    or ecx, [es:di + 12]
    jz .skip_ent
    inc dword [mmap_ent]
    add di, 24
.skip_ent:
    test ebx, ebx
    jne .e820_loop
    pop es
    ret
.e820_fail:
    pop es
    stc
    ret

copy_bios_font:
    push ds
    push es
    mov ax, 0x1130
    mov bh, 0x06
    int 0x10
    mov ax, es
    mov ds, ax
    mov si, bp
    xor ax, ax
    mov es, ax
    mov di, BIOS_FONT_ADDRESS
    mov cx, 4096
    cld
    rep movsb
    pop es
    pop ds
    ret

do_vbe:
    xor ax, ax
    mov es, ax
    mov di, vbe_info_block
    mov dword [vbe_info_block], 0x32454256
    mov ax, 0x4F00
    int 0x10
    mov dx, ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    cmp dx, 0x004F
    jne .fail

    mov ax, [vbe_info_block + 0x0E]
    mov [mode_list_offset], ax
    mov ax, [vbe_info_block + 0x10]
    mov [mode_list_segment], ax
    mov word [best_mode], 0xFFFF
    mov byte [best_rank], 0
    mov dword [best_area], 0

.mode_loop:
    mov ax, [mode_list_segment]
    mov fs, ax
    mov si, [mode_list_offset]
    mov cx, [fs:si]
    add word [mode_list_offset], 2
    cmp cx, 0xFFFF
    je .modes_done

    push cx
    xor ax, ax
    mov es, ax
    mov di, vbe_mode_info
    mov ax, 0x4F01
    int 0x10
    mov dx, ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    pop cx
    cmp dx, 0x004F
    jne .mode_loop

    mov ax, [vbe_mode_info + 0]
    and ax, 0x0091
    cmp ax, 0x0091
    jne .mode_loop
    cmp byte [vbe_mode_info + 25], 32
    jne .mode_loop
    cmp byte [vbe_mode_info + 27], 6
    jne .mode_loop
    cmp dword [vbe_mode_info + 40], 0
    je .mode_loop

    movzx eax, word [vbe_mode_info + 18]
    movzx edx, word [vbe_mode_info + 20]
    cmp eax, 800
    jb .mode_loop
    cmp eax, 1920
    ja .mode_loop
    cmp edx, 600
    jb .mode_loop
    cmp edx, 1080
    ja .mode_loop
    imul eax, edx
    mov [candidate_area], eax
    mov byte [candidate_rank], 1

    cmp word [vbe_mode_info + 18], 1280
    jne .check_1366
    cmp word [vbe_mode_info + 20], 720
    jne .check_1280_800
    mov byte [candidate_rank], 100
    jmp .consider
.check_1280_800:
    cmp word [vbe_mode_info + 20], 800
    jne .check_1280_1024
    mov byte [candidate_rank], 90
    jmp .consider
.check_1280_1024:
    cmp word [vbe_mode_info + 20], 1024
    jne .consider
    mov byte [candidate_rank], 80
    jmp .consider
.check_1366:
    cmp word [vbe_mode_info + 18], 1366
    jne .check_1024
    cmp word [vbe_mode_info + 20], 768
    jne .consider
    mov byte [candidate_rank], 95
    jmp .consider
.check_1024:
    cmp word [vbe_mode_info + 18], 1024
    jne .check_1920
    cmp word [vbe_mode_info + 20], 768
    jne .consider
    mov byte [candidate_rank], 85
    jmp .consider
.check_1920:
    cmp word [vbe_mode_info + 18], 1920
    jne .consider
    cmp word [vbe_mode_info + 20], 1080
    jne .consider
    mov byte [candidate_rank], 75

.consider:
    mov al, [candidate_rank]
    cmp al, [best_rank]
    ja .select_mode
    jb .mode_loop
    mov eax, [candidate_area]
    cmp eax, [best_area]
    jbe .mode_loop
.select_mode:
    mov [best_mode], cx
    mov al, [candidate_rank]
    mov [best_rank], al
    mov eax, [candidate_area]
    mov [best_area], eax
    jmp .mode_loop

.modes_done:
    cmp word [best_mode], 0xFFFF
    je .fail

    xor ax, ax
    mov es, ax
    mov di, vbe_mode_info
    mov cx, [best_mode]
    mov ax, 0x4F01
    int 0x10
    mov dx, ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    cmp dx, 0x004F
    jne .fail

    mov bx, [best_mode]
    or bx, 0x4000
    mov ax, 0x4F02
    int 0x10
    mov dx, ax
    xor ax, ax
    mov ds, ax
    mov es, ax
    cmp dx, 0x004F
    jne .fail

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov di, BOOT_FB_INFO_ADDRESS
    mov cx, BOOT_FB_INFO_SIZE / 2
    rep stosw

    mov dword [BOOT_FB_INFO_ADDRESS + 0], BOOT_FB_MAGIC
    mov word [BOOT_FB_INFO_ADDRESS + 4], 1
    mov word [BOOT_FB_INFO_ADDRESS + 6], BOOT_FB_INFO_SIZE
    mov eax, [vbe_mode_info + 40]
    mov [BOOT_FB_INFO_ADDRESS + 8], eax
    mov dword [BOOT_FB_INFO_ADDRESS + 12], 0

    movzx eax, word [vbe_mode_info + 50]
    test eax, eax
    jnz .pitch_ready
    movzx eax, word [vbe_mode_info + 16]
.pitch_ready:
    mov [BOOT_FB_INFO_ADDRESS + 16], eax
    mov ax, [vbe_mode_info + 18]
    mov [BOOT_FB_INFO_ADDRESS + 20], ax
    mov ax, [vbe_mode_info + 20]
    mov [BOOT_FB_INFO_ADDRESS + 22], ax
    mov al, [vbe_mode_info + 25]
    mov [BOOT_FB_INFO_ADDRESS + 24], al

    mov al, [vbe_mode_info + 54]
    test al, al
    jz .legacy_masks
    mov [BOOT_FB_INFO_ADDRESS + 25], al
    mov al, [vbe_mode_info + 55]
    mov [BOOT_FB_INFO_ADDRESS + 26], al
    mov al, [vbe_mode_info + 56]
    mov [BOOT_FB_INFO_ADDRESS + 27], al
    mov al, [vbe_mode_info + 57]
    mov [BOOT_FB_INFO_ADDRESS + 28], al
    mov al, [vbe_mode_info + 58]
    mov [BOOT_FB_INFO_ADDRESS + 29], al
    mov al, [vbe_mode_info + 59]
    mov [BOOT_FB_INFO_ADDRESS + 30], al
    mov al, [vbe_mode_info + 60]
    mov [BOOT_FB_INFO_ADDRESS + 31], al
    mov al, [vbe_mode_info + 61]
    mov [BOOT_FB_INFO_ADDRESS + 32], al
    jmp .masks_ready
.legacy_masks:
    mov al, [vbe_mode_info + 31]
    mov [BOOT_FB_INFO_ADDRESS + 25], al
    mov al, [vbe_mode_info + 32]
    mov [BOOT_FB_INFO_ADDRESS + 26], al
    mov al, [vbe_mode_info + 33]
    mov [BOOT_FB_INFO_ADDRESS + 27], al
    mov al, [vbe_mode_info + 34]
    mov [BOOT_FB_INFO_ADDRESS + 28], al
    mov al, [vbe_mode_info + 35]
    mov [BOOT_FB_INFO_ADDRESS + 29], al
    mov al, [vbe_mode_info + 36]
    mov [BOOT_FB_INFO_ADDRESS + 30], al
    mov al, [vbe_mode_info + 37]
    mov [BOOT_FB_INFO_ADDRESS + 31], al
    mov al, [vbe_mode_info + 38]
    mov [BOOT_FB_INFO_ADDRESS + 32], al
.masks_ready:
    mov dword [BOOT_FB_INFO_ADDRESS + 40], BIOS_FONT_ADDRESS
    mov dword [BOOT_FB_INFO_ADDRESS + 44], 0
    mov word [BOOT_FB_INFO_ADDRESS + 48], 8
    mov word [BOOT_FB_INFO_ADDRESS + 50], 16
    clc
    ret
.fail:
    xor ax, ax
    mov es, ax
    mov di, BOOT_FB_INFO_ADDRESS
    mov cx, BOOT_FB_INFO_SIZE / 2
    rep stosw
    stc
    ret

align 8
gdt32:
    dq 0x0000000000000000
    dq 0x00CF9A000000FFFF
    dq 0x00CF92000000FFFF
gdt32_descriptor:
    dw gdt32_descriptor - gdt32 - 1
    dd gdt32

bits 32
pm32_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x7C00

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb halt

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz halt

    mov edi, 0x10000
    mov cr3, edi
    xor eax, eax
    mov ecx, 4096
    rep stosd

    mov dword [0x10000], 0x11003
    mov dword [0x10FF8], 0x11003

    mov dword [0x11000], 0x12003
    mov dword [0x11FF0], 0x12003

    mov edi, 0x12000
    xor ebx, ebx
    mov ecx, 32
.map_low_64m:
    mov eax, ebx
    or eax, 0x83
    mov [edi], eax
    mov dword [edi + 4], 0
    add ebx, 0x200000
    add edi, 8
    loop .map_low_64m

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    lgdt [gdt64_descriptor]
    jmp 0x08:lm64_entry

align 8
gdt64:
    dq 0x0000000000000000
    dq 0x00209A0000000000
    dq 0x0000920000000000
gdt64_descriptor:
    dw gdt64_descriptor - gdt64 - 1
    dd gdt64

bits 64
lm64_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x7C00

    mov rsi, 0x20200
    mov edi, [rsi]
    cmp edi, 0x464C457F
    jne halt_64

    movzx rcx, word [rsi + 56]
    movzx rdx, word [rsi + 54]
    mov r8, [rsi + 32]
    add r8, rsi

.load_loop:
    cmp dword [r8], 1
    jne .next_phdr
    
    mov r9, [r8 + 8]
    add r9, rsi
    mov rdi, [r8 + 16]
    mov r11, [r8 + 32]
    
    push rcx
    mov rcx, r11
    test rcx, rcx
    jz .skip_copy
    push rsi
    mov rsi, r9
    rep movsb
    pop rsi
.skip_copy:
    pop rcx
    
.next_phdr:
    add r8, rdx
    dec rcx
    jnz .load_loop

    mov rbx, [rsi + 24]
    mov edi, [rel mmap_ent]
    mov rsi, 0x70000
    mov rdx, 0x20000
    mov rcx, BOOT_FB_INFO_ADDRESS
    jmp rbx

halt_64:
    cli
    hlt
    jmp halt_64

halt:
    cli
    hlt
    jmp halt

BOOT_FB_INFO_ADDRESS equ 0x5000
BIOS_FONT_ADDRESS equ 0x6000
BOOT_FB_MAGIC equ 0x30424654
BOOT_FB_INFO_SIZE equ 52

boot_drive      db 0
mmap_ent        dd 0
best_mode       dw 0xFFFF
best_rank       db 0
candidate_rank  db 0
best_area       dd 0
candidate_area  dd 0
mode_list_offset  dw 0
mode_list_segment dw 0

align 4
kernel_dap_first:
    db 0x10
    db 0
    dw 127
    dw 0x0000
    dw 0x2000
    dq 64

align 4
kernel_dap_second:
    db 0x10
    db 0
    dw 1
    dw 0xFE00
    dw 0x2000
    dq 191

align 4
kernel_dap_third:
    db 0x10
    db 0
    dw 65
    dw 0x0000
    dw 0x3000
    dq 192

align 4
vbe_info_block:
    times 512 db 0

align 4
vbe_mode_info:
    times 256 db 0
