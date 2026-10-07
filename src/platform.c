// platform.c -- CPUID signatures for the 5 chipset generations the firmware detects.
// Model IDs (0x2A/0x3A/0x3C/0x3D/0x37) and the CPUID.1.EAX "family/model/
// stepping" signatures below are Intel's own publicly documented values
// (e.g. 0x206A7 Sandy Bridge, 0x306A9 Ivy Bridge, 0x306C3 Haswell,
// 0x306D4 Broadwell, 0x30678 Bay Trail/Silvermont) -- they reproduce exactly
// what the firmware's hand-rolled family/model extraction expects to branch on.
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include "platform.h"

/* K7: tsc_per_instr 100/96/92/88/140 -> 1 on all profiles.  Those values
 * modeled a GHz-class TSC on top of the old ~14-MIPS virtual CPU (PIT pin
 * 12); with the K7 pin (384, ~373 virtual MIPS) they produced a 42.2 GHz
 * TSC/APIC-bus reading and the guest kernel's LAPIC-timer calibration
 * sanity bound ([8 MHz, 4 GHz)) rightly rejected it.  tsc_per_instr=1 is
 * the coherent in-order single-issue model the interpreter actually is:
 * RDTSC and the LAPIC timer bus both advance once per retired instruction,
 * i.e. a ~458 MHz virtual clock under the K7 pin (384*1193182/s). */
static const platform_t PROFILES[PLAT_COUNT] = {
    { PLAT_SANDYBRIDGE, "Sandy Bridge (LGA1155, model 0x2A)", 0x2A, 0x000206A7u,   1 },
    { PLAT_IVYBRIDGE,   "Ivy Bridge (LGA1155, model 0x3A)",   0x3A, 0x000306A9u,   1 },
    { PLAT_HASWELL,     "Haswell (LGA1150, model 0x3C)",      0x3C, 0x000306C3u,   1 },
    { PLAT_BROADWELL,   "Broadwell (model 0x3D)",             0x3D, 0x000306D4u,   1 },
    { PLAT_BAYTRAIL,    "Bay Trail / Silvermont (model 0x37)",0x37, 0x00030678u,   1 },
};

const platform_t *platform_get(platform_id_t id) {
    if (id < 0 || id >= PLAT_COUNT) return &PROFILES[PLAT_HASWELL];
    return &PROFILES[id];
}
const platform_t *platform_by_name(const char *name) {
    if (!name) return &PROFILES[PLAT_HASWELL];
    if (!strcasecmp(name,"sandybridge")||!strcasecmp(name,"sandy")) return &PROFILES[PLAT_SANDYBRIDGE];
    if (!strcasecmp(name,"ivybridge")||!strcasecmp(name,"ivy"))     return &PROFILES[PLAT_IVYBRIDGE];
    if (!strcasecmp(name,"haswell"))                                return &PROFILES[PLAT_HASWELL];
    if (!strcasecmp(name,"broadwell"))                              return &PROFILES[PLAT_BROADWELL];
    if (!strcasecmp(name,"baytrail")||!strcasecmp(name,"bay"))      return &PROFILES[PLAT_BAYTRAIL];
    return &PROFILES[PLAT_HASWELL];
}

void platform_cpuid(const platform_t *p, uint32_t leaf, uint32_t subleaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    (void)subleaf;
    *a=*b=*c=*d=0;
    switch (leaf) {
    case 0x0:
        *a = 0x16; /* highest basic leaf */
        *b = 0x756E6547; /* "Genu" */
        *d = 0x49656E69; /* "ineI" */
        *c = 0x6C65746E; /* "ntel" */
        break;
    case 0x1:
        *a = p->cpuid1_eax;
        *b = 0x00040800; /* brand index / CLFLUSH / APIC ID stub */
        *c = (1u<<0)                 /* SSE3 */
           | (1u<<31);               /* hypervisor present -- we ARE one
                                        * (KERNEL-BOOT K3: the guest's vmdrv
                                        * probe prints the vendor string) */
        *d = (1u<<0)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<9)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
        /* FPU,TSC,MSR,CX8,APIC,CMOV,MMX,FXSR,SSE,SSE2 */
        break;
    /* ---- hypervisor interface leaf (0x40000000) ---- */
    case 0x40000000:
        *a = 0x40000000;             /* highest hypervisor leaf = this one */
        *b = 0x61727541;             /* "Aura" (little-endian) */
        *c = 0x6574694C;             /* "Lite" */
        *d = 0x20564820;             /* " HV " */
        break;
    case 0x80000000:
        *a = 0x80000008; break;
    case 0x80000001:
        *d = (1u<<29); /* long mode supported */
        break;

    /* ---- C9 leaves ---- */
    case 0x2: /* cache/TLB descriptors: repeat once, 0xFF = "use leaf 4" */
        *a = 0x0000FF01u; break;
    case 0x4: { /* deterministic cache parameters, indexed by ECX */
        switch (subleaf) {
        case 0: /* L1 data: 32KB, 8-way, 64 sets, 64B line */
            *a = (1u)|(1u<<5)|(1u<<9); *b = 63u|(0u<<12)|(7u<<22); *c = 63u; break;
        case 1: /* L1 instruction: 32KB, 8-way, 64 sets, 64B line */
            *a = (2u)|(1u<<5)|(1u<<9); *b = 63u|(0u<<12)|(7u<<22); *c = 63u; break;
        case 2: /* L2 unified: 256KB, 8-way, 512 sets, 64B line */
            *a = (3u)|(2u<<5)|(1u<<9); *b = 63u|(0u<<12)|(7u<<22); *c = 511u; break;
        default: break; /* type=0 terminates the enumeration */
        }
        break; }
    case 0x7: /* structured extended features */
        if (subleaf == 0) { *a = 0; *b = (1u<<9); /* ERMS: we do fast REP MOVSB/STOSB */ }
        break;
    case 0x80000002: case 0x80000003: case 0x80000004: { /* brand string, 48B */
        char brand[48];
        memset(brand, ' ', sizeof brand);
        int n = snprintf(brand, sizeof brand, "AuraLite Virtual CPU, model 0x%02X",
                         p->cpuid_family_model);
        if (n < 0) n = 0;
        if (n > (int)sizeof brand) n = sizeof brand;
        else brand[n] = ' '; /* no NUL byte inside the architected 48B field */
        size_t off = (size_t)(leaf - 0x80000002u) * 16;
        uint32_t w[4] = {0,0,0,0};
        memcpy(w, brand + off, 16);
        *a=w[0]; *b=w[1]; *c=w[2]; *d=w[3];
        break; }
    case 0x80000006: /* L2 cache info: 64B line, 8-way (encoding 6), 256KB */
        *c = 64u|(6u<<12)|(256u<<16); break;
    case 0x80000008: /* address sizes: EAX[7:0]=physical 40, [15:8]=virtual 48 */
        *a = 40u|(48u<<8); break;

    default:
        break;
    }
}
