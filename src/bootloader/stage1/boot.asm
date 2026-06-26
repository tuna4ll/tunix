bits 16
org 0x7C00

start:
    jmp short main
    nop
    times 8 db 0

main:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7C00
    sti

    mov [boot_drive], dl

    mov si, msg_loading
    call print_string

    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc error_no_ext
    cmp bx, 0xAA55
    jne error_no_ext

    mov cx, 3
.retry_read:
    push cx
    mov ah, 0x42
    mov dl, [boot_drive]
    mov si, disk_address_packet
    int 0x13
    jnc .read_ok
    
    mov ah, 0x00
    int 0x13
    pop cx
    loop .retry_read
    
    jmp disk_error

.read_ok:
    pop cx

    mov dl, [boot_drive]
    jmp 0x0000:0x7E00

print_string:
    pusha
    mov ah, 0x0E
.loop:
    lodsb
    test al, al
    jz .done
    int 0x10
    jmp .loop
.done:
    popa
    ret

error_no_ext:
    mov si, msg_no_ext
    call print_string
    jmp halt

disk_error:
    mov si, msg_disk_err
    call print_string
    jmp halt

halt:
    cli
    hlt
    jmp halt

boot_drive      db 0
msg_loading     db "TUNIX: Loading Stage 2...", 13, 10, 0
msg_no_ext      db "ERR: No INT13h Ext!", 13, 10, 0
msg_disk_err    db "ERR: Disk read failed!", 13, 10, 0

align 4
disk_address_packet:
    db 0x10
    db 0
    dw 63
    dw 0x7E00
    dw 0x0000
    dq 1

times 510-($-$$) db 0
dw 0xAA55
