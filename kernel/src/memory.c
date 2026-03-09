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
// malloc small and large
#define KMALLOC_TAG_SMALL 0x4D53U
#define KMALLOC_TAG_LARGE 0x4D4CU
// TODO
// kmalloc_header remove
// Slab Reclamation
// Thread-Safe / Interrupt-Safe

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
// TODO Modify: store what's the byte
struct kmalloc_header {
    uint16_t tag;
    uint16_t class_idx;
    uint32_t pages;
};
// Intrusive linked list
struct kmalloc_free_chunk {
    struct kmalloc_free_chunk *next;
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
// TODO listhead version
static int16_t *g_frame_state = 0; // VAL/F/X
static int32_t *g_free_next = 0;
static int32_t g_free_lists[MEMORY_BUDDY_MAX_ORDER + 1];
// is buddy +kmalloc  ok
static int g_memory_ready = 0;

// slab allocate thing
static const uint16_t g_kmalloc_classes[] = { 32U, 64U, 128U, 256U, 512U, 1024U, 2048U };
#define KMALLOC_CLASS_COUNT ((unsigned int)(sizeof(g_kmalloc_classes) / sizeof(g_kmalloc_classes[0]))) // avoid hardcore 7
static struct kmalloc_free_chunk *g_kmalloc_freelist[KMALLOC_CLASS_COUNT];
// Linker
extern char _phys_start;
extern char _phys_end;
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
    uart_send_string("reserve start=");
    log_hex_u64(rs);
    uart_send_string(" size=");
    log_hex_u64(re - rs);
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
    if (find_region_by_index(idx, &rid) != 0) {
        return -1;
    }
    *pa_out = g_regions[rid].base + (idx - g_regions[rid].first_idx) * PAGE_SIZE;
    return 0;
}
// user give back
static int pa_to_index(uint64_t pa, uint64_t *idx_out, unsigned int *rid_out) {
    unsigned int rid;
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
    g_frame_state[idx] = (int16_t)order;
    for (i = 1; i < pages; i++) {
        g_frame_state[idx + i] = FRAME_BUDDY;
    }
}

// mark positive num or -1
static void block_mark_alloc(uint64_t idx, unsigned int order) {
    uint64_t pages = 1ULL << order;
    uint64_t i;
    g_frame_state[idx] = alloc_tag_from_order(order);
    for (i = 1; i < pages; i++) {
        g_frame_state[idx + i] = FRAME_USED;
    }
}
// mark -2 or -10X
static void freelist_push(unsigned int order, uint64_t idx) {
    int32_t i32 = (int32_t)idx;
    g_free_next[idx] = g_free_lists[order];
    g_free_lists[order] = i32;
}

/////////////////////////////////////////////////////* FREE LIST*///////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////
static int32_t freelist_pop(unsigned int order) {
    int32_t head = g_free_lists[order];
    if (head >= 0) {
        g_free_lists[order] = g_free_next[(uint64_t)head];
        g_free_next[(uint64_t)head] = -1;
    }
    return head;
}

