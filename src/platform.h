// platform.h -- CPU-generation ("chipset") profiles. the firmware's firmware uses
// CPUID family/model/stepping to pick one of five hand-written init paths
// (Sandy Bridge, Ivy Bridge, Haswell, Broadwell, Bay Trail). We expose a
// switchable profile so the same emulator core can drive any of them.
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>

typedef enum {
    PLAT_SANDYBRIDGE = 0,
    PLAT_IVYBRIDGE,
    PLAT_HASWELL,
    PLAT_BROADWELL,
    PLAT_BAYTRAIL,
    PLAT_COUNT
} platform_id_t;

typedef struct platform {
    platform_id_t id;
    const char *name;
    uint32_t cpuid_family_model; /* value the firmware expects to see after its
                                    (((eax>>4)&0xF) ... ) decode producing
                                    the "ebx" comparison constant (0x2A,0x3A,
                                    0x3C,0x3D,0x37) */
    uint32_t cpuid1_eax;         /* raw value returned for CPUID.1.EAX so the
                                    firmware's own extraction logic reproduces
                                    cpuid_family_model */
    uint32_t tsc_per_instr;      /* virtual TSC ticks per retired instruction;
                                    RDTSC = instr_count * tsc_per_instr, so
                                    runs stay deterministic (C9) */
} platform_t;

const platform_t *platform_get(platform_id_t id);
const platform_t *platform_by_name(const char *name);
void platform_cpuid(const platform_t *p, uint32_t leaf, uint32_t subleaf,
                     uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

#endif
