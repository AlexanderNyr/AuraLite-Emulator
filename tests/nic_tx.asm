; tests/nic_tx.asm -- NET N1 wire payload: the TX half of the e1000, measured
; through the real firmware-boot transport (not a unit fixture).
;
; The firmware's USB mass-storage loader reads exactly ONE 512-byte block off
; LBA 0 of --disk= into physical 0x00100000 and jumps there in 64-bit long
; mode, identity paging, interrupts off (firmware/sample_firmware.nas:689) --
; the contract disk/test_kernel.asm runs under.  So this payload must stay
; <= 512 bytes; it is that contract's NIC twin.  It programs the TX ring the
; way the frozen guest driver does (AuraLite-OS@0ed0d29 drivers/e1000/e1000.c:
; e1000_init + e1000_send), rings the TDT doorbell and reports on COM1:
;
;   N1TXOK status=01 tdh=01 icr=81   <- DD written back inside the doorbell
;                                        store, TDH advanced, TXDW|TXQE up
;   N1TXFAIL status=.. tdh=.. icr=.. <- any of those missing; defect D2's
;                                        receipt was status=00 tdh=00
;
; The frame itself leaves through the TAP socket (ground rule: UDP datagrams =
; Ethernet frames, 127.0.0.1:8080), so "the bytes reached the wire" is proven
; host-side by tests/integration/n1_tx_tap.sh, not here.  Keeping this output
; independent of the socket is deliberate: the same binary must print the same
; thing whether or not a responder is bound, or the determinism pair would
; depend on host scheduling.
;
; Build: nasm -f bin tests/nic_tx.asm -o <out>.bin   (Makefile: test-net-tx)
[bits 64]
[org 0x100000]

BAR    equ 0xFEBC0000       ; e1000 82540EM BAR0 (0:4.0, opt-in via --tap=)
CTRL   equ 0x0000
ICR    equ 0x00C0
IMS    equ 0x00D0
TCTL   equ 0x0400
TDBAL  equ 0x3800
TDLEN  equ 0x3808
TDH    equ 0x3810
TDT    equ 0x3818
COM1   equ 0x03F8
; The firmware's long-mode map is NOT all of RAM: PML4[0] -> PDPT[0] -> PD,
; and PD[0] = 0x19B is a 2 MiB page (PS in a PDE), so the only conventional
; RAM a payload may touch is 0x100000..0x1FFFFF -- measured: a write to
; 0x200000 raises #PF err=0x2 (PD[1] = 0).  MMIO reaches the payload through
; PDPT[3] (0xC0000000..0xFFFFFFFF, which is how the framebuffer at
; 0xD0000000 and BAR0 at 0xFEBC0000 work).  So the ring and the DMA buffer
; live just above this payload's own 512 bytes.
RING   equ 0x100800         ; 32 legacy TX descriptors (guest NUM_TX_DESC)
FRAME  equ 0x100C00         ; the frame's DMA buffer (guest tx_buffers[0])
FLEN   equ 20

%macro MW 2                 ; MMIO write
    mov r11d, BAR + %1
    mov eax, %2
    mov [r11], eax
%endmacro
%macro MR 1                 ; MMIO read -> eax
    mov r11d, BAR + %1
    mov eax, [r11]
%endmacro

start:
    cli
    ; ---- the frame: broadcast dst, the QEMU-default src 52:54:00:12:34:56,
    ; ---- ethertype 0x0800, payload "N1TXOK" -- three stores, not twenty
    ; ---- byte-writes, because the loader only gives us one block.
    mov rax, 0x5452FFFFFFFFFFFF       ; ff*6 | 52 54
    mov [FRAME], rax
    mov rax, 0x314E000856341200       ; 00 12 34 56 | 08 00 | 'N' '1'
    mov [FRAME+8], rax
    mov eax, 0x4B4F5854               ; 'T' 'X' 'O' 'K'
    mov [FRAME+16], eax

    ; ---- descriptor 0, exactly as e1000_send() fills it: addr=FRAME,
    ; ---- length=20, cso=0, cmd=0x0B (EOP|IFCS|RS), status=0.
    mov dword [RING+0], FRAME
    mov dword [RING+4], 0
    mov dword [RING+8], FLEN | (0x0B << 24)
    mov dword [RING+12], 0

    ; ---- TX init as e1000_init() does it.  The TCTL write is itself a
    ; ---- doorbell (QEMU maps [TCTL] and [TDT] to the same op), so the cause
    ; ---- register is drained afterwards to keep the receipt honest.
    MW CTRL,  (1 << 0) | (1 << 6)     ; FD | SLU
    MW IMS,   0xD5                    ; the guest's mask
    MW TDBAL, RING
    MW TDLEN, 32 * 16
    MW TDH,   0
    MW TDT,   0
    MW TCTL,  (1 << 1) | (0x10 << 4) | (0x40 << 12)   ; EN | CT | COLD
    MR ICR                            ; drain the empty init walk's TXQE

    ; ---- the doorbell.  e1000_send() then spins on status&0x01 with a
    ; ---- bounded PAUSE budget; here the very next read is the check, so a
    ; ---- DD that lands later than the TDT store is a failure.
    MW TDT, 1
    mov r12d, [RING+12]               ; status byte = bits 0..7 of the dword
    and r12d, 0xFF
    MR TDH
    mov r13d, eax
    MR ICR
    mov r14d, eax

    ; ---- verdict, then one receipt line
    mov rsi, .sok
    cmp r12d, 0x01                    ; DD?
    jne .fail
    cmp r13d, 1                       ; TDH advanced?
    jne .fail
    test r14d, (1 << 0) | (1 << 7)    ; TXDW and TXQE?
    jz .fail
    jmp .say
.fail:
    mov rsi, .sbad
.say:
    call .str
    mov rsi, .sst
    call .str
    mov r9d, r12d
    call .hex2
    mov rsi, .stdh
    call .str
    mov r9d, r13d
    call .hex2
    mov rsi, .sicr
    call .str
    mov r9d, r14d
    call .hex2
    mov ecx, 10
    call .put
.halt:
    hlt
    jmp .halt

.str:                                 ; NUL-terminated string at rsi -> COM1
    mov al, [rsi]
    test al, al
    jz .ret
    inc rsi
    mov cl, al                        ; .put takes the character in cl
    call .put
    jmp .str
.hex2:                                ; r9d low byte -> two hex digits
    mov rcx, r9
    shr rcx, 4
    call .nib
    mov rcx, r9
.nib:
    and ecx, 0xF
    cmp ecx, 10
    jb .dig
    add ecx, 39                       ; 'a'-10-'0'
.dig:
    add ecx, '0'
.put:                                 ; cl = character -> COM1, polled THR-empty
    push rax
    push rcx
    mov dx, COM1 + 5
.spin:
    in al, dx
    test al, 0x20
    jz .spin
    mov dx, COM1
    mov al, cl
    out dx, al
    pop rcx
    pop rax
.ret:
    ret

.sok:    db 'N1TXOK ', 0
.sbad:   db 'N1TXFAIL ', 0
.sst:    db ' status=', 0
.stdh:   db ' tdh=', 0
.sicr:   db ' icr=', 0

times 512-($-$$) db 0