// this is for merge, remove specific page: need traverse
static int freelist_remove(unsigned int order, uint64_t idx) {
    int32_t prev = -1;
    int32_t cur = g_free_lists[order];

    while (cur >= 0) {
        if ((uint64_t)cur == idx) {
            int32_t next = g_free_next[(uint64_t)cur];
            if (prev < 0) {
                g_free_lists[order] = next;
            } else {
                g_free_next[(uint64_t)prev] = next;
            }
            g_free_next[idx] = -1;
            return 0;
        }
        prev = cur;
        cur = g_free_next[(uint64_t)cur];
    }
    return -1;
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
    // Frame array size : log page
    uint64_t state_bytes = g_total_pages * sizeof(int16_t);
    // Freelist size : page index
    uint64_t next_bytes = g_total_pages * sizeof(int32_t);
    uint64_t i;

    g_frame_state = (int16_t *)startup_alloc(state_bytes, 8U);
    if (g_frame_state == 0) {
        return -1;
    }
    g_free_next = (int32_t *)startup_alloc(next_bytes, 8U);
    if (g_free_next == 0) {
        return -1;
    }
    // Default for safety
    for (i = 0; i < g_total_pages; i++) {
        g_frame_state[i] = FRAME_USED;
        g_free_next[i] = -1;
    }
    // Default
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
                g_frame_state[idx] = FRAME_USED;
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

        if (index_to_pa(buddy_idx, &buddy_pa) == 0) {
            log_prefix();
            uart_send_string("split release order=");
            uart_send_dec(cur_order);
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

    log_prefix();
    uart_send_string("p_alloc pages=");
    uart_send_dec(pages);
    uart_send_string(" order=");
    uart_send_dec(need_order);
    uart_send_string(" addr=");
    log_hex_u64(pa);
    uart_send_string("\n");

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

    tag = g_frame_state[idx];
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
        if (g_frame_state[buddy] != (int16_t)merged_order) {
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

    log_prefix();
    uart_send_string("p_free addr=");
    log_hex_u64(pa);
    uart_send_string(" order=");
    uart_send_dec(order);
    uart_send_string(" merged_order=");
    uart_send_dec(merged_order);
    uart_send_string("\n");
}

static int kmalloc_find_class(unsigned long size) {
    unsigned long need = size + sizeof(struct kmalloc_header);
    unsigned int i;
    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        if (need <= (unsigned long)g_kmalloc_classes[i]) {
            return (int)i;
        }
    }
    return -1;
}

static int kmalloc_refill_class(unsigned int class_idx) {
    void *page = p_alloc(1U);
    uint64_t base;
    uint64_t chunks;
    uint64_t i;
    uint64_t chunk_size;

    if (page == 0) {
        return -1;
    }

    base = (uint64_t)(uintptr_t)page;
    chunk_size = (uint64_t)g_kmalloc_classes[class_idx];
    chunks = PAGE_SIZE / chunk_size;
    if (chunks == 0) {
        return -1;
    }

    for (i = 0; i < chunks; i++) {
        uint64_t caddr = base + i * chunk_size;
        struct kmalloc_free_chunk *chunk = (struct kmalloc_free_chunk *)(uintptr_t)caddr;
        chunk->next = g_kmalloc_freelist[class_idx];
        g_kmalloc_freelist[class_idx] = chunk;
    }
    return 0;
}

void *kmalloc(unsigned long size) {
    int cls;
    struct kmalloc_header *hdr;
    void *ret;

    if (!g_memory_ready || size == 0UL) {
        return 0;
    }

    cls = kmalloc_find_class(size);
    if (cls >= 0) {
        struct kmalloc_free_chunk *chunk;

        if (g_kmalloc_freelist[(unsigned int)cls] == 0) {
            if (kmalloc_refill_class((unsigned int)cls) != 0) {
                return 0;
            }
        }

        chunk = g_kmalloc_freelist[(unsigned int)cls];
        g_kmalloc_freelist[(unsigned int)cls] = chunk->next;

        hdr = (struct kmalloc_header *)chunk;
        hdr->tag = KMALLOC_TAG_SMALL;
        hdr->class_idx = (uint16_t)cls;
        hdr->pages = 0U;
        ret = (void *)((uint8_t *)hdr + sizeof(struct kmalloc_header));

        log_prefix();
        uart_send_string("kmalloc size=");
        uart_send_dec(size);
        uart_send_string(" class=");
        uart_send_dec((unsigned long)cls);
        uart_send_string(" addr=");
        log_hex_u64((uint64_t)(uintptr_t)ret);
        uart_send_string("\n");

        return ret;
    }

    {
        unsigned long total = size + sizeof(struct kmalloc_header);
        unsigned long pages = (total + PAGE_SIZE - 1UL) / PAGE_SIZE;
        void *base = p_alloc((unsigned int)pages);
        if (base == 0) {
            return 0;
        }

        hdr = (struct kmalloc_header *)base;
        hdr->tag = KMALLOC_TAG_LARGE;
        hdr->class_idx = 0xFFFFU;
        hdr->pages = (uint32_t)pages;
        ret = (void *)((uint8_t *)hdr + sizeof(struct kmalloc_header));

        log_prefix();
        uart_send_string("kmalloc size=");
        uart_send_dec(size);
        uart_send_string(" pages=");
        uart_send_dec(pages);
        uart_send_string(" addr=");
        log_hex_u64((uint64_t)(uintptr_t)ret);
        uart_send_string("\n");

        return ret;
    }
}

void kfree(void *ptr) {
    struct kmalloc_header *hdr;

    if (!g_memory_ready || ptr == 0) {
        return;
    }

    hdr = (struct kmalloc_header *)((uint8_t *)ptr - sizeof(struct kmalloc_header));
    if (hdr->tag == KMALLOC_TAG_SMALL) {
        unsigned int cls = (unsigned int)hdr->class_idx;
        struct kmalloc_free_chunk *chunk;
        if (cls >= KMALLOC_CLASS_COUNT) {
            return;
        }
        chunk = (struct kmalloc_free_chunk *)hdr;
        chunk->next = g_kmalloc_freelist[cls];
        g_kmalloc_freelist[cls] = chunk;

        log_prefix();
        uart_send_string("kfree small addr=");
        log_hex_u64((uint64_t)(uintptr_t)ptr);
        uart_send_string("\n");
        return;
    }

    if (hdr->tag == KMALLOC_TAG_LARGE) {
        log_prefix();
        uart_send_string("kfree large addr=");
        log_hex_u64((uint64_t)(uintptr_t)ptr);
        uart_send_string(" pages=");
        uart_send_dec((unsigned long)hdr->pages);
        uart_send_string("\n");
        p_free((void *)hdr);
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
        g_kmalloc_freelist[i] = 0;
    }

    buddy_build_initial_state();
    g_memory_ready = 1;

    log_prefix();
    uart_send_string("memory init done regions=");
    uart_send_dec(g_region_count);
    uart_send_string(" pages=");
    uart_send_dec((unsigned long)g_total_pages);
    uart_send_string("\n");
}
