#ifndef FDT_H
#define FDT_H

#include <stdint.h>

int fdt_path_offset(const void *fdt, const char *path);
const void *fdt_getprop(const void *fdt, int nodeoffset, const char *name, int *lenp);

int fdt_get_memory_region(const void *fdt, uint64_t *base, uint64_t *size);
int fdt_get_initrd_range(const void *fdt, uint64_t *start, uint64_t *end);

#endif
