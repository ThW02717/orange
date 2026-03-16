#include <stdint.h>
#include "memory.h"
#include "memory_test.h"
#include "uart.h"

#define KMTEST_MAX_TRACKED_PTRS 128

static int streq_local(const char *a, const char *b) {
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) {
            return 0;
        }
        a++;
        b++;
    }
    return *a == *b;
}

static void test_log_prefix(void) {
    uart_send_string("[TEST] ");
}

static void test_log_name(const char *name) {
    test_log_prefix();
    uart_send_string(name);
    uart_send_string(": ");
}

static int test_fail(const char *name, const char *reason) {
    test_log_name(name);
    uart_send_string("FAIL: ");
    uart_send_string(reason);
    uart_send_string("\n");
    return -1;
}

static int test_pass(const char *name) {
    test_log_name(name);
    uart_send_string("PASS\n");
    return 0;
}

static int require(int cond, const char *name, const char *reason) {
    if (!cond) {
        return test_fail(name, reason);
    }
    return 0;
}

static int find_class_idx(unsigned int obj_size) {
    unsigned int i;
    struct memory_slab_class_snapshot snap;

    for (i = 0; memory_get_slab_class_snapshot(i, &snap) == 0; i++) {
        if (snap.obj_size == obj_size) {
            return (int)i;
        }
    }
    return -1;
}

static unsigned int total_slabs(const struct memory_slab_class_snapshot *snap) {
    return snap->partial_count + snap->full_count + snap->empty_count;
}

