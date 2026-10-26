/* d3_differential/ref32.S -- host reference execution of BOUND / LES / LDS.
 *
 * All three are legacy-mode only, so the reference runs at CPL3 inside a
 * static -m32 Linux binary; #BR and #GP surface as SIGSEGV/SIGBUS, which the
 * C driver catches with sigsetjmp so a faulting case does not end the run.
 *
 * The LES/LDS routines put the segment register back the way they found it
 * before returning: a *successful* load of the null selector leaves ES/DS
 * unusable, and the very next libc instruction that uses ES:DI (printf's
 * string moves) would fault for a reason that has nothing to do with the
 * instruction under test.  That mistake cost one debugging round; the comment
 * is cheaper than the round.
 */
    .code32
    .text

/* void ref_bound32(int32_t idx, const int32_t *bounds) */
    .globl ref_bound32
ref_bound32:
    pushl %ebx
    pushl %esi
    movl 12(%esp), %edi          /* bounds */
    movl 16(%esp), %eax          /* index  */
    .byte 0x62, 0x07             /* bound eax, [edi] */
    popl %esi
    popl %ebx
    ret

/* void ref_bound16(int16_t idx, const int16_t *bounds) */
    .globl ref_bound16
ref_bound16:
    pushl %ebx
    pushl %esi
    movl 12(%esp), %edi
    movzwl 16(%esp), %eax
    .byte 0x66, 0x62, 0x07       /* bound ax, [edi] */
    popl %esi
    popl %ebx
    ret

    .data
    .align 4
    .globl g_eax
g_eax:  .long 0
    .align 2
    .globl g_es
g_es:   .word 0
    .globl g_ds2
g_ds2:  .word 0
    .globl g_saved_ds
g_saved_ds: .word 0

    .text
/* void ref_les32(const void *farptr) -- C4 07: les eax, [edi] */
    .globl ref_les32
ref_les32:
    pushl %ebx
    pushl %esi
    movl 12(%esp), %edi
    movw g_saved_ds, %si          /* the data selector to restore */
    movl $0, g_eax                /* "was the offset register written?" */
    .byte 0xC4, 0x07              /* les eax, [edi] */
    movw %es, %cx                 /* read ES ... */
    movw %si, %es                 /* ... and put it back before any store:
                                   * a null/code selector in ES breaks the
                                   * very next libc string move */
    movw %cx, g_es
    movl %eax, g_eax
    popl %esi
    popl %ebx
    ret

/* void ref_lds32(const void *farptr) -- C5 07: lds eax, [edi] */
    .globl ref_lds32
ref_lds32:
    pushl %ebx
    pushl %esi
    movl 12(%esp), %edi
    movw g_saved_ds, %si
    movl $0, g_eax
    .byte 0xC5, 0x07              /* lds eax, [edi] */
    movw %ds, %cx                 /* read DS ... */
    movw %si, %ds                 /* ... then restore it before storing */
    movw %cx, g_ds2
    movl %eax, g_eax
    popl %esi
    popl %ebx
    ret

/* uint16_t read_es(void) / read_ds(void) -- measured, not assumed */
    .globl read_es
read_es:
    movw %es, %ax
    ret
    .globl read_ds
read_ds:
    movw %ds, %ax
    ret
