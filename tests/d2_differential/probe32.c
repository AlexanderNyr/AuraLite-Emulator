/* probe32.c -- derives the flag rules for AAD/AAM empirically: sweeps AH and
 * AL and prints the boundary where CF/AF turn on. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
typedef struct { uint32_t eax_in, flags_in, eax_out, flags_out;
                 uint16_t dst_in, src_in, dst_out; uint32_t zf_out; } st_t;
extern void ref_aad10(st_t *);
extern void ref_aad16(st_t *);
extern void ref_aam10(st_t *);
int main(void) {
    for (int k = 0; k < 2; k++) {
        void (*fn)(st_t *) = k ? ref_aad16 : ref_aad10;
        int base = k ? 16 : 10;
        printf("== AAD base=%d: CF/AF boundary per AH\n", base);
        for (int ah = 0; ah < 256; ah += 1) {
            int cf0 = -1, cf1 = -1, af0 = -1, af1 = -1;
            for (int al = 0; al < 256; al++) {
                st_t s; memset(&s, 0, sizeof s);
                s.eax_in = ((uint32_t)ah << 8) | (uint32_t)al;
                s.flags_in = 0x202;
                fn(&s);
                int cf = s.flags_out & 1, af = (s.flags_out >> 4) & 1;
                if (cf) { if (cf0 < 0) cf0 = al; cf1 = al; }
                if (af) { if (af0 < 0) af0 = al; af1 = al; }
            }
            if (ah % 16 == 0 || cf0 >= 0 || af0 >= 0)
                printf("ah=%02x sum0=%d cf[%d..%d] af[%d..%d]\n",
                       ah, ah * base, cf0, cf1, af0, af1);
            if (ah > 40) break;
        }
    }
    return 0;
}
