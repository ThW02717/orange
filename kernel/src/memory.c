/*
 * Physical pages: memory_init() discovers RAM, reserves live ranges and builds
 * buddy free lists. p_alloc()/p_free() split and merge power-of-two blocks.
 * Byte-sized allocations and slab lists are in slab.c.
 */
#include "memory_internal.h"
#include "user_layout.h"
#include "irqflags.h"
#include <stdint.h>
#include "memory.h"
#include "fdt.h"
#include "uart.h"
#include "trap.h"

#define MEMORY_MAX_RESERVES 64
/*
 * frame_meta.state encoding.
 * Free block head: non-negative order.
 * Free block body: FRAME_BUDDY.
 * Allocated block body: FRAME_USED.
 * Allocated block head: FRAME_ALLOC_BASE - order.
 */
#define FRAME_BUDDY ((int16_t)-1)
#define FRAME_USED  ((int16_t)-2)
#define FRAME_ALLOC_BASE ((int16_t)-100)

/* One page-aligned RAM range discovered from the devicetree. */
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

struct frame_meta {
    /* Block-head order or one of the state tags above. */
    int16_t state;
    /* Only free block heads use these links. */
    int32_t free_prev;
    int32_t free_next;
    /*
     * Upper-layer owner tag for allocated pages, used by kfree() to dispatch
     * either back to slab object free or to the large-allocation page path.
     */
    enum page_kind kind;
};

/* Physical memory description and reserved ranges. */
static struct mem_region_info regions[FDT_MAX_MEM_REGIONS];
static unsigned int region_count = 0;
static uint64_t managed_page_count = 0;
static struct reserve_range reserves[MEMORY_MAX_RESERVES];
static unsigned int reserve_count = 0;

/* Early bump allocator used before buddy metadata has been allocated. */
static uint64_t startup_cur = 0;
static uint64_t startup_end = 0;
static int startup_ready = 0;

/* Buddy allocator state: one frame_meta per page plus one free list per order. */
static struct frame_meta *frame_array = 0;
static int32_t free_lists[MEMORY_BUDDY_MAX_ORDER + 1];
static int regions_direct_map = 0;
static uint64_t direct_map_base = 0;
static uint64_t direct_map_first_idx = 0;
static uint64_t direct_map_last_idx = 0;

/* Runtime counters and debug flags shared by buddy and slab paths. */
static int memory_ready = 0;
static uint64_t page_alloc_count = 0;
static uint64_t page_free_count = 0;
static int allocator_log_enabled = 0;

/* Linker-provided physical range occupied by the kernel image. */
extern char _phys_start;
extern char _phys_end;

/* Align down/up. Alignment values used here are powers of two. */
static uint64_t align_down_u64(uint64_t v, uint64_t a)
{
    return v & ~(a - 1U);
}

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    return (v + (a - 1U)) & ~(a - 1U);
}

/* Half-open range overlap check: [a_start, a_end) intersects [b_start, b_end). */
static int ranges_overlap(uint64_t a_start, uint64_t a_end, uint64_t b_start, uint64_t b_end)
{
    return a_start < b_end && b_start < a_end;
}

/* UART logging helpers kept tiny so they work in the freestanding kernel. */
static void log_prefix(void)
{
    uart_send_string("[Allocator] ");
}

static void log_hex_u64(uint64_t value)
{
    uart_send_string("0x");
    uart_send_hex((unsigned long)value);
}

static void log_page_range(uint64_t start_idx, unsigned int order)
{
    uint64_t end_idx = start_idx + (1ULL << order) - 1ULL;

    uart_send_string("[");
    uart_send_dec((unsigned long)start_idx);
    uart_send_string(", ");
    uart_send_dec((unsigned long)end_idx);
    uart_send_string("]");
}

/* Count free buddy blocks of one order for memstat output. */
static unsigned int freelist_count_order(unsigned int order)
{
    unsigned int n = 0;
    int32_t cur = free_lists[order];

    while (cur >= 0) {
        n++;
        cur = frame_array[(uint64_t)cur].free_next;
    }
    return n;
}

