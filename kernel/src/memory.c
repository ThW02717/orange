#include <stdint.h>
#include "memory.h"
#include "fdt.h"
#include "uart.h"

#define MEMORY_MAX_RESERVES 64
#define MEMORY_BUDDY_MAX_ORDER 20 // 2^20 * 4KB = 4GB for OrangePi rv2

// F(-1)/X(-2)/VAL
#define FRAME_BUDDY ((int16_t)-1)
#define FRAME_USED  ((int16_t)-2)
#define FRAME_ALLOC_BASE ((int16_t)-100)
#define SLAB_MAGIC       0x534C4142U // "SLAB"
#define KMALLOC_MAX_SLAB_SIZE 2048U
#define KMEM_CACHE_EMPTY_LIMIT 1U
#define POISON_FREE_OBJECT 0xAAU
#define POISON_RECLAIM_PAGE 0xCCU
// TODO
// Slab Reclamation


// RAM
struct mem_region_info {
    uint64_t base;
    uint64_t size;
    uint64_t first_idx;
    uint64_t page_count;
};

struct reserve_range {
    uint64_t start;
    uint64_t end;
};

enum page_kind {
    PAGE_KIND_NONE = 0,
    PAGE_KIND_SLAB,
    PAGE_KIND_LARGE,
};

enum slab_state {
    SLAB_EMPTY = 0,
    SLAB_PARTIAL,
    SLAB_FULL,
};

struct frame_meta {
    int16_t state;
    int32_t free_prev;
    int32_t free_next;
    enum page_kind kind;
};

// Intrusive linked list
struct kmalloc_free_chunk {
    struct kmalloc_free_chunk *next;
};

struct slab_header;
// Slab page doubly linked list in kmem_cache
struct slab_link {
    struct slab_header *prev;
    struct slab_header *next;
    
};


// manage diffrernt size object
struct kmem_cache {
    unsigned int class_idx; // Size class
    unsigned int obj_size;  // Object size
    unsigned int obj_align; // Object alignment
    unsigned int obj_stride;
    unsigned int empty_count;
    struct slab_header *slabs_partial;
    struct slab_header *slabs_full;
    struct slab_header *slabs_empty;
};
struct slab_header {
    struct slab_link link;
    struct kmem_cache *cache;
    void *freelist; // Head of free objects inside this slab page
    unsigned int inuse; // allocated object
    unsigned int capacity;
    enum slab_state state; // partial, full or empty
    unsigned int magic;
};



// RAM region
static struct mem_region_info g_regions[FDT_MAX_MEM_REGIONS];
static unsigned int g_region_count = 0;
static uint64_t g_total_pages = 0;
static struct reserve_range g_reserves[MEMORY_MAX_RESERVES];
static unsigned int g_reserve_count = 0;
// Startup allocator thing
static uint64_t g_startup_cur = 0;
static uint64_t g_startup_end = 0;
static int g_startup_ready = 0;
// Buddy System thing
static struct frame_meta *g_frame_array = 0;
static int32_t g_free_lists[MEMORY_BUDDY_MAX_ORDER + 1];
static int g_regions_direct_map = 0;
static uint64_t g_direct_map_base = 0;
static uint64_t g_direct_map_first_idx = 0;
static uint64_t g_direct_map_last_idx = 0;
// is buddy +kmalloc  ok
static int g_memory_ready = 0;
static uint64_t g_page_alloc_count = 0;
static uint64_t g_page_free_count = 0;
static uint64_t g_object_alloc_count = 0;
static uint64_t g_object_free_count = 0;
static uint64_t g_slab_reclaim_count = 0;
static int g_allocator_log_enabled = 0;

// slab allocate thing
// kmem_cache for each class

static const uint16_t g_kmalloc_classes[] = { 16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U };
#define KMALLOC_CLASS_COUNT ((unsigned int)(sizeof(g_kmalloc_classes) / sizeof(g_kmalloc_classes[0])))
static struct kmem_cache g_kmem_caches[KMALLOC_CLASS_COUNT];
// Linker
extern char _phys_start;
extern char _phys_end;

/////////////////////////////////////////////////* HELPER FUNCTION*/////////////////////////////////////////////



// Is initrd good?
static int initrd_hint_valid(uint64_t start, uint64_t end) {
    if (start == 0 || end <= start) {
        return 0;
    }
    if ((end - start) > (256UL * 1024UL * 1024UL)) {
        return 0;
    }
    return 1;
}
// Helper
static uint64_t align_down_u64(uint64_t v, uint64_t a) {
    return v & ~(a - 1U);
}
// go up then align down
static uint64_t align_up_u64(uint64_t v, uint64_t a) {
    return (v + (a - 1U)) & ~(a - 1U);
}
// Ensure startp alloca not crash with reserve region
static int ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end) {
    return a_start < b_end && b_start < a_end;
}
// Command line msg 
// For observability
static void log_prefix(void) {
    uart_send_string("[Allocator] ");
}

static void log_hex_u64(uint64_t value) {
    uart_send_string("0x");
    uart_send_hex((unsigned long)value);
}

static void log_page_range(uint64_t start_idx, unsigned int order) {
    uint64_t end_idx = start_idx + (1ULL << order) - 1ULL;

    uart_send_string("[");
    uart_send_dec((unsigned long)start_idx);
    uart_send_string(", ");
    uart_send_dec((unsigned long)end_idx);
    uart_send_string("]");
}

static unsigned int slab_list_count(struct slab_header *head) {
    unsigned int n = 0;
    while (head != 0) {
        n++;
        head = head->link.next;
    }
    return n;
}

static unsigned int freelist_count_order(unsigned int order) {
    unsigned int n = 0;
    int32_t cur = g_free_lists[order];

    while (cur >= 0) {
        n++;
        cur = g_frame_array[(uint64_t)cur].free_next;
    }
    return n;
}

static uint64_t total_free_pages_count(void) {
    unsigned int order;
    uint64_t total = 0;

    for (order = 0; order <= MEMORY_BUDDY_MAX_ORDER; order++) {
        total += (uint64_t)freelist_count_order(order) << order;
    }
    return total;
}

static void poison_memory(void *ptr, uint64_t size, uint8_t value) {
    uint8_t *p = (uint8_t *)ptr;
    uint64_t i;

    if (ptr == 0) {
        return;
    }
    for (i = 0; i < size; i++) {
        p[i] = value;
    }
}

static void slab_log_invariant_error(unsigned int class_idx, const char *list_name,
                                     struct slab_header *slab, const char *reason) {
    if (!g_allocator_log_enabled) {
        return;
    }
    log_prefix();
    uart_send_string("[Invariant] class ");
    uart_send_dec(class_idx);
    uart_send_string(" ");
    uart_send_string(list_name);
    uart_send_string(" slab ");
    log_hex_u64((uint64_t)(uintptr_t)slab);
    uart_send_string(": ");
    uart_send_string(reason);
    uart_send_string("\n");
}

