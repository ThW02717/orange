#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

#define PAGE_SIZE 4096UL

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

#endif