/* Sum every buddy free list as pages, not blocks. */
static uint64_t total_free_pages_count(void)
{
    unsigned int order;
    uint64_t total = 0;

    for (order = 0; order <= MEMORY_BUDDY_MAX_ORDER; order++) {
        total += (uint64_t)freelist_count_order(order) << order;
    }
    return total;
}

/*
 * Encode/decode the allocated block order into frame_meta.state.
 * This lets p_free(ptr) recover the original buddy order from the head page.
 */
static int16_t alloc_tag_from_order(unsigned int order)
{
    return (int16_t)(FRAME_ALLOC_BASE - (int16_t)order);
}

static int is_alloc_head_tag(int16_t tag)
{
    return tag <= FRAME_ALLOC_BASE;
}

static unsigned int order_from_alloc_tag(int16_t tag)
{
    return (unsigned int)(FRAME_ALLOC_BASE - tag);
}

static int pa_to_index(uint64_t pa, uint64_t *idx_out, unsigned int *rid_out);

/*
 * Reserve a physical address range so the buddy allocator will never hand it
 * out. The range is rounded to full pages because the allocator manages memory
 * at PAGE_SIZE granularity.
 */
void memory_reserve(uint64_t start, uint64_t size)
{
    uint64_t end;
    uint64_t rs;
    uint64_t re;
    if (size == 0) {
        return;
    }

    end = start + size;
    if (end < start) {
        end = (uint64_t)-1;
    }

    rs = align_down_u64(start, PAGE_SIZE);
    if (end > ((uint64_t)-1 - (PAGE_SIZE - 1U))) {
        re = (uint64_t)-1;
    } else {
        re = align_up_u64(end, PAGE_SIZE);
    }
    if (re <= rs) {
        return;
    }

    if (reserve_count >= MEMORY_MAX_RESERVES) {
        log_prefix();
        uart_send_string("reserve table full\n");
        return;
    }
    reserves[reserve_count].start = rs;
    reserves[reserve_count].end = re;
    reserve_count++;

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
/* Return whether a physical address falls inside any reserved range. */
static int is_reserved_pa(uint64_t pa)
{
    unsigned int i;
    for (i = 0; i < reserve_count; i++) {
        if (pa >= reserves[i].start && pa < reserves[i].end) {
            return 1;
        }
    }
    return 0;
}
/* Prepare and validate the early allocation range before the buddy allocator starts. */
static void startup_allocator_init(uint64_t base, uint64_t size)
{
    uint64_t end = base + size;
    if (end < base) {
        end = (uint64_t)-1;
    }
    startup_cur = base;
    startup_end = end;
    if (startup_cur < PAGE_SIZE) {
        startup_cur = PAGE_SIZE;
    }
    startup_ready = (startup_end > startup_cur) ? 1 : 0;
}
/*
 * Early bump allocator that skips reserved ranges and then reserves its result.
 * Used while building the buddy allocator itself, before p_alloc() is ready.
 */
static void *startup_alloc(uint64_t size, uint64_t align)
{
    uint64_t p;
    uint64_t end;
    unsigned int i;
    int moved;
    if (!startup_ready || size == 0) {
        return 0;
    }
    if (align == 0) {
        align = 16U;
    }
    if ((align & (align - 1U)) != 0U) {
        return 0;
    }
    if (startup_cur > UINT64_MAX - (align - 1U)) {
        return 0;
    }
    p = align_up_u64(startup_cur, align);
    for (;;) {
        moved = 0;
        end = p + size;
        if (end < p) {
            return 0;
        }
        for (i = 0; i < reserve_count; i++) {
            if (ranges_overlap(p, end, reserves[i].start, reserves[i].end)) {
                if (reserves[i].end > UINT64_MAX - (align - 1U)) {
                    return 0;
                }
                p = align_up_u64(reserves[i].end, align);
                moved = 1;
                break;
            }
        }
        if (!moved) {
            break;
        }
    }

    end = p + size;
    if (end < p || end > startup_end) {
        return 0;
    }

    startup_cur = end;
    memory_reserve(p, size);
    return (void *)(uintptr_t)p;
}

/* Locate the RAM region that owns a global page index. */
static int find_region_by_index(uint64_t idx, unsigned int *rid_out)
{
    unsigned int rid;

    if (regions_direct_map) {
        if (idx >= direct_map_first_idx && idx < direct_map_last_idx) {
            if (rid_out != 0) {
                *rid_out = 0;
            }
            return 0;
        }
        return -1;
    }

    for (rid = 0; rid < region_count; rid++) {
        uint64_t first = regions[rid].first_idx;
        uint64_t last = first + regions[rid].page_count;
        if (idx >= first && idx < last) {
            if (rid_out != 0) {
                *rid_out = rid;
            }
            return 0;
        }
    }
    return -1;
}
/* Convert a global page index into its physical address. */
static int index_to_pa(uint64_t idx, uint64_t *pa_out)
{
    unsigned int rid;

    if (pa_out == 0) {
        return -1;
    }

    if (regions_direct_map) {
        if (idx < direct_map_first_idx || idx >= direct_map_last_idx) {
            return -1;
        }
        *pa_out = direct_map_base + (idx - direct_map_first_idx) * PAGE_SIZE;
        return 0;
    }

    if (find_region_by_index(idx, &rid) != 0) {
        return -1;
    }
    *pa_out = regions[rid].base + (idx - regions[rid].first_idx) * PAGE_SIZE;
    return 0;
}
/* Convert a page-aligned physical address back into a global page index. */
static int pa_to_index(uint64_t pa, uint64_t *idx_out, unsigned int *rid_out)
{
    unsigned int rid;

    if (idx_out == 0) {
        return -1;
    }

    if (regions_direct_map) {
        uint64_t end = direct_map_base + (direct_map_last_idx - direct_map_first_idx) * PAGE_SIZE;

        if (pa < direct_map_base || pa >= end || (pa & (PAGE_SIZE - 1U)) != 0U) {
            return -1;
        }
        *idx_out = direct_map_first_idx + ((pa - direct_map_base) / PAGE_SIZE);
        if (rid_out != 0) {
            *rid_out = 0;
        }
        return 0;
    }

    for (rid = 0; rid < region_count; rid++) {
        uint64_t base = regions[rid].base;
        uint64_t end = base + regions[rid].size;
        if (pa >= base && pa < end) {
            uint64_t off = pa - base;
            uint64_t page = off / PAGE_SIZE;
            if ((off % PAGE_SIZE) != 0U || page >= regions[rid].page_count) {
                return -1;
            }
            *idx_out = regions[rid].first_idx + page;
            if (rid_out != 0) {
                *rid_out = rid;
            }
            return 0;
        }
    }
    return -1;
}
/* Buddy frame metadata and free-list operations. */

/*
 * Buddy frame-state helper: mark a block as free.
 * The head page stores the order; following pages are marked FRAME_BUDDY so
 * they are known to belong to the same free block but are not list heads.
 */
static void block_mark_free(uint64_t idx, unsigned int order)
{
    uint64_t pages = 1ULL << order;
    uint64_t i;
    frame_array[idx].state = (int16_t)order;
    for (i = 1; i < pages; i++) {
        frame_array[idx + i].state = FRAME_BUDDY;
    }
}

/*
 * Buddy frame-state helper: mark a block as allocated.
 * The head page stores an allocated-order tag; following pages are FRAME_USED.
 */
static void block_mark_alloc(uint64_t idx, unsigned int order)
{
    uint64_t pages = 1ULL << order;
    uint64_t i;
    frame_array[idx].state = alloc_tag_from_order(order);
    for (i = 1; i < pages; i++) {
        frame_array[idx + i].state = FRAME_USED;
    }
}
/* Link free block heads by index. prev allows O(1) unlink during buddy merging. */
static void freelist_push(unsigned int order, uint64_t idx)
{
    int32_t i32 = (int32_t)idx;
    int32_t old_head = free_lists[order];

    frame_array[idx].free_prev = -1;
    frame_array[idx].free_next = old_head;
    if (old_head >= 0) {
        frame_array[(uint64_t)old_head].free_prev = i32;
    }
    free_lists[order] = i32;

    if (allocator_log_enabled) {
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

/* Pop the first free block head from free_lists[order]. */
static int32_t freelist_pop(unsigned int order)
{
    int32_t head = free_lists[order];
    if (head >= 0) {
        int32_t next = frame_array[(uint64_t)head].free_next;

        free_lists[order] = next;
        if (next >= 0) {
            frame_array[(uint64_t)next].free_prev = -1;
        }
        frame_array[(uint64_t)head].free_prev = -1;
        frame_array[(uint64_t)head].free_next = -1;

        if (allocator_log_enabled) {
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

/*
 * Remove a known free block head from free_lists[order] in O(1).
 * This is what buddy merging needs after it discovers that the adjacent buddy
 * block is also free. A singly linked list would need a scan to find prev.
 */
static int freelist_remove(unsigned int order, uint64_t idx)
{
    int32_t prev;
    int32_t next;

    if (idx >= managed_page_count) {
        return -1;
    }
    if (frame_array[idx].state != (int16_t)order) {
        return -1;
    }

    prev = frame_array[idx].free_prev;
    next = frame_array[idx].free_next;
    if (prev < 0) {
        if (free_lists[order] != (int32_t)idx) {
            return -1;
        }
        free_lists[order] = next;
    } else {
        frame_array[(uint64_t)prev].free_next = next;
    }
    if (next >= 0) {
        frame_array[(uint64_t)next].free_prev = prev;
    }
    frame_array[idx].free_prev = -1;
    frame_array[idx].free_next = -1;

    if (allocator_log_enabled) {
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
/*
 * Compute the adjacent buddy block for idx at the same order.
 * The XOR flips the bit representing this block size in the region-local page
 * index. Example: order 3 means 8-page blocks; local index 0 buddies with 8,
 * and local index 8 buddies with 0. The lookup stays inside one RAM region.
 */
static int buddy_index(uint64_t idx, unsigned int order, uint64_t *out)
{
    unsigned int rid;
    uint64_t local_idx;
    uint64_t buddy_local;
    uint64_t size = 1ULL << order;

    if (find_region_by_index(idx, &rid) != 0) {
        return -1;
    }

    local_idx = idx - regions[rid].first_idx;
    buddy_local = local_idx ^ size;
    if (buddy_local >= regions[rid].page_count) {
        return -1;
    }

    *out = regions[rid].first_idx + buddy_local;
    if (allocator_log_enabled) {
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
/* Return floor(log2(v)). Used to choose the largest block size for a run. */
static unsigned int floor_log2_u64(uint64_t v)
{
    unsigned int n = 0;
    while ((1ULL << (n + 1U)) <= v) {
        n++;
    }
    return n;
}
/*
 * Split one contiguous free run into aligned buddy blocks.
 * Buddy blocks must be aligned to their own size, so the function repeatedly
 * chooses the largest order that both fits in run_pages and starts at a valid
 * 2^order boundary.
 */
static void add_free_run(unsigned int rid, uint64_t run_start_page, uint64_t run_pages)
{
    while (run_pages > 0) {
        unsigned int order = floor_log2_u64(run_pages);
        uint64_t idx;

        if (order > MEMORY_BUDDY_MAX_ORDER) {
            order = MEMORY_BUDDY_MAX_ORDER;
        }
        /*
         * If run_start_page is not aligned for this order, reduce the order
         * until the block can be represented as a valid buddy block.
         */
        while (order > 0 && (run_start_page & ((1ULL << order) - 1ULL)) != 0ULL) {
            order--;
        }

        idx = regions[rid].first_idx + run_start_page;
        block_mark_free(idx, order);
        freelist_push(order, idx);

        run_start_page += (1ULL << order);
        run_pages -= (1ULL << order);
    }
}

/* Allocate and initialize allocator metadata using the early bump allocator. */
static int buddy_setup_metadata(void)
{
    uint64_t frame_array_bytes = managed_page_count * sizeof(struct frame_meta);
    uint64_t i;

    frame_array = (struct frame_meta *)startup_alloc(frame_array_bytes, PAGE_SIZE);
    if (frame_array == 0) {
        return -1;
    }

    for (i = 0; i < managed_page_count; i++) {
        frame_array[i].state = FRAME_USED;
        frame_array[i].free_prev = -1;
        frame_array[i].free_next = -1;
        frame_array[i].kind = PAGE_KIND_NONE;
    }
    for (i = 0; i <= MEMORY_BUDDY_MAX_ORDER; i++) {
        free_lists[i] = -1;
    }
    return 0;
}
/* Build the initial buddy free lists from devicetree RAM minus reservations. */
static void buddy_build_initial_state(void)
{
    unsigned int rid;
    for (rid = 0; rid < region_count; rid++) {
        uint64_t p;
        uint64_t run_start = 0;
        uint64_t run_len = 0;

        for (p = 0; p < regions[rid].page_count; p++) {
            uint64_t idx = regions[rid].first_idx + p;
            uint64_t pa = regions[rid].base + p * PAGE_SIZE;
            if (is_reserved_pa(pa)) {
                if (run_len > 0) {
                    add_free_run(rid, run_start, run_len);
                    run_len = 0;
                }
                frame_array[idx].state = FRAME_USED;
                continue;
            }

            if (run_len == 0) {
                run_start = p;
            }
            run_len++;
        }
        if (run_len > 0) {
            add_free_run(rid, run_start, run_len);
        }
    }
}

static void *p_alloc_locked(unsigned int pages)
{
    unsigned int need_order = 0;
    unsigned int cur_order;
    int32_t idx32;
    uint64_t idx;
    uint64_t pa = 0;

    if (!memory_ready || pages == 0) {
        return 0;
    }
    /* Round the requested page count up to a buddy order. */
    while ((1ULL << need_order) < (uint64_t)pages) {
        need_order++;
        if (need_order > MEMORY_BUDDY_MAX_ORDER) {
            return 0;
        }
    }
    /* Find the smallest available block that can satisfy the request. */
    cur_order = need_order;
    while (cur_order <= MEMORY_BUDDY_MAX_ORDER && free_lists[cur_order] < 0) {
        cur_order++;
    }
    if (cur_order > MEMORY_BUDDY_MAX_ORDER) {
        return 0;
    }
    idx32 = freelist_pop(cur_order);
    if (idx32 < 0) {
        return 0;
    }
    idx = (uint64_t)idx32;
    /*
     * Split larger blocks. The left half continues toward the allocation;
     * each right half becomes a free buddy block at the lower order.
     */
    while (cur_order > need_order) {
        uint64_t buddy_idx;
        uint64_t buddy_pa = 0;
        cur_order--;
        buddy_idx = idx + (1ULL << cur_order);
        block_mark_free(buddy_idx, cur_order);
        freelist_push(cur_order, buddy_idx);

        if (index_to_pa(buddy_idx, &buddy_pa) == 0 && allocator_log_enabled) {
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

    if (allocator_log_enabled) {
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
    page_alloc_count++;
    return (void *)(uintptr_t)pa;
}

static void p_free_locked(void *ptr)
{
    uint64_t pa;
    uint64_t idx;
    int16_t tag;
    unsigned int order;
    uint64_t merged_idx;
    unsigned int merged_order;
    /* p_free only accepts page-aligned pointers returned by p_alloc(). */
    if (!memory_ready || ptr == 0) {
        return;
    }

    pa = (uint64_t)(uintptr_t)ptr;
    if ((pa & (PAGE_SIZE - 1U)) != 0U) {
        return;
    }
    if (pa_to_index(pa, &idx, 0) != 0) {
        return;
    }

    tag = frame_array[idx].state;
    if (!is_alloc_head_tag(tag)) {
        return;
    }
    /* Recover the allocation size from the head page tag. */
    order = order_from_alloc_tag(tag);
    merged_idx = idx;
    merged_order = order;

    /* Merge upward while the adjacent buddy is free at the same order. */
    while (merged_order < MEMORY_BUDDY_MAX_ORDER) {
        uint64_t buddy;
        if (buddy_index(merged_idx, merged_order, &buddy) != 0) {
            break;
        }
        if (frame_array[buddy].state != (int16_t)merged_order) {
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

    if (allocator_log_enabled) {
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
        if (free_lists[merged_order] >= 0) {
            uint64_t next_pa = 0;
            if (index_to_pa((uint64_t)free_lists[merged_order], &next_pa) == 0) {
                log_hex_u64(next_pa);
            } else {
                uart_send_string("none");
            }
        } else {
            uart_send_string("none");
        }
        uart_send_string("\n");
    }
    page_free_count++;
}

void *p_alloc(unsigned int pages)
{
    uint64_t status = local_irq_save();
    void *ptr = p_alloc_locked(pages);
    local_irq_restore(status);
    return ptr;
}

void p_free(void *ptr)
{
    uint64_t status = local_irq_save();
    p_free_locked(ptr);
    local_irq_restore(status);
}

/* Shell/debug dump of buddy free lists by order. */
void memory_print_buddyinfo(void)
{
    unsigned int order;
    uint64_t total_free_pages = 0;
    unsigned int block_counts[MEMORY_BUDDY_MAX_ORDER + 1U];
    uint64_t status = local_irq_save();

    for (order = 0; order <= MEMORY_BUDDY_MAX_ORDER; order++) {
        block_counts[order] = freelist_count_order(order);
    }
    local_irq_restore(status);

    uart_send_string("Buddy free lists:\n");
    uart_send_string("order blocks pages\n");
    for (order = 0; order <= MEMORY_BUDDY_MAX_ORDER; order++) {
        unsigned int blocks = block_counts[order];
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

/* Copy allocator counters into a test-friendly snapshot struct. */
void memory_get_stats(struct memory_stats_snapshot *out)
{
    uint64_t status;
    if (out == 0) {
        return;
    }
    status = local_irq_save();

    out->page_allocs = page_alloc_count;
    out->page_frees = page_free_count;
    out->total_pages = managed_page_count;
    out->free_pages = total_free_pages_count();
    slab_get_stats(out);
    local_irq_restore(status);
}

/* Enable verbose allocator logging during bring-up/tests. */
void memory_set_allocator_log_enabled(int enabled)
{
    allocator_log_enabled = (enabled != 0);
}

/* Discover RAM, reserve occupied pages, then build buddy and slab state. */
void memory_init(const void *fdt, uint64_t initrd_start, uint64_t initrd_end)
{
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

    if (memory_ready) {
        return;
    }

    allocator_log_enabled = 1;

    regions_direct_map = 0;
    direct_map_base = 0;
    direct_map_first_idx = 0;
    direct_map_last_idx = 0;

    /* The devicetree is the source of truth for physical RAM on OrangePi RV2. */
    dt_count = fdt_get_memory_regions(fdt, dt_regions, FDT_MAX_MEM_REGIONS);
    if (dt_count <= 0) {
        log_prefix();
        uart_send_string("no memory regions from FDT\n");
        return;
    }

    region_count = 0;
    for (i = 0; i < dt_count && region_count < FDT_MAX_MEM_REGIONS; i++) {
        if (dt_regions[i].base > UINT64_MAX - (PAGE_SIZE - 1U)) {
            return;
        }
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
        if (pages > INT32_MAX - total_pages) {
            log_prefix(); uart_send_string("too many pages for frame indices\n"); return;
        }
        for (unsigned int r = 0; r < region_count; r++) {
            if (ranges_overlap(start, end, regions[r].base, regions[r].base + regions[r].size)) {
                log_prefix(); uart_send_string("overlapping RAM regions\n"); return;
            }
        }

        regions[region_count].base = start;
        regions[region_count].size = end - start;
        regions[region_count].first_idx = total_pages;
        regions[region_count].page_count = pages;
        total_pages += pages;
        region_count++;
    }

    if (region_count == 0 || total_pages == 0) {
        log_prefix();
        uart_send_string("no usable page-aligned memory\n");
        return;
    }

    /*
     * Common case: one contiguous RAM range. Direct mapping avoids scanning
     * regions for every page/address conversion.
     */
    if (region_count == 1) {
        regions_direct_map = 1;
        direct_map_base = regions[0].base;
        direct_map_first_idx = regions[0].first_idx;
        direct_map_last_idx = regions[0].first_idx + regions[0].page_count;
    }

    managed_page_count = total_pages;
    startup_allocator_init(regions[0].base, regions[0].size);

    /*
     * Never allocate physical page 0; a null/low address is too easy to misuse
     * and some firmware/platform code may treat it specially.
     */
    memory_reserve(0, PAGE_SIZE);

    /* The kernel still needs the DTB after allocator init, so reserve it. */
    dtb_addr = (uint64_t)(uintptr_t)fdt;
    dtb_size = (uint64_t)fdt_totalsize(fdt);
    if (dtb_size != 0) {
        memory_reserve(dtb_addr, dtb_size);
    }

    /* Do not let dynamic allocations overwrite the loaded kernel image. */
    k_start = (uint64_t)(uintptr_t)&_phys_start;
    k_size = (uint64_t)((uintptr_t)&_phys_end - (uintptr_t)&_phys_start);
    memory_reserve(k_start, k_size);

    /*
     * Raw user binaries execute here even though no page tables are active.
     * This window must not also be returned as buddy pages or allocator data.
     */
    memory_reserve(USER_CODE_BASE, USER_CODE_SIZE);

    /* Platform discovery already resolved the archive range. */
    if (initrd_end > initrd_start) {
        memory_reserve(initrd_start, initrd_end - initrd_start);
    }

    reserved_count = fdt_get_memreserve_regions(fdt, dt_reserved, FDT_MAX_MEM_REGIONS);
    if (reserved_count < 0) {
        log_prefix(); uart_send_string("invalid or oversized FDT reserve map\n"); return;
    }
    for (i = 0; i < reserved_count; i++) {
        memory_reserve(dt_reserved[i].base, dt_reserved[i].size);
    }

    /* Honor /reserved-memory nodes from the board DTB. */
    reserved_count = fdt_get_reserved_memory_regions(fdt, dt_reserved, FDT_MAX_MEM_REGIONS);
    if (reserved_count < 0) {
        log_prefix(); uart_send_string("invalid or oversized reserved-memory list\n"); return;
    }
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

    slab_init();

    buddy_build_initial_state();
    memory_ready = 1;

    log_prefix();
    uart_send_string("memory init done regions=");
    uart_send_dec(region_count);
    uart_send_string(" pages=");
    uart_send_dec((unsigned long)managed_page_count);
    uart_send_string("\n");

    /*
     * Keep allocator logs for boot-time bring-up only. Runtime allocations from
     * runu/fork/thread demos are too noisy; memory tests re-enable them
     * explicitly around allocator validation.
     */
    allocator_log_enabled = 0;
}

int page_query(uintptr_t base, struct page_info *out)
{
    uint64_t idx;

    if (out == 0 || !memory_ready || pa_to_index(base, &idx, 0) != 0) {
        return -1;
    }
    out->index = idx;
    out->kind = frame_array[idx].kind;
    out->allocated_head = is_alloc_head_tag(frame_array[idx].state);
    return 0;
}

void page_set_kind(uintptr_t base, enum page_kind kind)
{
    uint64_t idx;

    if (memory_ready && pa_to_index(base, &idx, 0) == 0 &&
        is_alloc_head_tag(frame_array[idx].state)) {
        frame_array[idx].kind = kind;
    }
}

int memory_allocator_log_enabled(void)
{
    return allocator_log_enabled;
}
