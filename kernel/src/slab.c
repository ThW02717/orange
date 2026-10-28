/*
 * Byte allocations: kmalloc() selects a cache or a whole buddy block.
 * kfree() checks the page owner, then returns the object or block.
 * Each slab is one page; its header and aligned objects live inside that page.
 */
#include "memory.h"
#include "memory_internal.h"
#include "irqflags.h"
#include "uart.h"

#define SLAB_MAGIC       0x534C4142U /* "SLAB" */
#define KMALLOC_MAX_SLAB_SIZE (PAGE_SIZE / 2U)
#define KMEM_CACHE_EMPTY_LIMIT 1U
#define POISON_FREE_OBJECT 0xAAU
#define POISON_RECLAIM_PAGE 0xCCU

enum slab_state {
    SLAB_EMPTY = 0,
    SLAB_PARTIAL,
    SLAB_FULL,
};

/* A free object stores next in its own bytes; allocated objects use them as data. */
struct kmalloc_free_chunk {
    struct kmalloc_free_chunk *next;
};

struct slab_header;
/* Embedded link: no separate node allocation for the cache lists. */
struct slab_link {
    struct slab_header *prev;
    struct slab_header *next;
};

/*
 * One cache manages one kmalloc size class, such as 64, 128, or 2048 bytes.
 * It owns three lists of slab blocks:
 * - partial: free objects available, including a freshly built slab
 * - full: no free objects left
 * - empty: no allocated objects; keep at most one for quick reuse
 */
struct kmem_cache {
    unsigned int obj_size; /* Power-of-two size, alignment, and object stride. */
    struct slab_header *slabs_partial;
    struct slab_header *slabs_full;
    struct slab_header *slabs_empty;
};

/* Page layout: header, alignment padding, then fixed-stride objects. */
struct slab_header {
    struct slab_link link; /* Node in one kmem_cache slab list. */
    struct kmem_cache *cache; /* Back-pointer to the owning size class. */
    void *freelist; /* Head of free objects inside this slab block. */
    unsigned int inuse; /* Number of currently allocated objects. */
    unsigned int capacity; /* Maximum objects this slab block can hold. */
    enum slab_state state; /* Current list/state: partial, full, or empty. */
    unsigned int magic; /* SLAB_MAGIC, used to catch bad frees/corruption. */
};

/*
 * One-page slab classes cover requests up to 2048 bytes. Larger requests go
 * straight to buddy, avoiding extra pages just to fit an in-block slab header.
 */
static const uint16_t kmalloc_classes[] = {
    16U, 32U, 64U, 128U, 256U, 512U, 1024U, 2048U
};
#define KMALLOC_CLASS_COUNT ((unsigned int)(sizeof(kmalloc_classes) / sizeof(kmalloc_classes[0])))
static struct kmem_cache kmem_caches[KMALLOC_CLASS_COUNT];

static uint64_t object_alloc_count;
static uint64_t object_free_count;
static uint64_t slab_reclaim_count;

static int slab_ready;

/* Align down/up. Alignment values used here are powers of two. */
static uint64_t align_down_u64(uint64_t v, uint64_t a)
{
    return v & ~(a - 1U);
}

