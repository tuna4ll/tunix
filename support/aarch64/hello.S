// A freestanding AArch64 user program, linked as a normal static ELF and
// loaded by the kernel's ELF loader rather than embedded as a raw blob.

    .global _start
    .text
_start:
    mov     x8, #64                 // __NR_write
    mov     x0, #1                  // stdout
    adr     x1, message
    mov     x2, #(message_end - message)
    svc     #0

    mov     x8, #93                 // __NR_exit
    mov     x0, #7
    svc     #0
1:  b       1b

message:
    .ascii  "hello from a real ELF binary at EL0\n"
message_end:
