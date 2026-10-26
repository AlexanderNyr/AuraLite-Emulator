/* d2-differential/host32.c -- measures the legacy BCD/ASCII adjust opcodes
 * and ARPL on the real CPU (i386 compatibility mode) and prints one line per
 * case in a format the emulator harness reproduces byte for byte. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct { uint32_t eax_in, flags_in, eax_out, flags_out;
                 uint16_t dst_in, src_in, dst_out; uint32_t zf_out; } st_t;

extern void ref_daa(st_t *);
extern void ref_das(st_t *);
extern void ref_aaa(st_t *);
extern void ref_aas(st_t *);
extern void ref_aam10(st_t *);
extern void ref_aam16(st_t *);
extern void ref_aad10(st_t *);
extern void ref_aad16(st_t *);
extern void ref_arpl(st_t *);

/* the flags this contract actually defines; anything else is architecturally
 * undefined and must not be compared */
#define MASK_ADJ (0x001u /*CF*/ | 0x004u /*PF*/ | 0x010u /*AF*/ | \
                  0x040u /*ZF*/ | 0x080u /*SF*/)

static void one(const char *nm, void (*fn)(st_t *), uint8_t al, uint8_t ah,
                int cf, int af) {
    st_t s;
    memset(&s, 0, sizeof s);
    s.eax_in = 0x5A5A0000u | ((uint32_t)ah << 8) | al;
    s.flags_in = 0x202u | (cf ? 1u : 0u) | (af ? 0x10u : 0u);
    fn(&s);
    printf("%-8s al=%02x ah=%02x cf=%d af=%d -> al=%02x ah=%02x f=%02x\n",
           nm, al, ah, cf, af,
           (unsigned)(s.eax_out & 0xFF), (unsigned)((s.eax_out >> 8) & 0xFF),
           (unsigned)(s.flags_out & MASK_ADJ));
}

int main(void) {
    void (*fns[8])(st_t *) = { ref_daa, ref_das, ref_aaa, ref_aas,
                               ref_aam10, ref_aam16, ref_aad10, ref_aad16 };
    const char *names[8] = { "daa", "das", "aaa", "aas",
                             "aam10", "aam16", "aad10", "aad16" };
    for (int f = 0; f < 8; f++)
        for (int cf = 0; cf < 2; cf++)
            for (int af = 0; af < 2; af++)
                for (int al = 0; al < 256; al++)
                    one(names[f], fns[f], (uint8_t)al, 0x5A, cf, af);

    /* ARPL: every dst/src RPL pair, with the payload bits kept non-zero so a
     * "wrote the whole word" bug is visible too */
    for (int d = 0; d < 4; d++)
        for (int s = 0; s < 4; s++) {
            st_t st; memset(&st, 0, sizeof st);
            st.dst_in = (uint16_t)(0xFFFCu | (unsigned)d);
            st.src_in = (uint16_t)(0x0000u | (unsigned)s);
            st.flags_in = 0x202;
            ref_arpl(&st);
            printf("%-8s dst=%04x src=%04x -> dst=%04x zf=%u\n",
                   "arpl", st.dst_in, st.src_in, st.dst_out, st.zf_out);
        }
    return 0;
}