static int slab_check_list_invariants(unsigned int class_idx, const char *list_name,
                                      struct slab_header *head, enum slab_state expected_state) {
    int ok = 1;

    while (head != 0) {
        if (head->state != expected_state) {
            slab_log_invariant_error(class_idx, list_name, head, "state mismatch");
            ok = 0;
        }
        if (expected_state == SLAB_PARTIAL && head->freelist == 0) {
            slab_log_invariant_error(class_idx, list_name, head, "partial slab has empty freelist");
            ok = 0;
        }
        if (expected_state == SLAB_FULL && head->inuse != head->capacity) {
            slab_log_invariant_error(class_idx, list_name, head, "full slab inuse != capacity");
            ok = 0;
        }
        if (expected_state == SLAB_EMPTY && head->inuse != 0) {
            slab_log_invariant_error(class_idx, list_name, head, "empty slab inuse != 0");
            ok = 0;
        }
        head = head->link.next;
    }
    return ok;
}
// Buddy Page account
static int16_t alloc_tag_from_order(unsigned int order) {
    return (int16_t)(FRAME_ALLOC_BASE - (int16_t)order);
}

static int is_alloc_head_tag(int16_t tag) {
    return tag <= FRAME_ALLOC_BASE;
}

static unsigned int order_from_alloc_tag(int16_t tag) {
    return (unsigned int)(FRAME_ALLOC_BASE - tag);
}

static int pa_to_index(uint64_t pa, uint64_t *idx_out, unsigned int *rid_out);

/*Slab page list helper*/
static void cache_add_partial(struct kmem_cache *cache, struct slab_header *slab) {
    if (cache == 0 || slab == 0) {
        return;
    }

    slab->cache = cache;
    slab->state = SLAB_PARTIAL;
    slab->link.prev = 0;
    slab->link.next = cache->slabs_partial;
    if (cache->slabs_partial != 0) {
        cache->slabs_partial->link.prev = slab;
    }
    cache->slabs_partial = slab;
}
static void cache_add_full(struct kmem_cache *cache, struct slab_header *slab) {
    if (cache == 0 || slab == 0) {
        return;
    }

    slab->cache = cache;
    slab->state = SLAB_FULL;
    slab->link.prev = 0;
    slab->link.next = cache->slabs_full;
    if (cache->slabs_full != 0) {
        cache->slabs_full->link.prev = slab;
    }
    cache->slabs_full = slab;
}

static void cache_add_empty(struct kmem_cache *cache, struct slab_header *slab) {
    if (cache == 0 || slab == 0) {
        return;
    }

    slab->cache = cache;
    slab->state = SLAB_EMPTY;
    slab->link.prev = 0;
    slab->link.next = cache->slabs_empty;
    if (cache->slabs_empty != 0) {
        cache->slabs_empty->link.prev = slab;
    }
    cache->slabs_empty = slab;
    cache->empty_count++;
}

static void cache_remove_slab(struct slab_header *slab) {
    struct kmem_cache *cache;

    if (slab == 0 || slab->cache == 0) {
        return;
    }
    // which size cache
    cache = slab->cache;

    if (slab->link.prev != 0) {
        slab->link.prev->link.next = slab->link.next;
    } else {
        if (slab->state == SLAB_PARTIAL) {
            cache->slabs_partial = slab->link.next;
        } else if (slab->state == SLAB_FULL) {
            cache->slabs_full = slab->link.next;
        } else {
            cache->slabs_empty = slab->link.next;
        }
    }

    if (slab->link.next != 0) {
        slab->link.next->link.prev = slab->link.prev;
    }

    if (slab->state == SLAB_EMPTY && cache->empty_count > 0) {
        cache->empty_count--;
    }

    slab->link.prev = 0;
    slab->link.next = 0;
}

static void cache_move_partial_to_full(struct kmem_cache *cache, struct slab_header *slab) {
    if (cache == 0 || slab == 0) {
        return;
    }

    cache_remove_slab(slab);
    cache_add_full(cache, slab);
}

static void cache_move_full_to_partial(struct kmem_cache *cache, struct slab_header *slab) {
    if (cache == 0 || slab == 0) {
        return;
    }

    cache_remove_slab(slab);
    cache_add_partial(cache, slab);
}

static void cache_move_empty_to_partial(struct kmem_cache *cache, struct slab_header *slab) {
    if (cache == 0 || slab == 0) {
        return;
    }

    cache_remove_slab(slab);
    cache_add_partial(cache, slab);
}

/*object  list helper*/
static void slab_push_free(struct slab_header *slab, void *obj) {
    struct kmalloc_free_chunk *chunk;

    if (slab == 0 || obj == 0) {
        return;
    }

    chunk = (struct kmalloc_free_chunk *)obj;
    chunk->next = (struct kmalloc_free_chunk *)slab->freelist;
    slab->freelist = obj;
}

static void *slab_pop_free(struct slab_header *slab) {
    struct kmalloc_free_chunk *chunk;

    if (slab == 0 || slab->freelist == 0) {
        return 0;
    }

    chunk = (struct kmalloc_free_chunk *)slab->freelist;
    slab->freelist = chunk->next;
    return (void *)chunk;
}

static int slab_freelist_contains(struct slab_header *slab, void *obj) {
    struct kmalloc_free_chunk *cur;

    if (slab == 0 || obj == 0) {
        return 0;
    }

    cur = (struct kmalloc_free_chunk *)slab->freelist;
    while (cur != 0) {
        if ((void *)cur == obj) {
            return 1;
        }
        cur = cur->next;
    }
    return 0;
}

static int size_to_class(unsigned long size) {
    unsigned int i;

    if (size == 0 || size > KMALLOC_MAX_SLAB_SIZE) {
        return -1;
    }

    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        if (size <= (unsigned long)g_kmalloc_classes[i]) {
            return (int)i;
        }
    }
    return -1;
}

static unsigned int class_to_size(unsigned int class_idx) {
    if (class_idx >= KMALLOC_CLASS_COUNT) {
        return 0;
    }
    return (unsigned int)g_kmalloc_classes[class_idx];
}

static unsigned int slab_calc_stride(unsigned int obj_size, unsigned int align) {
    if (obj_size == 0) {
        return 0;
    }
    if (align == 0) {
        align = 1;
    }
    return (unsigned int)align_up_u64((uint64_t)obj_size, (uint64_t)align);
}

static void *slab_object_start(void *page_base, struct kmem_cache *cache) {
    uint64_t base;
    uint64_t start;

    if (page_base == 0 || cache == 0) {
        return 0;
    }

    base = (uint64_t)(uintptr_t)page_base;
    start = align_up_u64(base + sizeof(struct slab_header), (uint64_t)cache->obj_align);
    return (void *)(uintptr_t)start;
}

