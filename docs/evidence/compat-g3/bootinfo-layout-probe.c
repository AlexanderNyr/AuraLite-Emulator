#include <stddef.h>
#include <stdio.h>
#include "boot/shared/boot_info.h"

#define FIELD(name, expr) printf("%-28s %zu\n", name, (size_t)(expr))

int main(void) {
    FIELD("sizeof(boot_fb_t)", sizeof(boot_fb_t));
    FIELD("sizeof(boot_mmap_entry_t)", sizeof(boot_mmap_entry_t));
    FIELD("sizeof(boot_cpu_t)", sizeof(boot_cpu_t));
    FIELD("sizeof(boot_info_t)", sizeof(boot_info_t));
    FIELD("offsetof(magic)", offsetof(boot_info_t, magic));
    FIELD("offsetof(fb)", offsetof(boot_info_t, fb));
    FIELD("offsetof(mmap)", offsetof(boot_info_t, mmap));
    FIELD("offsetof(mmap_count)", offsetof(boot_info_t, mmap_count));
    FIELD("offsetof(hhdm_offset)", offsetof(boot_info_t, hhdm_offset));
    FIELD("offsetof(initrd_phys)", offsetof(boot_info_t, initrd_phys));
    FIELD("offsetof(initrd_size)", offsetof(boot_info_t, initrd_size));
    FIELD("offsetof(cpu_count)", offsetof(boot_info_t, cpu_count));
    FIELD("offsetof(bsp_lapic_id)", offsetof(boot_info_t, bsp_lapic_id));
    FIELD("offsetof(cpus)", offsetof(boot_info_t, cpus));
    FIELD("offsetof(rsdp_phys)", offsetof(boot_info_t, rsdp_phys));
    FIELD("offsetof(boot_from_uefi)", offsetof(boot_info_t, boot_from_uefi));
    return 0;
}
