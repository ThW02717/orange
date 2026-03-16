#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

#define PAGE_SIZE 4096UL

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

struct memory_slab_class_snapshot {
    unsigned int class_idx;
    unsigned int obj_size;
    unsigned int obj_stride;
    unsigned int partial_count;
    unsigned int full_count;
    unsigned int empty_count;
    unsigned int cached_empty_count;
};

// Startup Allocator
void memory_init(const void *fdt, uint64_t initrd_start_hint, uint64_t initrd_end_hint);
void memory_reserve(uint64_t start, uint64_t size);

// Buddy System
void *p_alloc(unsigned int pages);
void p_free(void *ptr);

// Slab
void *kmalloc(unsigned long size);
void kfree(void *ptr);

// Debug / observability
void memory_print_memstat(void);
void memory_print_slabinfo(void);
void memory_print_buddyinfo(void);
void memory_debug_check_slabs(void);
int memory_check_slabs_ok(void);
void memory_get_stats(struct memory_stats_snapshot *out);
int memory_get_slab_class_snapshot(unsigned int class_idx, struct memory_slab_class_snapshot *out);
int memory_class_for_size(unsigned long size);

#endif