static unsigned int slab_calc_capacity(void *page_base, struct kmem_cache *cache) {
    uint64_t base;
    uint64_t start;
    uint64_t usable;

    if (page_base == 0 || cache == 0 || cache->obj_stride == 0) {
        return 0;
    }

    base = (uint64_t)(uintptr_t)page_base;
    start = (uint64_t)(uintptr_t)slab_object_start(page_base, cache);
    if (start < base || start >= (base + PAGE_SIZE)) {
        return 0;
    }

    usable = (base + PAGE_SIZE) - start;
    return (unsigned int)(usable / (uint64_t)cache->obj_stride);
}

static enum page_kind page_kind_get(uint64_t idx) {
    if (idx >= g_total_pages || g_frame_array == 0) {
        return PAGE_KIND_NONE;
    }
    return g_frame_array[idx].kind;
}

static void page_kind_set(uint64_t idx, enum page_kind kind) {
    if (idx >= g_total_pages || g_frame_array == 0) {
        return;
    }
    g_frame_array[idx].kind = kind;
}

// Enclosure babbaa
void memory_reserve(uint64_t start, uint64_t size) {
    uint64_t end;
    uint64_t rs;
    uint64_t re;
    // BC
    if (size == 0) {
        return;
    }
    // Avoid overflow
    end = start + size;
    if (end < start) {
        end = (uint64_t)-1;
    }
    // Align page size
    // we need the entire page
    rs = align_down_u64(start, PAGE_SIZE);
    if (end > ((uint64_t)-1 - (PAGE_SIZE - 1U))) {
        re = (uint64_t)-1;
    } else {
        re = align_up_u64(end, PAGE_SIZE);
    }
    if (re <= rs) {
        return;
    }

    if (g_reserve_count >= MEMORY_MAX_RESERVES) {
        log_prefix();
        uart_send_string("reserve table full\n");
        return;
    }
    // write into reserves
    g_reserves[g_reserve_count].start = rs;
    g_reserves[g_reserve_count].end = re;
    g_reserve_count++;
    // logging
    log_prefix();
    uart_send_string("[Reserve] [");
    log_hex_u64(rs);
    uart_send_string(", ");
    log_hex_u64(re);
    uart_send_string(")");
    {
        uint64_t start_idx;
        uint64_t end_idx;
        if (pa_to_index(rs, &start_idx, 0) == 0 && pa_to_index(re - PAGE_SIZE, &end_idx, 0) == 0) {
            uart_send_string(" pages [");
            uart_send_dec((unsigned long)start_idx);
            uart_send_string(", ");
            uart_send_dec((unsigned long)(end_idx + 1ULL));
            uart_send_string(")");
        }
    }
    uart_send_string("\n");
}
// check if reserve
// traverse g_reserves region
static int is_reserved_pa(uint64_t pa) {
    unsigned int i;
    for (i = 0; i < g_reserve_count; i++) {
        if (pa >= g_reserves[i].start && pa < g_reserves[i].end) {
            return 1;
        }
    }
    return 0;
}
// Check if startup allocator region ol
static void startup_allocator_init(uint64_t base, uint64_t size) {
    uint64_t end = base + size;
    if (end < base) {
        end = (uint64_t)-1;
    }
    g_startup_cur = align_up_u64(base, 16U);
    g_startup_end = align_down_u64(end, 16U);
    if (g_startup_cur < PAGE_SIZE) {
        g_startup_cur = PAGE_SIZE;
    }
    g_startup_ready = (g_startup_end > g_startup_cur) ? 1 : 0;
}
// all you can use void*
static void *startup_alloc(uint64_t size, uint64_t align) {
    uint64_t p;
    uint64_t end;
    unsigned int i;
    int moved;
    // pre check
    if (!g_startup_ready || size == 0) {
        return 0;
    }
    if (align == 0) {
        align = 16U;
    }
    if ((align & (align - 1U)) != 0U) {
        return 0;
    }
    // lAST ONE NO OVERLAPPED
    p = align_up_u64(g_startup_cur, align);
    for (;;) {
        moved = 0;
        end = p + size;
        if (end < p) {
            return 0;
        }
        for (i = 0; i < g_reserve_count; i++) {
            // If overlapp with the reserved region then jump behind
            if (ranges_overlap(p, end, g_reserves[i].start, g_reserves[i].end)) {
                p = align_up_u64(g_reserves[i].end, align);
                moved = 1;
                break; // re-check
            }
        }
        // no move: we good with all the reserve region
        if (!moved) {
            break;
        }
    }
    // update: write into reserve region
    end = p + size;
    if (end < p || end > g_startup_end) {
        return 0;
    }

    g_startup_cur = end;
    memory_reserve(p, size);
    // give permission
    return (void *)(uintptr_t)p;
}

static int find_region_by_index(uint64_t idx, unsigned int *rid_out) {
    unsigned int rid;

    if (g_regions_direct_map) {
        if (idx >= g_direct_map_first_idx && idx < g_direct_map_last_idx) {
            if (rid_out != 0) {
                *rid_out = 0;
            }
            return 0;
        }
        return -1;
    }

    for (rid = 0; rid < g_region_count; rid++) {
        uint64_t first = g_regions[rid].first_idx;
        uint64_t last = first + g_regions[rid].page_count;
        if (idx >= first && idx < last) {
            if (rid_out != 0) {
                *rid_out = rid;
            }
            return 0;
        }
    }
    return -1;
}
// Page index to real address9
static int index_to_pa(uint64_t idx, uint64_t *pa_out) {
    unsigned int rid;

    if (pa_out == 0) {
        return -1;
    }

    if (g_regions_direct_map) {
        if (idx < g_direct_map_first_idx || idx >= g_direct_map_last_idx) {
            return -1;
        }
        *pa_out = g_direct_map_base + (idx - g_direct_map_first_idx) * PAGE_SIZE;
        return 0;
    }

    if (find_region_by_index(idx, &rid) != 0) {
        return -1;
    }
    *pa_out = g_regions[rid].base + (idx - g_regions[rid].first_idx) * PAGE_SIZE;
    return 0;
}
// user give back
static int pa_to_index(uint64_t pa, uint64_t *idx_out, unsigned int *rid_out) {
    unsigned int rid;

    if (idx_out == 0) {
        return -1;
    }

    if (g_regions_direct_map) {
        uint64_t end = g_direct_map_base + (g_direct_map_last_idx - g_direct_map_first_idx) * PAGE_SIZE;

        if (pa < g_direct_map_base || pa >= end || (pa & (PAGE_SIZE - 1U)) != 0U) {
            return -1;
        }
        *idx_out = g_direct_map_first_idx + ((pa - g_direct_map_base) / PAGE_SIZE);
        if (rid_out != 0) {
            *rid_out = 0;
        }
        return 0;
    }

    for (rid = 0; rid < g_region_count; rid++) {
        uint64_t base = g_regions[rid].base;
        uint64_t end = base + g_regions[rid].size;
        if (pa >= base && pa < end) {
            uint64_t off = pa - base;
            uint64_t page = off / PAGE_SIZE;
            if ((off % PAGE_SIZE) != 0U || page >= g_regions[rid].page_count) {
                return -1;
            }
            *idx_out = g_regions[rid].first_idx + page;
            if (rid_out != 0) {
                *rid_out = rid;
            }
            return 0;
        }
    }
    return -1;
}
/////////////////////////////////////////////////////* FRAME ARRAY*///////////////////////////////////////////
// Frame array mark helper
static void block_mark_free(uint64_t idx, unsigned int order) {
    uint64_t pages = 1ULL << order;
    uint64_t i;
    g_frame_array[idx].state = (int16_t)order;
    for (i = 1; i < pages; i++) {
        g_frame_array[idx + i].state = FRAME_BUDDY;
    }
}

