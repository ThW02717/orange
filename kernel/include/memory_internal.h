#ifndef MEMORY_INTERNAL_H
#define MEMORY_INTERNAL_H

#include "memory.h"

/* 2^20 pages = 4 GiB with 4 KiB pages. */
#define MEMORY_BUDDY_MAX_ORDER 20

/* Independent of the buddy allocation/free order stored in frame metadata. */
enum page_kind {
    PAGE_KIND_NONE,
    PAGE_KIND_SLAB,
    PAGE_KIND_LARGE,
};

struct page_info {
    uint64_t index;
    enum page_kind kind;
    int allocated_head;
};

/* Caller holds local IRQs off while querying/updating allocation ownership. */
int page_query(uintptr_t base, struct page_info *out);
void page_set_kind(uintptr_t base, enum page_kind kind);
int memory_allocator_log_enabled(void);

void slab_init(void);
void slab_get_stats(struct memory_stats_snapshot *out);

#endif
