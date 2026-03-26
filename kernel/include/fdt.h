#ifndef FDT_H
#define FDT_H

#include <stdint.h>

#define FDT_MAX_MEM_REGIONS 16

struct fdt_mem_region {
    uint64_t base;
    uint64_t size;
};

int fdt_path_offset(const void *fdt, const char *path);
const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp);
int fdt_find_compatible(const void *fdt, const char *compatible);

uint32_t fdt_totalsize(const void *fdt);
int fdt_get_memory_regions(const void *fdt, struct fdt_mem_region *regions, int max_regions);
int fdt_get_reserved_memory_regions(const void *fdt, struct fdt_mem_region *regions, int max_regions);
int fdt_get_memory_region(const void *fdt, uint64_t *base, uint64_t *size);
int fdt_get_initrd_range(const void *fdt, uint64_t *start, uint64_t *end);
int fdt_get_timebase_frequency(const void *fdt, uint32_t *freq);
int fdt_get_reg_base(const void *fdt, int nodeoffset, uint64_t *base);
int fdt_get_interrupt_id(const void *fdt, int nodeoffset, uint32_t *irq);

#endif