// mark positive num or -1
static void block_mark_alloc(uint64_t idx, unsigned int order) {
    uint64_t pages = 1ULL << order;
    uint64_t i;
    g_frame_array[idx].state = alloc_tag_from_order(order);
    for (i = 1; i < pages; i++) {
        g_frame_array[idx + i].state = FRAME_USED;
    }
}
// mark -2 or -10X
static void freelist_push(unsigned int order, uint64_t idx) {
    int32_t i32 = (int32_t)idx;
    int32_t old_head = g_free_lists[order];

    g_frame_array[idx].free_prev = -1;
    g_frame_array[idx].free_next = old_head;
    if (old_head >= 0) {
        g_frame_array[(uint64_t)old_head].free_prev = i32;
    }
    g_free_lists[order] = i32;

    if (g_allocator_log_enabled) {
        log_prefix();
        uart_send_string("[+] Add page ");
        uart_send_dec((unsigned long)idx);
        uart_send_string(" to order ");
        uart_send_dec(order);
        uart_send_string(". Range of pages: ");
        log_page_range(idx, order);
        uart_send_string("\n");
    }
}

/////////////////////////////////////////////////////* FREE LIST*///////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////
static int32_t freelist_pop(unsigned int order) {
    int32_t head = g_free_lists[order];
    if (head >= 0) {
        int32_t next = g_frame_array[(uint64_t)head].free_next;

        g_free_lists[order] = next;
        if (next >= 0) {
            g_frame_array[(uint64_t)next].free_prev = -1;
        }
        g_frame_array[(uint64_t)head].free_prev = -1;
        g_frame_array[(uint64_t)head].free_next = -1;

        if (g_allocator_log_enabled) {
            log_prefix();
            uart_send_string("[-] Remove page ");
            uart_send_dec((unsigned long)head);
            uart_send_string(" from order ");
            uart_send_dec(order);
            uart_send_string(". Range of pages: ");
            log_page_range((uint64_t)head, order);
            uart_send_string("\n");
        }
    }
    return head;
}

// Remove a known free block in O(1) from the order freelist.
static int freelist_remove(unsigned int order, uint64_t idx) {
    int32_t prev;
    int32_t next;

    if (idx >= g_total_pages) {
        return -1;
    }
    if (g_frame_array[idx].state != (int16_t)order) {
        return -1;
    }

    prev = g_frame_array[idx].free_prev;
    next = g_frame_array[idx].free_next;
    if (prev < 0) {
        if (g_free_lists[order] != (int32_t)idx) {
            return -1;
        }
        g_free_lists[order] = next;
    } else {
        g_frame_array[(uint64_t)prev].free_next = next;
    }
    if (next >= 0) {
        g_frame_array[(uint64_t)next].free_prev = prev;
    }
    g_frame_array[idx].free_prev = -1;
    g_frame_array[idx].free_next = -1;

    if (g_allocator_log_enabled) {
        log_prefix();
        uart_send_string("[-] Remove page ");
        uart_send_dec((unsigned long)idx);
        uart_send_string(" from order ");
        uart_send_dec(order);
        uart_send_string(". Range of pages: ");
        log_page_range(idx, order);
        uart_send_string("\n");
    }
    return 0;
}
// Find my potential buddddddddyyyyyyyy
static int buddy_index(uint64_t idx, unsigned int order, uint64_t *out) {
    unsigned int rid;
    uint64_t local_idx;
    uint64_t buddy_local;
    uint64_t size = 1ULL << order;

    if (find_region_by_index(idx, &rid) != 0) {
        return -1;
    }

    local_idx = idx - g_regions[rid].first_idx;
    // XOR ！！！
    buddy_local = local_idx ^ size;
    if (buddy_local >= g_regions[rid].page_count) {
        return -1;
    }

    *out = g_regions[rid].first_idx + buddy_local;
    if (g_allocator_log_enabled) {
        log_prefix();
        uart_send_string("[*] Buddy found! buddy idx: ");
        uart_send_dec((unsigned long)*out);
        uart_send_string(" for page ");
        uart_send_dec((unsigned long)idx);
        uart_send_string(" with order ");
        uart_send_dec(order);
        uart_send_string("\n");
    }
    return 0;
}
// MERGE
static unsigned int floor_log2_u64(uint64_t v) {
    unsigned int n = 0;
    while ((1ULL << (n + 1U)) <= v) {
        n++;
    }
    return n;
}
// SLice and merge the memory
static void add_free_run(unsigned int rid, uint64_t run_start_page, uint64_t run_pages) {
    while (run_pages > 0) {
        unsigned int order = floor_log2_u64(run_pages);
        uint64_t idx;

        if (order > MEMORY_BUDDY_MAX_ORDER) {
            order = MEMORY_BUDDY_MAX_ORDER;
        }
        // bit manipulation
        // order 3 for example: 00000100 -> 00000111 
        // if the result of & is 0 then runstart page can be divide by 2^3
        while (order > 0 && (run_start_page & ((1ULL << order) - 1ULL)) != 0ULL) {
            order--;
        }

        idx = g_regions[rid].first_idx + run_start_page;
        block_mark_free(idx, order);
        freelist_push(order, idx);

        run_start_page += (1ULL << order);
        run_pages -= (1ULL << order);
    }
}

