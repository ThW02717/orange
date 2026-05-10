#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

/* This kernel uses the standard RISC-V 4KB page granularity. */
#define PAGE_SIZE 4096UL

/* Allocator counters used by tests and the shell mem command. */
struct memory_stats_snapshot {
    uint64_t page_allocs;
    uint64_t page_frees;
    uint64_t object_allocs;
    uint64_t object_frees;
    uint64_t slab_reclaims;
    uint64_t total_pages;
    uint64_t free_pages;
    unsigned int empty_slab_limit;
};

/* Per-size-class slab state used by tests and debug output. */
struct memory_slab_class_snapshot {
    unsigned int class_idx;
    unsigned int obj_size;
    unsigned int obj_stride;
    unsigned int partial_count;
    unsigned int full_count;
    unsigned int empty_count;
    unsigned int cached_empty_count;
};

/* Initialize the allocator from the devicetree and bootloader initrd hints. */
void memory_init(const void *fdt, uint64_t initrd_start_hint, uint64_t initrd_end_hint);

/* Mark a physical range unavailable before buddy free lists are built. */
void memory_reserve(uint64_t start, uint64_t size);

/* Page allocator API. p_alloc(n) returns at least n contiguous physical pages;
 * internally it rounds to a power-of-two buddy order. p_free() requires the
 * exact page-aligned pointer returned by p_alloc().
 */
void *p_alloc(unsigned int pages);
void p_free(void *ptr);

/* General kernel allocator API.
 * Small allocations use slab caches; large allocations use whole buddy pages.
 */
void *kmalloc(unsigned long size);
void kfree(void *ptr);

/* Debug / observability helpers for shell commands and allocator tests. */
void memory_print_memstat(void);
void memory_print_slabinfo(void);
void memory_print_buddyinfo(void);
void memory_debug_check_slabs(void);
int memory_check_slabs_ok(void);
void memory_get_stats(struct memory_stats_snapshot *out);
int memory_get_slab_class_snapshot(unsigned int class_idx, struct memory_slab_class_snapshot *out);
int memory_class_for_size(unsigned long size);
void memory_set_allocator_log_enabled(int enabled);

#endif