static int kmtest_basic(void) {
    const char *name = "basic";
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot after;
    void *a;
    void *b;
    void *c;
    void *d;

    memory_get_stats(&before);

    a = kmalloc(32);
    b = kmalloc(32);
    c = kmalloc(32);
    if (require(a != 0 && b != 0 && c != 0, name, "kmalloc(32) returned null") != 0) {
        return -1;
    }
    if (require(a != b && a != c && b != c, name, "distinct allocs returned duplicate pointers") != 0) {
        return -1;
    }

    kfree(b);
    d = kmalloc(32);
    if (require(d == b, name, "freed object was not reused from slab freelist") != 0) {
        return -1;
    }

    kfree(a);
    kfree(c);
    kfree(d);

    memory_get_stats(&after);
    if (require(after.object_allocs == before.object_allocs + 4U, name, "object_alloc counter mismatch") != 0) {
        return -1;
    }
    if (require(after.object_frees == before.object_frees + 4U, name, "object_free counter mismatch") != 0) {
        return -1;
    }
    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_boundary(void) {
    static const struct {
        unsigned long size;
        int expect_class;
    } cases[] = {
        {0UL, -1}, {1UL, 0}, {16UL, 0}, {17UL, 1}, {31UL, 1}, {32UL, 1},
        {33UL, 2}, {63UL, 2}, {64UL, 2}, {65UL, 3}, {127UL, 3}, {128UL, 3},
        {129UL, 4}, {255UL, 4}, {256UL, 4}, {257UL, 5}, {511UL, 5}, {512UL, 5},
        {513UL, 6}, {1023UL, 6}, {1024UL, 6}, {1025UL, 7}, {2047UL, 7}, {2048UL, 7},
        {2049UL, -1}
    };
    const char *name = "boundary";
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot after;
    unsigned int i;

    memory_get_stats(&before);

    for (i = 0; i < (unsigned int)(sizeof(cases) / sizeof(cases[0])); i++) {
        void *ptr;
        int cls = memory_class_for_size(cases[i].size);
        struct memory_stats_snapshot pre_case;
        struct memory_stats_snapshot post_case;

        if (require(cls == cases[i].expect_class, name, "size-to-class mapping mismatch") != 0) {
            return -1;
        }

        memory_get_stats(&pre_case);
        ptr = kmalloc(cases[i].size);
        memory_get_stats(&post_case);
        if (cases[i].size == 0UL) {
            if (require(ptr == 0, name, "kmalloc(0) should return null") != 0) {
                return -1;
            }
        } else {
            if (require(ptr != 0, name, "boundary allocation returned null") != 0) {
                return -1;
            }
            if (cases[i].size == 2049UL) {
                struct memory_stats_snapshot pre_free;
                struct memory_stats_snapshot post_free;

                if (require(post_case.page_allocs == pre_case.page_allocs + 1U, name,
                            "2049-byte large alloc did not hit page allocator once") != 0) {
                    return -1;
                }
                memory_get_stats(&pre_free);
                kfree(ptr);
                memory_get_stats(&post_free);
                if (require(post_free.page_frees == pre_free.page_frees + 1U, name,
                            "2049-byte large free did not release one page allocation") != 0) {
                    return -1;
                }
            } else {
                kfree(ptr);
            }
        }
    }

    memory_get_stats(&after);
    if (require(after.object_allocs == before.object_allocs + 23U, name, "unexpected small-object alloc count") != 0) {
        return -1;
    }
    if (require(after.object_frees == before.object_frees + 23U, name, "unexpected small-object free count") != 0) {
        return -1;
    }
    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_reclaim(void) {
    const char *name = "reclaim";
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot after;
    struct memory_slab_class_snapshot snap;
    void *ptrs[KMTEST_MAX_TRACKED_PTRS];
    unsigned int count = 0;
    int class_idx = find_class_idx(128U);

    if (require(class_idx >= 0, name, "128-byte slab class not found") != 0) {
        return -1;
    }

    memory_get_stats(&before);

    while (count < KMTEST_MAX_TRACKED_PTRS) {
        ptrs[count] = kmalloc(128U);
        if (require(ptrs[count] != 0, name, "kmalloc(128) returned null during reclaim setup") != 0) {
            return -1;
        }
        count++;
        if (memory_get_slab_class_snapshot((unsigned int)class_idx, &snap) != 0) {
            return test_fail(name, "failed to read slab snapshot");
        }
        if (total_slabs(&snap) >= 2U) {
            break;
        }
    }

    if (require(total_slabs(&snap) >= 2U, name, "did not expand to at least two slab pages") != 0) {
        return -1;
    }

    while (count > 0) {
        count--;
        kfree(ptrs[count]);
    }

    if (memory_get_slab_class_snapshot((unsigned int)class_idx, &snap) != 0) {
        return test_fail(name, "failed to read slab snapshot after free");
    }
    memory_get_stats(&after);

    if (require(snap.empty_count == 1U, name, "expected one cached empty slab page") != 0) {
        return -1;
    }
    if (require(after.slab_reclaims >= before.slab_reclaims + 1U, name, "expected at least one slab reclaim") != 0) {
        return -1;
    }
    if (require(after.page_frees >= before.page_frees + 1U, name, "expected reclaimed slab page to return to buddy") != 0) {
        return -1;
    }
    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_slab(void) {
    const char *name = "slab";
    struct memory_slab_class_snapshot before;
    struct memory_slab_class_snapshot mid;
    struct memory_slab_class_snapshot after_one_free;
    void *ptrs[KMTEST_MAX_TRACKED_PTRS];
    unsigned int count = 0;
    int class_idx = find_class_idx(128U);

    if (require(class_idx >= 0, name, "128-byte slab class not found") != 0) {
        return -1;
    }
    if (memory_get_slab_class_snapshot((unsigned int)class_idx, &before) != 0) {
        return test_fail(name, "failed to read initial slab snapshot");
    }

    while (count < KMTEST_MAX_TRACKED_PTRS) {
        ptrs[count] = kmalloc(128U);
        if (require(ptrs[count] != 0, name, "kmalloc(128) returned null while filling slab") != 0) {
            return -1;
        }
        count++;
        if (memory_get_slab_class_snapshot((unsigned int)class_idx, &mid) != 0) {
            return test_fail(name, "failed to read slab snapshot while filling");
        }
        if (mid.full_count >= before.full_count + 1U) {
            break;
        }
    }

    if (require(mid.full_count >= before.full_count + 1U, name, "did not drive slab into full state") != 0) {
        return -1;
    }

    kfree(ptrs[count - 1U]);
    if (memory_get_slab_class_snapshot((unsigned int)class_idx, &after_one_free) != 0) {
        return test_fail(name, "failed to read slab snapshot after partial transition");
    }
    if (require(after_one_free.full_count == before.full_count, name, "full slab did not move back to partial") != 0) {
        return -1;
    }
    if (require(after_one_free.partial_count >= before.partial_count + 1U, name,
                "expected at least one partial slab after freeing from full") != 0) {
        return -1;
    }

    while (count > 1U) {
        count--;
        kfree(ptrs[count - 1U]);
    }

    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_multislab(void) {
    const char *name = "multislab";
    struct memory_slab_class_snapshot before;
    struct memory_slab_class_snapshot after_fill;
    void *ptrs[KMTEST_MAX_TRACKED_PTRS];
    unsigned int count = 0;
    int class_idx = find_class_idx(128U);

    if (require(class_idx >= 0, name, "128-byte slab class not found") != 0) {
        return -1;
    }
    if (memory_get_slab_class_snapshot((unsigned int)class_idx, &before) != 0) {
        return test_fail(name, "failed to read initial multislab snapshot");
    }

    while (count < KMTEST_MAX_TRACKED_PTRS) {
        ptrs[count] = kmalloc(128U);
        if (require(ptrs[count] != 0, name, "kmalloc(128) returned null while growing multislab") != 0) {
            return -1;
        }
        count++;
        if (memory_get_slab_class_snapshot((unsigned int)class_idx, &after_fill) != 0) {
            return test_fail(name, "failed to read multislab snapshot");
        }
        if (after_fill.full_count >= before.full_count + 1U &&
            after_fill.partial_count >= before.partial_count + 1U) {
            break;
        }
    }

    if (require(after_fill.full_count >= before.full_count + 1U, name, "expected one full slab page") != 0) {
        return -1;
    }
    if (require(after_fill.partial_count >= before.partial_count + 1U, name, "expected a second partial slab page") != 0) {
        return -1;
    }

    while (count > 0) {
        count--;
        kfree(ptrs[count]);
    }

    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_large(void) {
    const char *name = "large";
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot after;
    void *a;
    void *b;

    memory_get_stats(&before);

    a = kmalloc(2049U);
    b = kmalloc(5000U);
    if (require(a != 0 && b != 0, name, "large allocation returned null") != 0) {
        return -1;
    }

    kfree(a);
    kfree(b);

    memory_get_stats(&after);
    if (require(after.page_allocs == before.page_allocs + 2U, name, "large alloc count mismatch") != 0) {
        return -1;
    }
    if (require(after.page_frees == before.page_frees + 2U, name, "large free count mismatch") != 0) {
        return -1;
    }
    if (require(after.object_allocs == before.object_allocs, name, "large alloc should not change object alloc count") != 0) {
        return -1;
    }
    if (require(after.object_frees == before.object_frees, name, "large free should not change object free count") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_buddy(void) {
    const char *name = "buddy";
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot mid;
    struct memory_stats_snapshot after;
    void *p1;
    void *p2;

    memory_get_stats(&before);

    p1 = p_alloc(1U);
    p2 = p_alloc(3U);
    if (require(p1 != 0 && p2 != 0, name, "direct buddy allocation returned null") != 0) {
        return -1;
    }
    if (require(p1 != p2, name, "direct buddy allocations returned the same address") != 0) {
        return -1;
    }

    memory_get_stats(&mid);
    if (require(mid.page_allocs == before.page_allocs + 2U, name, "buddy page alloc count mismatch") != 0) {
        return -1;
    }
    if (require(mid.free_pages <= before.free_pages - 4U, name, "free page count did not decrease as expected") != 0) {
        return -1;
    }

    p_free(p1);
    p_free(p2);

    memory_get_stats(&after);
    if (require(after.page_frees == before.page_frees + 2U, name, "buddy page free count mismatch") != 0) {
        return -1;
    }
    if (require(after.free_pages == before.free_pages, name, "buddy free pages did not return to baseline") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_invalid(void) {
    const char *name = "invalid";
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot after;
    uint8_t *small;
    uint8_t *large;

    memory_get_stats(&before);

    small = (uint8_t *)kmalloc(128U);
    if (require(small != 0, name, "kmalloc(128) returned null") != 0) {
        return -1;
    }
    kfree(small);
    kfree(small);

    small = (uint8_t *)kmalloc(128U);
    if (require(small != 0, name, "second kmalloc(128) returned null") != 0) {
        return -1;
    }
    kfree((void *)(small + 1U));
    kfree(small);

    large = (uint8_t *)kmalloc(5000U);
    if (require(large != 0, name, "kmalloc(5000) returned null") != 0) {
        return -1;
    }
    kfree((void *)(large + 8U));
    kfree(large);
    kfree(large);

    memory_get_stats(&after);
    if (require(after.object_allocs == before.object_allocs + 2U, name, "invalid test small-object alloc count mismatch") != 0) {
        return -1;
    }
    if (require(after.object_frees == before.object_frees + 2U, name, "invalid frees should only count valid slab frees") != 0) {
        return -1;
    }
    if (require(after.page_allocs == before.page_allocs + 1U, name, "invalid test large alloc count mismatch") != 0) {
        return -1;
    }
    if (require(after.page_frees == before.page_frees + 1U, name, "invalid frees should only count one valid large free") != 0) {
        return -1;
    }
    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_stress(void) {
    const char *name = "stress";
    static const unsigned long sizes[] = {16UL, 32UL, 64UL, 128UL, 256UL, 5000UL};
    struct memory_stats_snapshot before;
    struct memory_stats_snapshot after;
    unsigned int i;

    memory_get_stats(&before);

    for (i = 0; i < 48U; i++) {
        unsigned int j;

        for (j = 0; j < (unsigned int)(sizeof(sizes) / sizeof(sizes[0])); j++) {
            void *ptr = kmalloc(sizes[j]);
            if (require(ptr != 0, name, "stress allocation returned null") != 0) {
                return -1;
            }
            kfree(ptr);
        }
    }

    memory_get_stats(&after);
    if (require(after.object_allocs >= before.object_allocs + (48U * 5U), name,
                "stress test did not perform expected small allocations") != 0) {
        return -1;
    }
    if (require(after.page_allocs >= before.page_allocs + 48U, name,
                "stress test did not perform expected large allocations") != 0) {
        return -1;
    }
    if (require(memory_check_slabs_ok(), name, "slab invariant check failed") != 0) {
        return -1;
    }

    return test_pass(name);
}

static int kmtest_all(void) {
    if (kmtest_basic() != 0) {
        return -1;
    }
    if (kmtest_boundary() != 0) {
        return -1;
    }
    if (kmtest_slab() != 0) {
        return -1;
    }
    if (kmtest_multislab() != 0) {
        return -1;
    }
    if (kmtest_reclaim() != 0) {
        return -1;
    }
    if (kmtest_large() != 0) {
        return -1;
    }
    if (kmtest_buddy() != 0) {
        return -1;
    }
    if (kmtest_invalid() != 0) {
        return -1;
    }
    if (kmtest_stress() != 0) {
        return -1;
    }

    test_log_prefix();
    uart_send_string("SUMMARY: PASS\n");
    return 0;
}

void memory_print_kmtest_usage(void) {
    uart_send_string("usage: kmtest <basic|boundary|slab|multislab|reclaim|large|buddy|invalid|stress|all>\n");
}

int memory_run_kmtest(const char *name) {
    if (name == 0 || name[0] == '\0') {
        memory_print_kmtest_usage();
        return -1;
    }

    if (streq_local(name, "basic")) {
        return kmtest_basic();
    }
    if (streq_local(name, "boundary")) {
        return kmtest_boundary();
    }
    if (streq_local(name, "reclaim")) {
        return kmtest_reclaim();
    }
    if (streq_local(name, "slab")) {
        return kmtest_slab();
    }
    if (streq_local(name, "multislab")) {
        return kmtest_multislab();
    }
    if (streq_local(name, "large")) {
        return kmtest_large();
    }
    if (streq_local(name, "buddy")) {
        return kmtest_buddy();
    }
    if (streq_local(name, "invalid")) {
        return kmtest_invalid();
    }
    if (streq_local(name, "stress")) {
        return kmtest_stress();
    }
    if (streq_local(name, "all")) {
        return kmtest_all();
    }

    memory_print_kmtest_usage();
    return -1;
}