// allocate frame array and freelist in startup
static int buddy_setup_metadata(void) {
    uint64_t frame_array_bytes = g_total_pages * sizeof(struct frame_meta);
    uint64_t i;

    g_frame_array = (struct frame_meta *)startup_alloc(frame_array_bytes, PAGE_SIZE);
    if (g_frame_array == 0) {
        return -1;
    }

    for (i = 0; i < g_total_pages; i++) {
        g_frame_array[i].state = FRAME_USED;
        g_frame_array[i].free_prev = -1;
        g_frame_array[i].free_next = -1;
        g_frame_array[i].kind = PAGE_KIND_NONE;
    }
    for (i = 0; i <= MEMORY_BUDDY_MAX_ORDER; i++) {
        g_free_lists[i] = -1;
    }
    return 0;
}
///////////////////////////////////////////////////////////////////////////////* Build our budddy system init*////////////////////////////////////////////////////////
static void buddy_build_initial_state(void) {
    unsigned int rid;
    for (rid = 0; rid < g_region_count; rid++) {
        uint64_t p;
        uint64_t run_start = 0;
        uint64_t run_len = 0;

        for (p = 0; p < g_regions[rid].page_count; p++) {
            uint64_t idx = g_regions[rid].first_idx + p;
            uint64_t pa = g_regions[rid].base + p * PAGE_SIZE;
            // reserved checked
            if (is_reserved_pa(pa)) {
                if (run_len > 0) {
                    add_free_run(rid, run_start, run_len);
                    run_len = 0;
                }
                g_frame_array[idx].state = FRAME_USED;
                continue;
            }

            if (run_len == 0) {
                run_start = p;
            }
            run_len++;
        }
        // send to the slice function
        if (run_len > 0) {
            add_free_run(rid, run_start, run_len);
        }
    }
}

void *p_alloc(unsigned int pages) {
    unsigned int need_order = 0;
    unsigned int cur_order;
    int32_t idx32;
    uint64_t idx;
    uint64_t pa = 0;

    if (!g_memory_ready || pages == 0) {
        return 0;
    }
    // what's the order we need?
    while ((1ULL << need_order) < (uint64_t)pages) {
        need_order++;
        if (need_order > MEMORY_BUDDY_MAX_ORDER) {
            return 0;
        }
    }
    // Do we have it?
    cur_order = need_order;
    while (cur_order <= MEMORY_BUDDY_MAX_ORDER && g_free_lists[cur_order] < 0) {
        cur_order++;
    }
    if (cur_order > MEMORY_BUDDY_MAX_ORDER) {
        return 0;
    }
    // pop the pages we need
    idx32 = freelist_pop(cur_order);
    if (idx32 < 0) {
        return 0;
    }
    idx = (uint64_t)idx32;
    // cut cut cut until we get the size we need
    while (cur_order > need_order) {
        uint64_t buddy_idx;
        uint64_t buddy_pa = 0;
        cur_order--;
        buddy_idx = idx + (1ULL << cur_order);
        block_mark_free(buddy_idx, cur_order);
        freelist_push(cur_order, buddy_idx);

        if (index_to_pa(buddy_idx, &buddy_pa) == 0 && g_allocator_log_enabled) {
            log_prefix();
            uart_send_string("[+] Add split page ");
            uart_send_dec((unsigned long)buddy_idx);
            uart_send_string(" to order ");
            uart_send_dec(cur_order);
            uart_send_string(". Range of pages: ");
            log_page_range(buddy_idx, cur_order);
            uart_send_string(" addr=");
            log_hex_u64(buddy_pa);
            uart_send_string("\n");
        }

        block_mark_free(idx, cur_order);
    }

    block_mark_alloc(idx, need_order);
    if (index_to_pa(idx, &pa) != 0) {
        return 0;
    }

    if (g_allocator_log_enabled) {
        log_prefix();
        uart_send_string("[Page] Allocate ");
        log_hex_u64(pa);
        uart_send_string(" at order ");
        uart_send_dec(need_order);
        uart_send_string(", page ");
        uart_send_dec((unsigned long)idx);
        uart_send_string(". Next address at order ");
        uart_send_dec(need_order);
        uart_send_string(": ");
        if (index_to_pa(idx + (1ULL << need_order), &pa) == 0) {
            log_hex_u64(pa);
        } else {
            uart_send_string("none");
        }
        uart_send_string("\n");
    }

    if (index_to_pa(idx, &pa) != 0) {
        return 0;
    }
    g_page_alloc_count++;
    return (void *)(uintptr_t)pa;
}

void p_free(void *ptr) {
    uint64_t pa;
    uint64_t idx;
    int16_t tag;
    unsigned int order;
    uint64_t merged_idx;
    unsigned int merged_order;
    // Safe check
    if (!g_memory_ready || ptr == 0) {
        return;
    }

    pa = (uint64_t)(uintptr_t)ptr;
    if ((pa & (PAGE_SIZE - 1U)) != 0U) {
        return;
    }
    if (pa_to_index(pa, &idx, 0) != 0) {
        return;
    }

    tag = g_frame_array[idx].state;
    if (!is_alloc_head_tag(tag)) {
        return;
    }
    // what;s the order of free mem
    order = order_from_alloc_tag(tag);
    merged_idx = idx;
    merged_order = order;
    // Check if there are empty memory that can be merged
    while (merged_order < MEMORY_BUDDY_MAX_ORDER) {
        uint64_t buddy;
        if (buddy_index(merged_idx, merged_order, &buddy) != 0) {
            break;
        }
        if (g_frame_array[buddy].state != (int16_t)merged_order) {
            break;
        }
        if (freelist_remove(merged_order, buddy) != 0) {
            break;
        }
        if (buddy < merged_idx) {
            merged_idx = buddy;
        }
        merged_order++;
    }

    block_mark_free(merged_idx, merged_order);
    freelist_push(merged_order, merged_idx);

    if (g_allocator_log_enabled) {
        log_prefix();
        uart_send_string("[Page] Free ");
        log_hex_u64(pa);
        uart_send_string(" and add back to order ");
        uart_send_dec(merged_order);
        uart_send_string(", page ");
        uart_send_dec((unsigned long)merged_idx);
        uart_send_string(". Next address at order ");
        uart_send_dec(merged_order);
        uart_send_string(": ");
        if (g_free_lists[merged_order] >= 0) {
            uint64_t next_pa = 0;
            if (index_to_pa((uint64_t)g_free_lists[merged_order], &next_pa) == 0) {
                log_hex_u64(next_pa);
            } else {
                uart_send_string("none");
            }
        } else {
            uart_send_string("none");
        }
        uart_send_string("\n");
    }
    g_page_free_count++;
}

