; Minimal 64-bit test payload. the firmware's USB mass-storage loader reads this
; straight off (virtual) LBA 0 of the USB stick into physical memory at
; 0x00100000 and -- once control actually reaches launch64bit -- jumps to it
; with CS already a flat 64-bit long-mode code segment, paging identity
; mapped, interrupts off. We paint the GPU framebuffer that the firmware's own
; "enable:" code maps at 0xD0000000 (800x600x32bpp) to prove a real guest
; payload is actually running, then halt.
[bits 64]
[org 0x100000]

WIDTH  equ 800
HEIGHT equ 600

start:
    cli
    mov rdi, 0xD0000000      ; framebuffer pixel pointer (matches devices.c GPU VRAM)
    xor r12, r12              ; row = 0

.row_loop:
    xor r13, r13              ; col = 0
.col_loop:
    mov eax, r13d              ; col -> blue channel
    mov ebx, r12d
    shl ebx, 8                 ; row -> green channel
    or eax, ebx
    or eax, 0x00200000         ; constant red tint so it's clearly "painted"
    mov [rdi], eax
    add rdi, 4
    inc r13
    cmp r13, WIDTH
    jb .col_loop
    inc r12
    cmp r12, HEIGHT
    jb .row_loop

halt_loop:
    hlt
    jmp halt_loop

times 4096-($-$$) db 0
