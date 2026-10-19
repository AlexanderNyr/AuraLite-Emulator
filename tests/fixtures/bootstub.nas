; tests/fixtures/bootstub.nas -- 512-byte boot stub for the ISO acceptance lanes.
;
; Assembled with NASM (-f bin).  Used twice inside the test ISO built by
; tools/mkiso.py:
;   * at LBA 0 -- the MBR SeaBIOS reads when the ISO is attached as a disk
;     (the quick GUI route, docs/WINDOWS.md E);
;   * as the El Torito no-emulation boot image -- what SeaBIOS loads when the
;     ISO is attached as an ATAPI CD (--cdrom=, STORE S6).
; Either way the stub lands at 0x7c00 and prints its marker to COM1, whose
; THR is flushed to the machine log by src/serial.c (measured contract).
bits 16
org 0x7c00

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7c00
    sti
    mov dx, 0x3f8               ; COM1 THR (DLAB=0 after reset)
    mov si, msg
.put:
    lodsb
    test al, al
    jz .halt
    out dx, al
    jmp .put
.halt:
    cli
    hlt
    jmp .halt

msg db "AURA_ISO_BOOT_OK", 13, 10, 0

times 510 - ($ - $$) db 0
dw 0xaa55