static int kmalloc_refill_class(unsigned int class_idx) {
    // request one page for slab
    void *page = p_alloc(1U);
    struct kmem_cache *cache;
    struct slab_header *slab;
    void *obj_start;
    uint64_t base;
    uint64_t page_idx;
    uint64_t chunks;
    uint64_t i;
    uint64_t chunk_size;

    if (page == 0) {
        return -1;
    }
    // Initialization for our new slab page. Buddy ownership/state stays in g_frame_array.
    base = (uint64_t)(uintptr_t)page;
    if (pa_to_index(base, &page_idx, 0) != 0) {
        p_free(page);
        return -1;
    }
    page_kind_set(page_idx, PAGE_KIND_SLAB);

    cache = &g_kmem_caches[class_idx];
    slab = (struct slab_header *)page;
    slab->link.prev = 0;
    slab->link.next = 0;
    slab->magic = SLAB_MAGIC;
    slab->cache = cache;
    slab->freelist = 0;
    slab->inuse = 0;
    slab->capacity = slab_calc_capacity(page, cache);
    slab->state = SLAB_PARTIAL;

    obj_start = slab_object_start(page, cache);
    chunk_size = (uint64_t)cache->obj_stride;
    chunks = slab->capacity;
    if (chunks == 0) {
        page_kind_set(page_idx, PAGE_KIND_NONE);
        p_free(page);
        return -1;
    }
    // Slide the page into many object
    for (i = 0; i < chunks; i++) {
        uint64_t caddr = (uint64_t)(uintptr_t)obj_start + i * chunk_size;
        void *obj = (void *)(uintptr_t)caddr;
        slab_push_free(slab, obj);
    }
    // Refill now populates this slab page's freelist and links it into the cache.
    cache_add_partial(cache, slab);
    return 0;
}

void *kmalloc(unsigned long size) {
    if (!g_memory_ready || size == 0UL) {
        return 0;
    }
    // Slab Path
    if (size <= KMALLOC_MAX_SLAB_SIZE) {
        int cls = size_to_class(size);
        struct kmem_cache *cache;
        struct slab_header *slab;
        void *obj;

        if (cls < 0) {
            return 0;
        }

        cache = &g_kmem_caches[(unsigned int)cls];
        if (cache->slabs_partial == 0) {
            if (cache->slabs_empty != 0) {
                cache_move_empty_to_partial(cache, cache->slabs_empty);
            } else if (kmalloc_refill_class((unsigned int)cls) != 0) {
                return 0;
            }
        }
        // Safety check
        if (cache->slabs_partial == 0) {
            log_prefix();
            uart_send_string("[Error] kmalloc no partial slab after refill/reuse for class ");
            uart_send_dec((unsigned long)cls);
            uart_send_string("\n");
            return 0;
        }
        // Find the first partial slab that still has a free object.
        slab = cache->slabs_partial;
        while (slab != 0 && slab->freelist == 0) {
            slab = slab->link.next;
        }
        if (slab == 0) {
            log_prefix();
            uart_send_string("[Error] kmalloc partial list exhausted for class ");
            uart_send_dec((unsigned long)cls);
            uart_send_string("\n");
            return 0;
        }
        // Get free objet
        obj = slab_pop_free(slab);
        if (obj == 0) {
            log_prefix();
            uart_send_string("[Error] kmalloc slab_pop_free failed for class ");
            uart_send_dec((unsigned long)cls);
            uart_send_string(", page ");
            log_hex_u64((uint64_t)(uintptr_t)slab);
            uart_send_string("\n");
            return 0;
        }
        // If full......
        slab->inuse++;
        if (slab->inuse == slab->capacity) {
            cache_move_partial_to_full(cache, slab);
        }
        // For observabiliy
        if (g_allocator_log_enabled) {
            log_prefix();
            uart_send_string("[Object] Allocate ");
            log_hex_u64((uint64_t)(uintptr_t)obj);
            uart_send_string(" at object size ");
            uart_send_dec(cache->obj_size);
            uart_send_string(" (class ");
            uart_send_dec((unsigned long)cls);
            uart_send_string(", inuse ");
            uart_send_dec(slab->inuse);
            uart_send_string("/");
            uart_send_dec(slab->capacity);
            uart_send_string(")");
            uart_send_string("\n");
        }
        g_object_alloc_count++;
        return obj;
    }
    // Large path
    {
        void *base;
        uint64_t base_idx;
        // round the value bilibala
        unsigned long pages = (size + PAGE_SIZE - 1UL) / PAGE_SIZE;

        base = p_alloc((unsigned int)pages);
        if (base == 0) {
            return 0;
        }
        if (pa_to_index((uint64_t)(uintptr_t)base, &base_idx, 0) != 0) {
            p_free(base);
            return 0;
        }
        page_kind_set(base_idx, PAGE_KIND_LARGE);
        return base;
    }
}

