/* d2-differential/ref32.S -- host reference for the legacy BCD/ASCII adjust
 * opcodes and ARPL.
 *
 * These instructions are #UD in 64-bit mode, so the host measures them in
 * i386 compatibility mode (a static -m32 Linux binary executes them
 * natively).  Every routine is handed a state block:
 *
 *   0  eax_in      4  flags_in     8  eax_out     12 flags_out
 *  16  dst_in(w)  18  src_in(w)   20  dst_out(w)  24 zf_out
 *
 * and executes exactly ONE instruction.
 */
    .code32
    .text

#define ADJ_PROLOG                 \
    movl 4(%esp), %esi;            \
    movl 0(%esi), %eax;            \
    pushl 4(%esi);                 \
    popfl

#define ADJ_EPILOG                 \
    pushfl;                        \
    popl %ecx;                     \
    movl %eax, 8(%esi);            \
    movl %ecx, 12(%esi);           \
    ret

    .globl ref_daa
ref_daa:    ADJ_PROLOG; .byte 0x27; ADJ_EPILOG
    .globl ref_das
ref_das:    ADJ_PROLOG; .byte 0x2F; ADJ_EPILOG
    .globl ref_aaa
ref_aaa:    ADJ_PROLOG; .byte 0x37; ADJ_EPILOG
    .globl ref_aas
ref_aas:    ADJ_PROLOG; .byte 0x3F; ADJ_EPILOG
    .globl ref_aam10
ref_aam10:  ADJ_PROLOG; .byte 0xD4, 0x0A; ADJ_EPILOG
    .globl ref_aam16
ref_aam16:  ADJ_PROLOG; .byte 0xD4, 0x10; ADJ_EPILOG
    .globl ref_aad10
ref_aad10:  ADJ_PROLOG; .byte 0xD5, 0x0A; ADJ_EPILOG
    .globl ref_aad16
ref_aad16:  ADJ_PROLOG; .byte 0xD5, 0x10; ADJ_EPILOG

/* ARPL %src, mem16 -- loads dst into a private buffer, adjusts, reads back */
    .globl ref_arpl
ref_arpl:
    movl 4(%esp), %esi
    movzwl 16(%esi), %ecx            /* dst */
    movl %ecx, arpl_buf
    pushl 4(%esi)
    popfl
    movzwl 18(%esi), %edx            /* src */
    arpl %dx, arpl_buf               /* AT&T: arpl src, dst */
    pushfl
    popl %ecx
    movl arpl_buf, %edx
    movw %dx, 20(%esi)
    shrl $6, %ecx
    andl $1, %ecx
    movl %ecx, 24(%esi)              /* ZF */
    ret

    .data
    .align 2
arpl_buf:
    .word 0