static uint64_t align_up_u64(uint64_t v, uint64_t a)
{
    return (v + (a - 1U)) & ~(a - 1U);
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

/* Count list nodes for shell/debug snapshots. Not used on the allocation fast path. */
static unsigned int slab_list_count(struct slab_header *head)
{
    unsigned int n = 0;
    while (head != 0) {
        n++;
        head = head->link.next;
    }
    return n;
}

/* Fill freed/reclaimed memory with a visible byte pattern for debugging. */
static void poison_memory(void *ptr, uint64_t size, uint8_t value)
{
    uint8_t *p = (uint8_t *)ptr;
    uint64_t i;

    if (ptr == 0) {
        return;
    }
    for (i = 0; i < size; i++) {
        p[i] = value;
    }
}

/* Print one slab invariant failure when allocator logging is enabled. */
static void slab_log_invariant_error(unsigned int class_idx, const char *list_name,
                                     struct slab_header *slab, const char *reason)
{
    if (!memory_allocator_log_enabled()) {
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

/* Validate that every slab in a cache list matches the list's promised state. */
static int slab_check_list_invariants(unsigned int class_idx, const char *list_name,
                                      struct slab_header *head, enum slab_state expected_state)
{
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

/*
 * kmem_cache slab-list helpers.
 * These manipulate whole slab blocks between the partial/full/empty lists.
 * The object freelist inside each slab is separate and uses slab_push_free().
 */
static void cache_add_partial(struct kmem_cache *cache, struct slab_header *slab)
{
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

static void cache_add_full(struct kmem_cache *cache, struct slab_header *slab)
{
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

static void cache_add_empty(struct kmem_cache *cache, struct slab_header *slab)
{
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
}

static void cache_remove_slab(struct slab_header *slab)
{
    struct kmem_cache *cache;

    if (slab == 0 || slab->cache == 0) {
        return;
    }
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

    slab->link.prev = 0;
    slab->link.next = 0;
}

static void cache_move_partial_to_full(struct kmem_cache *cache, struct slab_header *slab)
{
    if (cache == 0 || slab == 0) {
        return;
    }

    cache_remove_slab(slab);
    cache_add_full(cache, slab);
}

static void cache_move_full_to_partial(struct kmem_cache *cache, struct slab_header *slab)
{
    if (cache == 0 || slab == 0) {
        return;
    }

    cache_remove_slab(slab);
    cache_add_partial(cache, slab);
}

static void cache_move_empty_to_partial(struct kmem_cache *cache, struct slab_header *slab)
{
    if (cache == 0 || slab == 0) {
        return;
    }

    cache_remove_slab(slab);
    cache_add_partial(cache, slab);
}

/* LIFO object list; next lives in the freed object, not a separate node. */
static void slab_push_free(struct slab_header *slab, void *obj)
{
    struct kmalloc_free_chunk *chunk;

    if (slab == 0 || obj == 0) {
        return;
    }

    chunk = (struct kmalloc_free_chunk *)obj;
    chunk->next = (struct kmalloc_free_chunk *)slab->freelist;
    slab->freelist = obj;
}

/* Return one free object from this slab, or 0 if no object is available. */
static void *slab_pop_free(struct slab_header *slab)
{
    struct kmalloc_free_chunk *chunk;

    if (slab == 0 || slab->freelist == 0) {
        return 0;
    }

    chunk = (struct kmalloc_free_chunk *)slab->freelist;
    slab->freelist = chunk->next;
    return (void *)chunk;
}

/* O(free objects) double-free check; list insertion itself is O(1). */
static int slab_freelist_contains(struct slab_header *slab, void *obj)
{
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

/* Smallest fitting class, or -1 for the whole-page path. */
static int size_to_class(unsigned long size)
{
    unsigned int i;

    if (size == 0 || size > KMALLOC_MAX_SLAB_SIZE) {
        return -1;
    }

    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        if (size <= (unsigned long)kmalloc_classes[i]) {
            return (int)i;
        }
    }
    return -1;
}

/* Skip the header, then align the first object. */
static void *slab_object_start(void *page_base, struct kmem_cache *cache)
{
    uint64_t base;
    uint64_t start;

    if (page_base == 0 || cache == 0) {
        return 0;
    }

    base = (uint64_t)(uintptr_t)page_base;
    start = align_up_u64(base + sizeof(struct slab_header), (uint64_t)cache->obj_size);
    return (void *)(uintptr_t)start;
}

/* Only the bytes after the aligned header are available for objects. */
static unsigned int slab_calc_capacity(void *page_base, struct kmem_cache *cache)
{
    uint64_t base;
    uint64_t start;
    uint64_t usable;

    if (page_base == 0 || cache == 0 || cache->obj_size == 0) {
        return 0;
    }

    base = (uint64_t)(uintptr_t)page_base;
    start = (uint64_t)(uintptr_t)slab_object_start(page_base, cache);
    if (start < base || start >= (base + PAGE_SIZE)) {
        return 0;
    }

    usable = (base + PAGE_SIZE) - start;
    return (unsigned int)(usable / (uint64_t)cache->obj_size);
}

/* Get one buddy page and build its object freelist before publishing the slab. */
static int kmalloc_refill_class(unsigned int class_idx)
{
    void *page;
    struct kmem_cache *cache;
    struct slab_header *slab;
    void *obj_start;
    uint64_t base;
    uint64_t chunks;
    uint64_t i;

    if (class_idx >= KMALLOC_CLASS_COUNT) {
        return -1;
    }
    cache = &kmem_caches[class_idx];

    page = p_alloc(1);
    if (page == 0) {
        return -1;
    }
    /* The buddy tag identifies this page as slab storage for kfree(). */
    base = (uint64_t)(uintptr_t)page;
    page_set_kind(base, PAGE_KIND_SLAB);
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
    chunks = slab->capacity;
    if (chunks == 0) {
        page_set_kind(base, PAGE_KIND_NONE);
        p_free(page);
        return -1;
    }
    /*
     * Carve the slab payload into fixed-size objects and push each free object
     * onto the slab's LIFO object freelist.
     */
    for (i = 0; i < chunks; i++) {
        uint64_t caddr = (uint64_t)(uintptr_t)obj_start + i * (uint64_t)cache->obj_size;
        void *obj = (void *)(uintptr_t)caddr;
        slab_push_free(slab, obj);
    }
    cache_add_partial(cache, slab);
    return 0;
}

static void *kmalloc_locked(unsigned long size)
{
    if (!slab_ready || size == 0UL) {
        return 0;
    }

    /* Small requests use a size-class cache. */
    if (size <= KMALLOC_MAX_SLAB_SIZE) {
        int cls = size_to_class(size);
        struct kmem_cache *cache;
        struct slab_header *slab;
        void *obj;

        if (cls < 0) {
            return 0;
        }

        cache = &kmem_caches[(unsigned int)cls];
        if (cache->slabs_partial == 0) {
            if (cache->slabs_empty != 0) {
                cache_move_empty_to_partial(cache, cache->slabs_empty);
            } else if (kmalloc_refill_class((unsigned int)cls) != 0) {
                return 0;
            }
        }
        /* A partial slab should exist after reusing an empty slab or refilling. */
        if (cache->slabs_partial == 0) {
            log_prefix();
            uart_send_string("[Error] kmalloc no partial slab after refill/reuse for class ");
            uart_send_dec((unsigned long)cls);
            uart_send_string("\n");
            return 0;
        }
        /* Partial list entries must have free objects and valid counts. */
        slab = cache->slabs_partial;
        if (slab->state != SLAB_PARTIAL || slab->freelist == 0 ||
            slab->inuse >= slab->capacity) {
            log_prefix();
            uart_send_string("[Error] kmalloc partial slab invariant broken for class ");
            uart_send_dec((unsigned long)cls);
            uart_send_string("\n");
            return 0;
        }
        /* Pop one free object from the chosen slab's object freelist. */
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
        slab->inuse++;
        if (slab->inuse == slab->capacity) {
            cache_move_partial_to_full(cache, slab);
        }

        if (memory_allocator_log_enabled()) {
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
        object_alloc_count++;
        return obj;
    } else {
        /* Keep the byte API; buddy rounds the page count to a power of two. */
        void *base;
        unsigned long pages;

        /* Bound the request before rounding or narrowing to unsigned int. */
        if (size > (PAGE_SIZE << MEMORY_BUDDY_MAX_ORDER)) {
            return 0;
        }
        pages = (size + PAGE_SIZE - 1UL) / PAGE_SIZE;

        base = p_alloc((unsigned int)pages);
        if (base == 0) {
            return 0;
        }
        page_set_kind((uintptr_t)base, PAGE_KIND_LARGE);
        return base;
    }
}

static void kfree_locked(void *ptr)
{
    uint64_t page_base;
    struct page_info info;
    uint64_t ptr_addr;
    enum page_kind kind;

    if (!slab_ready || ptr == 0) {
        return;
    }
    ptr_addr = (uint64_t)(uintptr_t)ptr;
    page_base = align_down_u64((uint64_t)(uintptr_t)ptr, PAGE_SIZE);
    if (page_query(page_base, &info) != 0) {
        log_prefix();
        uart_send_string("[Error] kfree invalid page base for ptr ");
        log_hex_u64(ptr_addr);
        uart_send_string("\n");
        return;
    }
    kind = info.kind;

    /*
     * page_kind is the handoff point between kmalloc and the lower buddy
     * allocator: slab objects go back to a slab freelist, while large
     * allocations return whole pages through p_free().
     */
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
        uint64_t slab_base = page_base;
        struct slab_header *slab;
        struct kmem_cache *cache;
        uint64_t obj_start;
        uint64_t obj_end;
        uint64_t offset;

        slab = (struct slab_header *)(uintptr_t)slab_base;

        if (slab->magic != SLAB_MAGIC) {
            log_prefix();
            uart_send_string("[Error] kfree slab magic mismatch at page ");
            log_hex_u64(slab_base);
            uart_send_string(" for ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }
        cache = slab->cache;
        if (cache == 0 || cache->obj_size == 0) {
            log_prefix();
            uart_send_string("[Error] kfree slab cache metadata invalid at page ");
            log_hex_u64(slab_base);
            uart_send_string("\n");
            return;
        }

        obj_start = (uint64_t)(uintptr_t)slab_object_start((void *)(uintptr_t)slab_base, cache);
        obj_end = obj_start + (uint64_t)cache->obj_size * (uint64_t)slab->capacity;
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
        if ((offset % (uint64_t)cache->obj_size) != 0) {
            log_prefix();
            uart_send_string("[Error] kfree slab ptr misaligned to object stride: ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string(", stride ");
            uart_send_dec(cache->obj_size);
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
        /*
         * Double-free defense: a valid allocated object must not already be on
         * the slab's free-object list.
         */
        if (slab_freelist_contains(slab, ptr)) {
            log_prefix();
            uart_send_string("[Object] Double free detected at ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }

        /*
         * Return the object to its slab, then move the slab between cache
         * lists if it changed state.
         */
        poison_memory(ptr, cache->obj_size, POISON_FREE_OBJECT);
        slab_push_free(slab, ptr);
        slab->inuse--;
        object_free_count++;
        if (slab->state == SLAB_FULL) {
            cache_move_full_to_partial(cache, slab);
        }
        if (slab->inuse == 0) {
            cache_remove_slab(slab);
            if (cache->slabs_empty == 0) {
                cache_add_empty(cache, slab);

                if (memory_allocator_log_enabled()) {
                    log_prefix();
                    uart_send_string("[Object] Free ");
                    log_hex_u64(ptr_addr);
                    uart_send_string(" at object size ");
                    uart_send_dec(cache->obj_size);
                    uart_send_string(". [Slab] Keep empty page ");
                    log_hex_u64(slab_base);
                    uart_send_string("\n");
                }
            } else {
                /*
                 * Keep at most one empty slab per cache. Extra empty slabs are
                 * returned to the buddy allocator to reduce memory hoarding.
                 */
                page_set_kind(slab_base, PAGE_KIND_NONE);
                slab->magic = 0;
                slab->cache = 0;
                slab->freelist = 0;
                poison_memory((void *)(uintptr_t)slab_base, PAGE_SIZE, POISON_RECLAIM_PAGE);

                if (memory_allocator_log_enabled()) {
                    log_prefix();
                    uart_send_string("[Object] Free ");
                    log_hex_u64(ptr_addr);
                    uart_send_string(" at object size ");
                    uart_send_dec(cache->obj_size);
                    uart_send_string(". [Slab] Reclaim page ");
                    log_hex_u64(slab_base);
                    uart_send_string("\n");
                }
                slab_reclaim_count++;
                p_free((void *)(uintptr_t)slab_base);
            }
            return;
        }
        slab->state = SLAB_PARTIAL;
        if (memory_allocator_log_enabled()) {
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
        /*
         * Large kmalloc pointers must be the page-aligned allocation head;
         * freeing an interior page would lose the original buddy order.
         */
        if ((ptr_addr & (PAGE_SIZE - 1U)) != 0U) {
            log_prefix();
            uart_send_string("[Error] kfree large ptr not page-aligned: ");
            log_hex_u64(ptr_addr);
            uart_send_string("\n");
            return;
        }
        if (!info.allocated_head) {
            log_prefix();
            uart_send_string("[Error] kfree large ptr is not allocation head: ptr ");
            log_hex_u64(ptr_addr);
            uart_send_string(", page ");
            uart_send_dec((unsigned long)info.index);
            uart_send_string("\n");
            return;
        }
        page_set_kind(page_base, PAGE_KIND_NONE);

        if (memory_allocator_log_enabled()) {
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

void *kmalloc(unsigned long size)
{
    uint64_t status = local_irq_save();
    void *ptr = kmalloc_locked(size);
    local_irq_restore(status);
    return ptr;
}

void kfree(void *ptr)
{
    uint64_t status = local_irq_save();
    kfree_locked(ptr);
    local_irq_restore(status);
}

/* Shell/debug dump of slab cache list sizes. */
void memory_print_slabinfo(void)
{
    unsigned int i;
    struct memory_slab_class_snapshot snapshot[KMALLOC_CLASS_COUNT];
    uint64_t status = local_irq_save();

    /* Copy list counts while stable; do not hold IRQs off during UART output. */
    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        (void)memory_get_slab_class_snapshot(i, &snapshot[i]);
    }
    local_irq_restore(status);

    uart_send_string("Slab caches:\n");
    uart_send_string("class size partial full empty\n");
    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        uart_send_dec(i);
        uart_send_string(" ");
        uart_send_dec(snapshot[i].obj_size);
        uart_send_string(" ");
        uart_send_dec(snapshot[i].partial_count);
        uart_send_string(" ");
        uart_send_dec(snapshot[i].full_count);
        uart_send_string(" ");
        uart_send_dec(snapshot[i].empty_count);
        uart_send_string("\n");
    }
}

/* Runtime invariant check for all slab cache lists. */
int memory_check_slabs_ok(void)
{
    unsigned int i;
    int ok = 1;
    uint64_t status = local_irq_save();

    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        if (!slab_check_list_invariants(i, "partial", kmem_caches[i].slabs_partial, SLAB_PARTIAL)) {
            ok = 0;
        }
        if (!slab_check_list_invariants(i, "full", kmem_caches[i].slabs_full, SLAB_FULL)) {
            ok = 0;
        }
        if (!slab_check_list_invariants(i, "empty", kmem_caches[i].slabs_empty, SLAB_EMPTY)) {
            ok = 0;
        }
    }

    local_irq_restore(status);
    return ok;
}

/* Copy one kmalloc cache's state into a test-friendly snapshot struct. */
int memory_get_slab_class_snapshot(unsigned int class_idx, struct memory_slab_class_snapshot *out)
{
    struct kmem_cache *cache;
    uint64_t status;

    if (out == 0 || class_idx >= KMALLOC_CLASS_COUNT) {
        return -1;
    }
    status = local_irq_save();

    cache = &kmem_caches[class_idx];
    out->obj_size = cache->obj_size;
    out->partial_count = slab_list_count(cache->slabs_partial);
    out->full_count = slab_list_count(cache->slabs_full);
    out->empty_count = slab_list_count(cache->slabs_empty);
    local_irq_restore(status);
    return 0;
}

/* Expose size-to-class mapping for allocator tests and shell diagnostics. */
int memory_class_for_size(unsigned long size)
{
    return size_to_class(size);
}

/* Shell/debug dump of allocator counters and debug settings. */
void memory_print_memstat(void)
{
    struct memory_stats_snapshot snapshot;
    memory_get_stats(&snapshot);

    uart_send_string("Allocator stats:\n");
    uart_send_string("page_allocs: ");
    uart_send_dec((unsigned long)snapshot.page_allocs);
    uart_send_string("\n");
    uart_send_string("page_frees: ");
    uart_send_dec((unsigned long)snapshot.page_frees);
    uart_send_string("\n");
    uart_send_string("object_allocs: ");
    uart_send_dec((unsigned long)snapshot.object_allocs);
    uart_send_string("\n");
    uart_send_string("object_frees: ");
    uart_send_dec((unsigned long)snapshot.object_frees);
    uart_send_string("\n");
    uart_send_string("slab_reclaims: ");
    uart_send_dec((unsigned long)snapshot.slab_reclaims);
    uart_send_string("\n");
    uart_send_string("total_pages: ");
    uart_send_dec((unsigned long)snapshot.total_pages);
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

/* Shell command helper for checking slab list invariants. */
void memory_debug_check_slabs(void)
{
    if (memory_check_slabs_ok()) {
        uart_send_string("slabcheck: OK\n");
    } else {
        uart_send_string("slabcheck: FAILED\n");
    }
}

void slab_init(void)
{
    unsigned int i;

    for (i = 0; i < KMALLOC_CLASS_COUNT; i++) {
        kmem_caches[i].obj_size = (unsigned int)kmalloc_classes[i];
        kmem_caches[i].slabs_partial = 0;
        kmem_caches[i].slabs_full = 0;
        kmem_caches[i].slabs_empty = 0;
    }

    slab_ready = 1;
}

void slab_get_stats(struct memory_stats_snapshot *out)
{
    out->object_allocs = object_alloc_count;
    out->object_frees = object_free_count;
    out->slab_reclaims = slab_reclaim_count;
    out->empty_slab_limit = KMEM_CACHE_EMPTY_LIMIT;
}
