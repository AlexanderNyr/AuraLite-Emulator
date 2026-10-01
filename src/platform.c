// platform.c -- CPUID signatures for the 5 chipset generations the firmware detects.
// Model IDs (0x2A/0x3A/0x3C/0x3D/0x37) and the CPUID.1.EAX "family/model/
// stepping" signatures below are Intel's own publicly documented values
// (e.g. 0x206A7 Sandy Bridge, 0x306A9 Ivy Bridge, 0x306C3 Haswell,
// 0x306D4 Broadwell, 0x30678 Bay Trail/Silvermont) -- they reproduce exactly
// what the firmware's hand-rolled family/model extraction expects to branch on.
#define _POSIX_C_SOURCE 200809L
#include <string.h>
#include <strings.h>
#include "platform.h"

static const platform_t PROFILES[PLAT_COUNT] = {
    { PLAT_SANDYBRIDGE, "Sandy Bridge (LGA1155, model 0x2A)", 0x2A, 0x000206A7u },
    { PLAT_IVYBRIDGE,   "Ivy Bridge (LGA1155, model 0x3A)",   0x3A, 0x000306A9u },
    { PLAT_HASWELL,     "Haswell (LGA1150, model 0x3C)",      0x3C, 0x000306C3u },
    { PLAT_BROADWELL,   "Broadwell (model 0x3D)",             0x3D, 0x000306D4u },
    { PLAT_BAYTRAIL,    "Bay Trail / Silvermont (model 0x37)",0x37, 0x00030678u },
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
        *c = (1u<<0);                 /* SSE3 */
        *d = (1u<<0)|(1u<<4)|(1u<<5)|(1u<<8)|(1u<<9)|(1u<<15)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26);
        /* FPU,TSC,MSR,CX8,APIC,CMOV,MMX,FXSR,SSE,SSE2 */
        break;
    case 0x80000000:
        *a = 0x80000008; break;
    case 0x80000001:
        *d = (1u<<29); /* long mode supported */
        break;
    default:
        break;
    }
}