void kfree(void *ptr) {
    uint64_t page_base;
    uint64_t page_idx;
    uint64_t ptr_addr;
    enum page_kind kind;

    if (!g_memory_ready || ptr == 0) {
        return;
    }
    ptr_addr = (uint64_t)(uintptr_t)ptr;
    page_base = align_down_u64((uint64_t)(uintptr_t)ptr, PAGE_SIZE);
    if (pa_to_index(page_base, &page_idx, 0) != 0) {
        log_prefix();
        uart_send_string("[Error] kfree invalid page base for ptr ");
        log_hex_u64(ptr_addr);
        uart_send_string("\n");
        return;
    }
    kind = page_kind_get(page_idx);

    if (kind == PAGE_KIND_NONE) {
        log_prefix();
        uart_send_string("[Error] kfree page kind NONE for ptr ");
        log_hex_u64(ptr_addr);
        uart_send_string(", page ");
        log_hex_u64(page_base);
        uart_send_string("\n");
        return;
    }

    if (kind == PAGE_KIND_SLAB) {
        struct slab_header *slab = (struct slab_header *)(uintptr_t)page_base;
        struct kmem_cache *cache;
        uint64_t obj_start;
        uint64_t obj_end;
        uint64_t offset;

        if (slab->magic != SLAB_MAGIC) {
            log_prefix();
            uart_send_string("[Error] kfree slab magic mismatch at page ");
            log_hex_u64(page_base);
            uart_send_string(" for ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }
        cache = slab->cache;
        if (cache == 0 || cache->obj_stride == 0) {
            log_prefix();
            uart_send_string("[Error] kfree slab cache metadata invalid at page ");
            log_hex_u64(page_base);
            uart_send_string("\n");
            return;
        }

        obj_start = (uint64_t)(uintptr_t)slab_object_start((void *)(uintptr_t)page_base, cache);
        obj_end = obj_start + (uint64_t)cache->obj_stride * (uint64_t)slab->capacity;
        if (ptr_addr < obj_start || ptr_addr >= obj_end) {
            log_prefix();
            uart_send_string("[Error] kfree slab ptr out of object range: ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string(", range [");
            log_hex_u64(obj_start);
            uart_send_string(", ");
            log_hex_u64(obj_end);
            uart_send_string(")\n");
            return;
        }

        offset = ptr_addr - obj_start;
        if ((offset % (uint64_t)cache->obj_stride) != 0) {
            log_prefix();
            uart_send_string("[Error] kfree slab ptr misaligned to object stride: ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string(", stride ");
            uart_send_dec(cache->obj_stride);
            uart_send_string("\n");
            return;
        }
        if (slab->inuse == 0) {
            log_prefix();
            uart_send_string("[Error] kfree slab underflow attempt at ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }
        // double free defense
        if (slab_freelist_contains(slab, ptr)) {
            log_prefix();
            uart_send_string("[Object] Double free detected at ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }

        poison_memory(ptr, cache->obj_size, POISON_FREE_OBJECT);
        slab_push_free(slab, ptr);
        slab->inuse--;
        g_object_free_count++;
        if (slab->state == SLAB_FULL) {
            cache_move_full_to_partial(cache, slab);
        }
        if (slab->inuse == 0) {
            cache_remove_slab(slab);
            if (cache->empty_count < KMEM_CACHE_EMPTY_LIMIT) {
                cache_add_empty(cache, slab);

                if (g_allocator_log_enabled) {
                    log_prefix();
                    uart_send_string("[Object] Free ");
                    log_hex_u64(ptr_addr);
                    uart_send_string(" at object size ");
                    uart_send_dec(cache->obj_size);
                    uart_send_string(". [Slab] Keep empty page ");
                    log_hex_u64(page_base);
                    uart_send_string("\n");
                }
            } else {
                page_kind_set(page_idx, PAGE_KIND_NONE);
                slab->magic = 0;
                slab->cache = 0;
                slab->freelist = 0;
                poison_memory((void *)(uintptr_t)page_base, PAGE_SIZE, POISON_RECLAIM_PAGE);

                if (g_allocator_log_enabled) {
                    log_prefix();
                    uart_send_string("[Object] Free ");
                    log_hex_u64(ptr_addr);
                    uart_send_string(" at object size ");
                    uart_send_dec(cache->obj_size);
                    uart_send_string(". [Slab] Reclaim page ");
                    log_hex_u64(page_base);
                    uart_send_string("\n");
                }
                g_slab_reclaim_count++;
                p_free((void *)(uintptr_t)page_base);
            }
            return;
        }
        slab->state = SLAB_PARTIAL;
        if (g_allocator_log_enabled) {
            log_prefix();
            uart_send_string("[Object] Free ");
            log_hex_u64(ptr_addr);
            uart_send_string(" at object size ");
            uart_send_dec(cache->obj_size);
            uart_send_string("\n");
        }
        return;
    }

    if (kind == PAGE_KIND_LARGE) {
        int16_t tag = g_frame_array[page_idx].state;
        if ((ptr_addr & (PAGE_SIZE - 1U)) != 0U) {
            log_prefix();
            uart_send_string("[Error] kfree large ptr not page-aligned: ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }
        if (!is_alloc_head_tag(tag)) {
            log_prefix();
            uart_send_string("[Error] kfree large ptr is not allocation head: ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string(", page ");
            uart_send_dec((unsigned long)page_idx);
            uart_send_string("\n");
            return;
        }
        page_kind_set(page_idx, PAGE_KIND_NONE);

        if (g_allocator_log_enabled) {
            log_prefix();
            uart_send_string("[Page] Free ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
        }
        p_free((void *)(uintptr_t)page_base);
        return;
    }

    log_prefix();
    uart_send_string("[Error] kfree unknown page kind for ptr ");
    log_hex_u64(ptr_addr);
    uart_send_string("\n");
}

void memory_print_slabinfo(void) {
    unsigned int i;

    uart_send_string("Slab caches:\n");
    uart_send_string("class size stride partial full empty\n");
    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        uart_send_dec(i);
        uart_send_string(" ");
        uart_send_dec(g_kmem_caches[i].obj_size);
        uart_send_string(" ");
        uart_send_dec(g_kmem_caches[i].obj_stride);
        uart_send_string(" ");
        uart_send_dec(slab_list_count(g_kmem_caches[i].slabs_partial));
        uart_send_string(" ");
        uart_send_dec(slab_list_count(g_kmem_caches[i].slabs_full));
        uart_send_string(" ");
        uart_send_dec(slab_list_count(g_kmem_caches[i].slabs_empty));
        uart_send_string("\n");
    }
}

void memory_print_buddyinfo(void) {
    unsigned int order;
    uint64_t total_free_pages = 0;

    uart_send_string("Buddy free lists:\n");
    uart_send_string("order blocks pages\n");
    for (order = 0; order <= MEMORY_BUDDY_MAX_ORDER; order++) {
        unsigned int blocks = freelist_count_order(order);
        uint64_t pages = (uint64_t)blocks << order;

        if (blocks == 0) {
            continue;
        }
        total_free_pages += pages;
        uart_send_dec(order);
        uart_send_string(" ");
        uart_send_dec(blocks);
        uart_send_string(" ");
        uart_send_dec((unsigned long)pages);
        uart_send_string("\n");
    }
    uart_send_string("total_free_pages: ");
    uart_send_dec((unsigned long)total_free_pages);
    uart_send_string("\n");
}

int memory_check_slabs_ok(void) {
    unsigned int i;
    int ok = 1;

    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        if (!slab_check_list_invariants(i, "partial", g_kmem_caches[i].slabs_partial, SLAB_PARTIAL)) {
            ok = 0;
        }
        if (!slab_check_list_invariants(i, "full", g_kmem_caches[i].slabs_full, SLAB_FULL)) {
            ok = 0;
        }
        if (!slab_check_list_invariants(i, "empty", g_kmem_caches[i].slabs_empty, SLAB_EMPTY)) {
            ok = 0;
        }
    }

    return ok;
}

void memory_get_stats(struct memory_stats_snapshot *out) {
    if (out == 0) {
        return;
    }

    out->page_allocs = g_page_alloc_count;
    out->page_frees = g_page_free_count;
    out->object_allocs = g_object_alloc_count;
    out->object_frees = g_object_free_count;
    out->slab_reclaims = g_slab_reclaim_count;
    out->total_pages = g_total_pages;
    out->free_pages = total_free_pages_count();
    out->empty_slab_limit = KMEM_CACHE_EMPTY_LIMIT;
}

int memory_get_slab_class_snapshot(unsigned int class_idx, struct memory_slab_class_snapshot *out) {
    struct kmem_cache *cache;

    if (out == 0 || class_idx >= KMALLOC_CLASS_COUNT) {
        return -1;
    }

    cache = &g_kmem_caches[class_idx];
    out->class_idx = class_idx;
    out->obj_size = cache->obj_size;
    out->obj_stride = cache->obj_stride;
    out->partial_count = slab_list_count(cache->slabs_partial);
    out->full_count = slab_list_count(cache->slabs_full);
    out->empty_count = slab_list_count(cache->slabs_empty);
    out->cached_empty_count = cache->empty_count;
    return 0;
}

int memory_class_for_size(unsigned long size) {
    return size_to_class(size);
}

void memory_set_allocator_log_enabled(int enabled)
{
    g_allocator_log_enabled = (enabled != 0);
}

void memory_print_memstat(void) {
    uart_send_string("Allocator stats:\n");
    uart_send_string("page_allocs: ");
    uart_send_dec((unsigned long)g_page_alloc_count);
    uart_send_string("\n");
    uart_send_string("page_frees: ");
    uart_send_dec((unsigned long)g_page_free_count);
    uart_send_string("\n");
    uart_send_string("object_allocs: ");
    uart_send_dec((unsigned long)g_object_alloc_count);
    uart_send_string("\n");
    uart_send_string("object_frees: ");
    uart_send_dec((unsigned long)g_object_free_count);
    uart_send_string("\n");
    uart_send_string("slab_reclaims: ");
    uart_send_dec((unsigned long)g_slab_reclaim_count);
    uart_send_string("\n");
    uart_send_string("total_pages: ");
    uart_send_dec((unsigned long)g_total_pages);
    uart_send_string("\n");
    uart_send_string("empty_slab_limit: ");
    uart_send_dec((unsigned long)KMEM_CACHE_EMPTY_LIMIT);
    uart_send_string("\n");
    uart_send_string("poison_free_object: 0x");
    uart_send_hex((unsigned long)POISON_FREE_OBJECT);
    uart_send_string("\n");
    uart_send_string("poison_reclaim_page: 0x");
    uart_send_hex((unsigned long)POISON_RECLAIM_PAGE);
    uart_send_string("\n");
}

void memory_debug_check_slabs(void) {
    if (memory_check_slabs_ok()) {
        uart_send_string("slabcheck: OK\n");
    } else {
        uart_send_string("slabcheck: FAILED\n");
    }
}

void memory_init(const void *fdt, uint64_t initrd_start_hint, uint64_t initrd_end_hint) {
    struct fdt_mem_region dt_regions[FDT_MAX_MEM_REGIONS];
    struct fdt_mem_region dt_reserved[FDT_MAX_MEM_REGIONS];
    int dt_count;
    int reserved_count;
    int i;
    uint64_t total_pages = 0;
    uint64_t dtb_addr;
    uint64_t dtb_size;
    uint64_t k_start;
    uint64_t k_size;
    uint64_t initrd_start = 0;
    uint64_t initrd_end = 0;

    if (g_memory_ready) {
        return;
    }

    g_allocator_log_enabled = 1;

    g_regions_direct_map = 0;
    g_direct_map_base = 0;
    g_direct_map_first_idx = 0;
    g_direct_map_last_idx = 0;

    dt_count = fdt_get_memory_regions(fdt, dt_regions, FDT_MAX_MEM_REGIONS);
    if (dt_count <= 0) {
        log_prefix();
        uart_send_string("no memory regions from FDT\n");
        return;
    }

    g_region_count = 0;
    for (i = 0; i < dt_count && g_region_count < FDT_MAX_MEM_REGIONS; i++) {
        uint64_t start = align_up_u64(dt_regions[i].base, PAGE_SIZE);
        uint64_t raw_end = dt_regions[i].base + dt_regions[i].size;
        uint64_t end;
        uint64_t pages;

        if (raw_end < dt_regions[i].base) {
            raw_end = (uint64_t)-1;
        }
        end = align_down_u64(raw_end, PAGE_SIZE);

        if (end <= start) {
            continue;
        }
        pages = (end - start) / PAGE_SIZE;
        if (pages == 0) {
            continue;
        }

        g_regions[g_region_count].base = start;
        g_regions[g_region_count].size = end - start;
        g_regions[g_region_count].first_idx = total_pages;
        g_regions[g_region_count].page_count = pages;
        total_pages += pages;
        g_region_count++;
    }

    if (g_region_count == 0 || total_pages == 0) {
        log_prefix();
        uart_send_string("no usable page-aligned memory\n");
        return;
    }

    if (g_region_count == 1) {
        g_regions_direct_map = 1;
        g_direct_map_base = g_regions[0].base;
        g_direct_map_first_idx = g_regions[0].first_idx;
        g_direct_map_last_idx = g_regions[0].first_idx + g_regions[0].page_count;
    }

    g_total_pages = total_pages;
    startup_allocator_init(g_regions[0].base, g_regions[0].size);

    memory_reserve(0, PAGE_SIZE);

    dtb_addr = (uint64_t)(uintptr_t)fdt;
    dtb_size = (uint64_t)fdt_totalsize(fdt);
    if (dtb_size != 0) {
        memory_reserve(dtb_addr, dtb_size);
    }

    k_start = (uint64_t)(uintptr_t)&_phys_start;
    k_size = (uint64_t)((uintptr_t)&_phys_end - (uintptr_t)&_phys_start);
    memory_reserve(k_start, k_size);

    if (fdt_get_initrd_range(fdt, &initrd_start, &initrd_end) != 0 || initrd_end <= initrd_start) {
        initrd_start = 0;
        initrd_end = 0;
    }
    if ((initrd_start == 0 || initrd_end <= initrd_start) &&
        initrd_hint_valid(initrd_start_hint, initrd_end_hint)) {
        initrd_start = initrd_start_hint;
        initrd_end = initrd_end_hint;
    }
    if (initrd_end > initrd_start) {
        memory_reserve(initrd_start, initrd_end - initrd_start);
    }

    reserved_count = fdt_get_reserved_memory_regions(fdt, dt_reserved, FDT_MAX_MEM_REGIONS);
    if (reserved_count > 0) {
        for (i = 0; i < reserved_count; i++) {
            memory_reserve(dt_reserved[i].base, dt_reserved[i].size);
        }
    }

    if (buddy_setup_metadata() != 0) {
        log_prefix();
        uart_send_string("metadata allocation failed\n");
        return;
    }

    for (i = 0; i < (int)KMALLOC_CLASS_COUNT; i++) {
        g_kmem_caches[i].class_idx = (unsigned int)i;
        g_kmem_caches[i].obj_size = class_to_size((unsigned int)i);
        g_kmem_caches[i].obj_align = g_kmem_caches[i].obj_size;
        g_kmem_caches[i].obj_stride =
            slab_calc_stride(g_kmem_caches[i].obj_size, g_kmem_caches[i].obj_align);
        g_kmem_caches[i].empty_count = 0;
        g_kmem_caches[i].slabs_partial = 0;
        g_kmem_caches[i].slabs_full = 0;
        g_kmem_caches[i].slabs_empty = 0;
    }

    buddy_build_initial_state();
    g_memory_ready = 1;

    log_prefix();
    uart_send_string("memory init done regions=");
    uart_send_dec(g_region_count);
    uart_send_string(" pages=");
    uart_send_dec((unsigned long)g_total_pages);
    uart_send_string("\n");

    /* Keep allocator logs for boot-time bring-up only. Runtime allocations from
     * runu/fork/thread demos are too noisy; memory tests re-enable them
     * explicitly around allocator validation.
     */
    g_allocator_log_enabled = 0;
}
