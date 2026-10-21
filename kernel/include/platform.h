#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

struct boot_info {
    unsigned long hartid;
    uintptr_t dtb;
    uint64_t initrd_start;
    uint64_t initrd_end;
};

/* Resolve board devices and the archive range once, before allocator init. */
int platform_init(unsigned long hartid, uintptr_t dtb,
                  uint64_t initrd_hint_start, uint64_t initrd_hint_end);
const struct boot_info *platform_boot_info(void);

#endif
