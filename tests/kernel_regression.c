#include "thread_internal.h"
#include "sched.h"
#include "user_layout.h"
#include <stdint.h>
#include <stddef.h>
#include "trap.h"
#include "bottom_half.h"
#include "custom_work.h"
#include "demo.h"

#include "kernel_test.h"
#include "thread.h"
#include "timer.h"
#include "uart.h"
#include "exec.h"
#include "cpio.h"
#include "platform.h"
#include "syscall.h"
#include "syscall_dispatch.h"
#include "irqflags.h"

/* Link production modules normally. Allocation, archive input, the trap
 * dispatcher and optional bottom-half arrival probes are fixtures.
 */
static unsigned char heap[4194304] __attribute__((aligned(4096)));
static unsigned int heap_used;
static int allocations_before_failure = -1;
static unsigned int frees;
static unsigned int workers;
static unsigned int s_traps;
static unsigned int u_traps;
static unsigned int callback_a_done;
static unsigned int callback_b_done;
static unsigned int reruns;
static unsigned int expected_final_s_traps;
static volatile int hardware_task_test;
static volatile int hardware_outer_running;
static volatile int hardware_queued_ran;
static unsigned int hardware_irq_count;
static unsigned char user_stack[4096] __attribute__((aligned(16)));
static unsigned char user_kstack[16384] __attribute__((aligned(16)));

extern void supervisor_roundtrip(void);
extern void user_roundtrip(void);
extern const char *test_uart_races(void);

static void puts_raw(const char *s)
{
    while (*s) {
        while ((*(volatile uint8_t *)0x10000005 & 0x20) == 0) {}
        *(volatile uint8_t *)0x10000000 = (uint8_t)*s++;
    }
}

static void finish(int ok) __attribute__((noreturn));
static void finish(int ok)
{
    puts_raw(ok ? "[REGRESSION] SUMMARY: PASS\n" : "[REGRESSION] SUMMARY: FAIL\n");
    *(volatile uint32_t *)0x100000 = ok ? 0x5555 : 0x3333;
    for (;;) { asm volatile("wfi"); }
}

static void check(int ok, const char *reason)
{
    if (!ok) {
        puts_raw(reason);
        puts_raw("\n");
        finish(0);
    }
}

void *kmalloc(unsigned long size)
{
    unsigned int pos = (heap_used + 15U) & ~15U;
    if (allocations_before_failure == 0) { return 0; }
    if (allocations_before_failure > 0) { allocations_before_failure--; }
    if (size > sizeof(heap) - pos) { return 0; }
    heap_used = pos + size;
    return heap + pos;
}

void kfree(void *ptr)
{
    check(ptr != 0, "unexpected null free");
    frees++;
}

/* The GPR test deliberately puts 0x104 in tp, not a real thread pointer. */
int user_has_exited(void) { return 0; }

const struct boot_info *platform_boot_info(void)
{
    static const struct boot_info boot = { .initrd_start = 1, .initrd_end = 5 };
    return &boot;
}

int cpio_find(const void *start, const void *end, const char *name,
              const void **data, unsigned long *size, unsigned int *mode)
{
    static const uint32_t image = 0x00000013;
    (void)start;
    (void)end;
    (void)name;
    *data = &image;
    *size = sizeof(image);
    *mode = 0100644;
    return 0;
}

int memory_run_kmtest(const char *name) { (void)name; return -1; }
void memory_print_kmtest_usage(void) {}

static long fork_from_context(struct trap_context *tc)
{
    tc->a7 = SYS_fork;
    handle_user_ecall(tc);
    return (long)tc->a0;
}

static void start_test_timer(void)
{
    timer_init();
    timer_source_mask();
    local_irq_disable();
}

static void hardware_queued_callback(void *arg)
{
    (void)arg;
    check(!hardware_outer_running && hardware_irq_count == 1,
          "queued callback ran before interrupted callback returned");
    hardware_queued_ran = 1;
}

/* Field order follows trap_context; x2/sp is checked separately. The second
 * trap in each pair checks the actual register values restored by sret.
 */
void trap_dispatch(struct trap_context *tc)
{
    if (hardware_task_test && tc->scause == (SCAUSE_INTERRUPT_BIT | SCAUSE_S_TIMER_INT)) {
        check((tc->sstatus & SSTATUS_SPP) != 0, "expected nested S-mode timer IRQ");
        timer_source_mask();
        (void)sbi_set_timer(UINT64_MAX);
        hardware_irq_count++;
        check(defer_callback(hardware_queued_callback, 0) == 0, "IRQ enqueue failed");
        return;
    }
    const uint64_t expected[] = {
        0x101, 0, 0x103, 0x104, 0x105, 0x106, 0x107, 0x108, 0x109,
        0x10a, 0x10b, 0x10c, 0x10d, 0x10e, 0x10f, 0x110, 0x111,
        0x112, 0x113, 0x114, 0x115, 0x116, 0x117, 0x118, 0x119,
        0x11a, 0x11b, 0x11c, 0x11d, 0x11e, 0x11f
    };
    const uint64_t *words = (const uint64_t *)tc;
    uint64_t scratch;
    asm volatile("csrr %0, sscratch" : "=r"(scratch));
    check(scratch == 0, "sscratch must be zero while handling S-mode work");
    check(((uintptr_t)tc & 15U) == 0, "trap context stack alignment");
    for (unsigned int i = 0; i < 31; i++) {
        if (i != 1) { check(words[i] == expected[i], "GPR corrupted across trap"); }
    }
    if (tc->sstatus & SSTATUS_SPP) {
        check(tc->scause == 3, "expected supervisor breakpoint");
        check(tc->sp == (uintptr_t)tc + TRAP_CONTEXT_ALLOC_SIZE, "S-mode interrupted sp");
        s_traps++;
    } else {
        check(tc->scause == SCAUSE_U_ECALL, "expected user ecall");
        check(tc->sp == (uintptr_t)user_stack + sizeof(user_stack), "U-mode interrupted sp");
        u_traps++;
        if (u_traps == 1) {
            supervisor_roundtrip();
        } else {
            check(s_traps == expected_final_s_traps, "missing supervisor/nested traps");
            puts_raw("[REGRESSION] all 31 GPRs: S->S, U->S, nested S->S: PASS\n");
            finish(1);
        }
    }
    tc->sepc += 4;
}

static void worker(void *arg)
{
    (void)arg;
    for (unsigned int i = 0; i < 4; i++) { workers++; thread_yield(); }
}

static void callback_b(void *arg)
{
    (void)arg;
    check(callback_a_done, "nested runner executed callback before outer completion");
    callback_b_done++;
}

static void callback_a(void *arg)
{
    (void)arg;
    check(defer_callback(callback_b, 0) == 0, "enqueue callback B");
    run_bottom_halves();
    check(callback_b_done == 0, "deferred runner reentered");
    supervisor_roundtrip();
    check(callback_b_done == 0, "nested trap ran deferred callback recursively");
    callback_a_done = 1;
}

static void rerun_callback(void *arg)
{
    (void)arg;
    if (++reruns == 1)
        check(defer_callback(rerun_callback, 0) == 0, "enqueue successor");
}

static unsigned int task_events[64];
static unsigned int task_event_count;

static void record_task(void *arg)
{
    uint64_t status;
    asm volatile("csrr %0, sstatus" : "=r"(status));
    check((status & SSTATUS_SIE) != 0, "callback ran with SIE disabled");
    check(task_event_count < 64, "test event overflow");
    task_events[task_event_count++] = (unsigned int)(uintptr_t)arg;
}

static void outer_callback(void *arg)
{
    (void)arg;
    record_task((void *)10);
    check(defer_callback(record_task, (void *)13) == 0, "enqueue first callback");
    check(defer_callback(record_task, (void *)14) == 0, "enqueue second callback");
    check(defer_callback(record_task, (void *)12) == 0, "enqueue third callback");
    run_bottom_halves();
    check(task_event_count == 1, "explicit nested runner executed a callback");
    supervisor_roundtrip();
    check(task_event_count == 1, "nested trap executed a later callback");
    record_task((void *)11);
}

static void sequential_callback(void *arg)
{
    unsigned int step = (unsigned int)(uintptr_t)arg;
    unsigned int before;

    record_task((void *)(uintptr_t)step);
    before = task_event_count;
    if (step != 0) {
        check(defer_callback(sequential_callback, (void *)(uintptr_t)(step - 1)) == 0, "enqueue next step");
        supervisor_roundtrip();
        check(task_event_count == before, "step callback ran recursively");
    }
    record_task((void *)(uintptr_t)(step + 8));
}

static void task_stack_worker(void *arg)
{
    struct thread *self = thread_current();
    volatile unsigned char *guard = self->kstack_base;
    (void)arg;
    for (unsigned int i = 0; i < 32; i++) { guard[i] = 0xa5; }
    check(defer_callback(sequential_callback, (void *)7) == 0, "enqueue step ladder");
    run_bottom_halves();
    for (unsigned int i = 0; i < 32; i++) {
        check(guard[i] == 0xa5, "tasks exhausted the thread kernel stack");
    }
}

static void hardware_outer_callback(void *arg)
{
    uint64_t start = timer_read();
    (void)arg;
    hardware_outer_running = 1;
    (void)sbi_set_timer(start + 10000);
    asm volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    while (hardware_irq_count == 0 && timer_read() - start < 10000000) {
        asm volatile("" ::: "memory");
    }
    check(!hardware_queued_ran && hardware_irq_count == 1,
          "top half was blocked or bottom half ran recursively");
    hardware_outer_running = 0;
}

/* Test the production bitmap runner with controlled subsystem arrivals. */
static int bitmap_probe;
static unsigned int bitmap_timer_runs;

void test_timer_bottom_half(void)
{
    if (!bitmap_probe) {
        timer_run_bottom_half();
        return;
    }
    record_task((void *)20);
    if (++bitmap_timer_runs < 3) {
        raise_bottom_half(BH_TIMER);
        raise_bottom_half(BH_TIMER);
        run_bottom_halves();
        check(bitmap_timer_runs < 3, "nested runner consumed next round");
    }
}

void test_uart_rx_bottom_half(void)
{
    if (bitmap_probe)
        record_task((void *)21);
    else
        uart_rx_run_bottom_half();
}

void test_uart_tx_bottom_half(void)
{
    if (bitmap_probe)
        record_task((void *)22);
    else
        uart_tx_run_bottom_half();
}

static void submit_next_batch(void *arg)
{
    (void)arg;
    record_task((void *)30);
    raise_bottom_half(BH_TIMER);
    check(defer_callback(record_task, (void *)32) == 0, "submit next batch");
}

static void test_irq_flags(void)
{
    const uint64_t sum = 1UL << 18;
    uint64_t original_status, original_sources, outer, inner, current;

    /* No device IRQ may run while this test deliberately changes SIE/SUM. */
    asm volatile("csrr %0, sstatus" : "=r"(original_status) :: "memory");
    asm volatile("csrrw %0, sie, zero" : "=r"(original_sources) :: "memory");
    local_irq_disable();
    asm volatile("csrc sstatus, %0" :: "r"(sum) : "memory");
    local_irq_enable();

    outer = local_irq_save();
    check((outer & SSTATUS_SIE) && !local_irqs_enabled(), "IRQ save lost enabled state");
    asm volatile("csrs sstatus, %0" :: "r"(sum) : "memory");
    inner = local_irq_save();
    check(!(inner & SSTATUS_SIE), "nested IRQ save lost disabled state");
    asm volatile("csrc sstatus, %0" :: "r"(sum) : "memory");
    local_irq_restore(inner);
    asm volatile("csrr %0, sstatus" : "=r"(current) :: "memory");
    check(!(current & (sum | SSTATUS_SIE)), "IRQ restore overwrote cleared SUM or enabled nested IRQs");

    asm volatile("csrs sstatus, %0" :: "r"(sum) : "memory");
    local_irq_restore(outer);
    asm volatile("csrr %0, sstatus" : "=r"(current) :: "memory");
    check((current & (sum | SSTATUS_SIE)) == (sum | SSTATUS_SIE),
          "IRQ restore overwrote set SUM or lost enabled SIE");

    /* Restore the complete test fixture state, not just interrupt flags. */
    local_irq_disable();
    asm volatile("csrw sstatus, %0" :: "r"(original_status) : "memory");
    asm volatile("csrw sie, %0" :: "r"(original_sources) : "memory");
    puts_raw("[REGRESSION] IRQ flags preserve SUM changes and nested SIE state: PASS\n");
}

static void test_bitmap_rounds(void)
{
    static const unsigned int expected[] = {20, 21, 22, 23, 20, 20};
    static const unsigned int batches[] = {30, 31, 20, 32};

    bitmap_probe = 1;
    bitmap_timer_runs = 0;
    task_event_count = 0;
    raise_bottom_half(BH_TIMER);
    raise_bottom_half(BH_TIMER);
    raise_bottom_half(BH_UART_RX);
    raise_bottom_half(BH_UART_TX);
    raise_bottom_half(BH_COUNT);
    raise_bottom_half((enum bottom_half)-1);
    check(defer_callback(record_task, (void *)23) == 0, "submit custom bit");
    run_bottom_halves();
    check(task_event_count == 6, "bitmap duplicated or lost a type");
    for (unsigned int i = 0; i < 6; i++)
        check(task_events[i] == expected[i], "new TIMER overtook this round's UART/custom");
    check(!bottom_halves_pending(), "bitmap did not drain");

    task_event_count = 0;
    check(defer_callback(submit_next_batch, 0) == 0, "submit batch producer");
    check(defer_callback(record_task, (void *)31) == 0, "submit batch tail");
    run_bottom_halves();
    check(task_event_count == 4, "custom batch lost work");
    for (unsigned int i = 0; i < 4; i++)
        check(task_events[i] == batches[i], "custom appended work to current batch");

    raise_bottom_half(BH_TIMER);
    raise_bottom_half(BH_UART_RX);
    clear_bottom_half(BH_TIMER);
    task_event_count = 0;
    local_irq_enable();
    run_bottom_halves();
    check(local_irqs_enabled(), "runner lost caller's enabled SIE");
    local_irq_disable();
    check(task_event_count == 1 && task_events[0] == 21, "clear affected another bit");
    bitmap_probe = 0;
    puts_raw("[REGRESSION] bitmap coalescing, rounds, UART fairness, custom snapshots, SIE restore: PASS\n");
}

static void fill_pool_while_running(void *arg)
{
    (void)arg;
    for (unsigned int i = 0; i < CUSTOM_WORK_CAPACITY - 1; i++)
        check(defer_callback(record_task, (void *)(uintptr_t)i) == 0,
              "running slot test filled early");
    check(defer_callback(record_task, 0) == -1, "running slot reused before return");
    run_bottom_halves();
    check(task_event_count == 0, "new custom batch ran recursively");
}

static void test_custom_tasks(void)
{
    uint64_t status;
    unsigned int before;

    task_event_count = 0;
    check(defer_callback(record_task, (void *)1) == 0 &&
          defer_callback(record_task, (void *)2) == 0 &&
          defer_callback(record_task, (void *)0) == 0, "queue FIFO callbacks");
    run_bottom_halves();
    check(task_event_count == 3 && task_events[0] == 1 &&
          task_events[1] == 2 && task_events[2] == 0, "custom FIFO order");

    task_event_count = 0;
    check(defer_callback(outer_callback, 0) == 0, "enqueue outer callback");
    run_bottom_halves();
    check(task_event_count == 5, "missing custom callback");
    for (unsigned int i = 0; i < 5; i++) {
        check(task_events[i] == (unsigned int[]){10, 11, 13, 14, 12}[i], "custom batch order");
    }

    task_event_count = 0;
    for (unsigned int i = 0; i < CUSTOM_WORK_CAPACITY; i++) {
        check(defer_callback(record_task, (void *)(uintptr_t)i) == 0, "pool filled early");
    }
    check(defer_callback(record_task, 0) == -1, "pool overflow accepted");
    check(defer_callback(0, 0) == -1, "invalid callback accepted");
    run_bottom_halves();
    check(task_event_count == CUSTOM_WORK_CAPACITY, "pool lost callbacks");
    check(defer_callback(record_task, 0) == 0, "pool slot was not released");
    run_bottom_halves();
    asm volatile("csrr %0, sstatus" : "=r"(status));
    check(!(status & SSTATUS_SIE), "runner changed caller's disabled SIE");

    task_event_count = 0;
    check(defer_callback(fill_pool_while_running, 0) == 0, "queue running slot test");
    run_bottom_halves();
    check(task_event_count == CUSTOM_WORK_CAPACITY - 1, "next batch lost callbacks");

    task_event_count = 0;
    /* Each callback returns before its successor runs on this stack. */
    before = thread_test_next_tid(1000);
    check(thread_create_kernel(task_stack_worker, 0) > 0, "create stack test worker");
    thread_run_until_idle();
    (void)thread_test_next_tid(before);
    check(task_event_count == 16, "missing step callback");
    for (unsigned int i = 0; i < 8; i++) {
        check(task_events[2 * i] == 7 - i && task_events[2 * i + 1] == 15 - i,
              "step callback did not finish before its successor");
    }

    hardware_task_test = 1;
    check(defer_callback(hardware_outer_callback, 0) == 0, "enqueue hardware test");
    run_bottom_halves();
    hardware_task_test = 0;
    timer_source_mask();
    check(hardware_queued_ran && !bottom_halves_pending(), "queued queued callback was lost");
    puts_raw("[REGRESSION] custom FIFO/batches, non-reentrant callbacks, pool reuse, nested timer top half: PASS\n");
}

static unsigned int timer_cleanup_count;
static void timer_test_cleanup(void *arg)
{
    (void)arg;
    timer_cleanup_count++;
}

static void timer_test_callback(void *arg)
{
    (*(unsigned int *)arg)++;
}

static void stop_timer_task(void *arg)
{
    (void)arg;
    timer_stop();
}

static void timer_deferred_stop_callback(void *arg)
{
    (*(unsigned int *)arg)++;
    check(defer_callback(stop_timer_task, 0) == 0, "enqueue timer stop");
    supervisor_roundtrip();
    check(timer_is_running() && timer_cleanup_count == 0,
          "queued stop ran before timer callback returned");
}

static void test_timer_ownership(void)
{
    unsigned int fired = 0;
    start_test_timer();
    check(add_timer_with_cleanup(timer_test_callback, &fired, 0,
                                 timer_test_cleanup) == 0, "queue owned due timer");
    check(add_timer_with_cleanup(timer_test_callback, &fired, 10000000,
                                 timer_test_cleanup) == 0, "queue owned future timer");
    timer_irq_top();
    run_bottom_halves();
    check(fired == 1 && timer_cleanup_count == 1, "fired payload not released once");
    timer_stop();
    check(fired == 1 && timer_cleanup_count == 2, "cancelled payload not released once");
    check(add_timer_with_cleanup(timer_test_callback, &fired, 0,
                                 timer_test_cleanup) == -1 && timer_cleanup_count == 2,
          "failed registration took caller ownership");

    fired = 0;
    timer_cleanup_count = 0;
    start_test_timer();
    check(add_timer_with_cleanup(timer_deferred_stop_callback, &fired, 0,
                                 timer_test_cleanup) == 0, "queue deferred stop timer");
    check(add_timer_with_cleanup(timer_test_callback, &fired, 10000000,
                                 timer_test_cleanup) == 0, "queue cancelled future timer");
    timer_irq_top();
    run_bottom_halves();
    check(fired == 1 && timer_cleanup_count == 2 && !timer_is_running() &&
          !bottom_halves_pending(), "deferred stop leaked/repeated a callback or cleanup");
    puts_raw("[REGRESSION] timer fire/cancel/failure/deferred-stop payload ownership: PASS\n");
}

static void stop_from_callback(void *arg)
{
    unsigned int *called = (unsigned int *)arg;
    (*called)++;
    timer_stop();
}

static void test_timer_budget(void)
{
    unsigned int fired = 0;
    unsigned int before = frees;

    start_test_timer();
    for (unsigned int i = 0; i < 20; i++)
        check(add_timer(timer_test_callback, &fired, 0) == 0, "queue budget events");
    /* Drive the real handler with IRQs off to inspect the boundary between
     * batches before the next hardware timer notification can arrive.
     */
    timer_run_bottom_half();
    check(fired == 16 && timer_pending_count() == 4 && bottom_halves_pending(),
          "timer failed to yield after its event budget");
    clear_bottom_half(BH_TIMER);
    timer_run_bottom_half();
    check(fired == 20 && timer_pending_count() == 0 && frees - before == 20,
          "timer budget lost events or cleanup");
    timer_stop();
    puts_raw("[REGRESSION] real timer event budget and continuation: PASS\n");
}

static void test_user_groups(void)
{
    struct thread *idle = thread_current();
    struct thread *parent;
    struct thread *other;
    struct thread *sibling;
    struct thread *child;
    uintptr_t stack;
    uintptr_t parent_stack;
    int parent_tid;
    int other_tid;
    int tid;
    unsigned int before;

    check(!thread_has_runnable_tasks(), "fixture heap still in use");
    heap_used = 0;
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    child = thread_prepare_user(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
    check(child && child->state == THREAD_NEW && !thread_user_image_busy(0) &&
          thread_test_find(child->tid) == 0 && !thread_has_runnable_tasks(),
          "prepared child visible before publication");
    thread_publish(child);
    thread_publish(child);
    check(thread_test_find(child->tid) == child && thread_has_runnable_tasks() && child->state == THREAD_RUNNABLE,
          "published child missing from scheduler");
    check(thread_stop_tid(child->tid, 0) == 0, "stop prepared child");
    thread_run_until_idle();
    heap_used = 0;
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    parent_tid = user_process_create(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    other_tid = user_process_create(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
    parent = thread_test_find(parent_tid);
    other = thread_test_find(other_tid);
    check(parent && other && parent->tid != other->tid, "new process IDs");
    parent_stack = parent->user_stack_base;
    check(parent->tid == parent->tgid && other->tid == other->tgid, "leader IDs");
    check(thread_create_user(USER_CODE_BASE, 0, USER_CODE_BASE + 4) == -1,
          "kernel caller cannot create a same-process user thread");

    /* Run constructor/syscall code as parent, without entering the fixture's
     * fake user image. Restore idle before any real scheduler switch.
     */
    sched_test_set_current(parent);
    parent->tc->gp = 0x12345678;
    before = heap_used;
    check(thread_create_user(0, 0, USER_CODE_BASE + 4) == -1 &&
          thread_create_user(USER_CODE_BASE + 1, 0, USER_CODE_BASE + 4) == -1 &&
          thread_create_user(USER_CODE_BASE, 0, USER_CODE_LIMIT) == -1 &&
          thread_create_user(USER_CODE_BASE, 0, USER_CODE_BASE + 1) == -1,
          "invalid thread entry/return");
    check(heap_used == before, "invalid entry allocated memory");

    for (int budget = 0; budget < 3; budget++) {
        unsigned int heap_before = heap_used;
        before = frees;
        allocations_before_failure = budget;
        check(thread_create_user(USER_CODE_BASE, 0, USER_CODE_BASE + 4) == -1,
              "sibling OOM must fail");
        allocations_before_failure = -1;
        check(frees - before == (unsigned int)budget && thread_user_group_size(parent) == 1,
              "sibling OOM leaked or published a task");
        heap_used = heap_before;
    }
    tid = thread_create_user(USER_CODE_BASE + 8, parent->user_stack_base + 64, USER_CODE_BASE + 4);
    sibling = thread_test_find(tid);
    check(sibling && sibling->tid != parent->tid && sibling->tgid == parent->tgid,
          "same process, different TID");
    check(sibling->kstack_base != parent->kstack_base &&
          sibling->user_stack_base != parent->user_stack_base && sibling->tc != parent->tc,
          "sibling has private stacks and context");
    check(sibling->tc->a0 == parent->user_stack_base + 64 &&
          sibling->tc->ra == USER_CODE_BASE + 4 && sibling->tc->sepc == USER_CODE_BASE + 8 &&
          sibling->tc->gp == parent->tc->gp && sibling->tc->tp == (uintptr_t)sibling &&
          sibling->tc->sp == sibling->user_stack_top && sibling->tc->sstatus == SSTATUS_SPIE,
          "sibling initial registers");
    check(thread_user_group_size(parent) == 2 && fork_from_context(parent->tc) == -1,
          "multithreaded fork must fail");
    check(thread_user_stack_contains(sibling, parent->user_stack_base, parent->user_stack_top) &&
          !thread_user_stack_contains(other, parent->user_stack_base, parent->user_stack_top) &&
          !thread_user_stack_contains(parent, parent->user_stack_base, parent->user_stack_top + 1),
          "group stack buffer boundaries");
    check(thread_stop_tid(tid, 0) == 0 && thread_user_group_size(parent) == 1,
          "thread exit kept group alive");
    thread_reap_zombies();

    parent->tc->sepc = USER_CODE_BASE + 20;
    parent->tc->sp = parent->user_stack_base - 1;
    before = heap_used;
    check(fork_from_context(parent->tc) == -1 && heap_used == before, "invalid fork sp allocated child");
    parent->tc->sp = parent->user_stack_top - 64;
    parent->tc->s0 = parent->user_stack_top - 32;
    *(uint64_t *)(parent->user_stack_base + 64) = parent->user_stack_base + 128;
    *(uint64_t *)(parent->user_stack_base + 72) = 0x1234;
    for (int budget = 0; budget < 3; budget++) {
        unsigned int heap_before = heap_used;
        before = frees;
        allocations_before_failure = budget;
        check(fork_from_context(parent->tc) == -1, "fork OOM must fail");
        allocations_before_failure = -1;
        check(frees - before == (unsigned int)budget && other->state == THREAD_RUNNABLE && thread_user_group_size(parent) == 1,
              "fork OOM leaked or published a child");
        heap_used = heap_before;
    }
    tid = (int)fork_from_context(parent->tc);
    child = thread_test_find(tid);
    check(child && child->tid == child->tgid && child->tgid != parent->tgid, "fork new process");
    check(child->tc->a0 == 0 && child->tc->sepc == parent->tc->sepc &&
          child->tc->tp == (uintptr_t)child && child->tc->sp == child->user_stack_top - 64 &&
          child->tc->s0 == child->user_stack_top - 32 && child->state == THREAD_RUNNABLE,
          "fork published incomplete context");
    check(*(uint64_t *)(child->user_stack_base + 64) == child->user_stack_base + 128 &&
          *(uint64_t *)(child->user_stack_base + 72) == 0x1234,
          "fork stack copy/rebase");

    tid = thread_create_user(USER_CODE_BASE, 0, USER_CODE_BASE + 4);
    sibling = thread_test_find(tid);
    check(sibling != 0, "create survivor");
    thread_mark_current_zombie(0);
    sched_test_set_current(idle);
    thread_reap_zombies();
    check(thread_test_find(parent_tid) == 0 && sibling->tgid == parent_tid,
          "reaping leader changed process ID");
    check(!thread_user_stack_contains(sibling, parent_stack, parent_stack + 1),
          "exited leader stack still accepted");
    check(!thread_user_stack_contains(sibling, stack, stack + 1), "other process stack accepted");
    check(thread_user_image_busy(sibling), "other process still uses image");
    before = frees;
    check(thread_stop_process(parent_tid, 0) == 0 && thread_stop_process(parent_tid, 0) == 0,
          "stop group without leader");
    thread_reap_zombies();
    check(frees - before == 3 && other->state == THREAD_RUNNABLE && child->state == THREAD_RUNNABLE,
          "group stop affected another process or double-freed");
    check(thread_stop_process(other->tgid, 0) == 0 && thread_stop_process(child->tgid, 0) == 0,
          "stop remaining groups");
    thread_run_until_idle();
    check(!thread_has_runnable_tasks() && !thread_user_image_busy(0), "group cleanup");

    (void)thread_test_next_tid(INT32_MAX);
    check(thread_create_kernel(worker, 0) == INT32_MAX &&
          thread_create_kernel(worker, 0) == -1, "TID overflow");
    check(thread_stop_tid(INT32_MAX, 0) == 0, "stop last representable TID");
    thread_run_until_idle();
    puts_raw("[REGRESSION] user groups, register seeds, fork copy, leader exit, ID overflow: PASS\n");
    puts_raw("[REGRESSION] sibling/fork allocation-failure rollback (3 + 3 positions): PASS\n");
}

void regression_main(void)
{
    int pid;
    struct exec_result result;
    struct trap_context *tc;
    unsigned int timer_called = 0;
    asm volatile("csrw stvec, %0" : : "r"(trap_entry));

    thread_system_bootstrap_init();
    pid = thread_create_kernel(worker, 0);
    check(pid > 0, "create stopped worker");
    check(thread_stop_tid(pid, 0) == 0, "stop runnable worker");
    check(thread_stop_tid(pid, 0) == 0, "repeat stop");
    thread_run_until_idle();
    check(workers == 0 && frees == 2, "zombie removed/freed exactly once");
    check(!thread_has_runnable_tasks(), "stale scheduler links");
    check(thread_create_kernel(worker, 0) > 0 && thread_create_kernel(worker, 0) > 0, "create workers");
    {
        struct thread *boot = thread_current();
        thread_system_bootstrap_init();
        check(thread_current() == boot && thread_has_runnable_tasks(),
              "repeated bootstrap reset a live run queue");
    }
    thread_run_until_idle();
    check(workers == 8 && frees == 6, "cooperative scheduling and reclamation");
    puts_raw("[REGRESSION] scheduler stop/repeat-stop/yield/reclaim: PASS\n");

    /* Fail every allocation position in a two-task launch: user stack,
     * control block, and kernel stack for each task. No partial task may run.
     */
    for (int budget = 0; budget < 6; budget++) {
        unsigned int before = frees;
        allocations_before_failure = budget;
        check(exec_spawn("preempt", 2, &result) == -1, "launch must fail");
        allocations_before_failure = -1;
        check(frees - before == (unsigned int)budget, "launch failure leaked allocations");
        check(!thread_has_runnable_tasks() && !thread_user_image_busy(0),
              "launch failure left live user task");
    }
    puts_raw("[REGRESSION] user launch allocation-failure rollback (6 positions): PASS\n");
    for (unsigned int which = 0; which < 2; which++) {
        for (int budget = 0; budget < 6; budget++) {
            unsigned int before = frees;
            allocations_before_failure = budget;
            check(demo_run(which ? "thread" : "foo") == -1, "worker batch must fail");
            allocations_before_failure = -1;
            check(frees - before == (unsigned int)budget && !thread_has_runnable_tasks(),
                  "failed demo left a worker using expired stack arguments");
        }
    }
    puts_raw("[REGRESSION] kernel demo allocation-failure rollback (6 + 6 positions): PASS\n");
    test_user_groups();

    supervisor_roundtrip();
    test_irq_flags();
    bottom_half_init();
    check(defer_callback(callback_a, 0) == 0, "enqueue callback A");
    run_bottom_halves();
    check(callback_a_done && callback_b_done == 1 && !bottom_halves_pending(), "deferred completion");
    check(defer_callback(rerun_callback, 0) == 0, "enqueue successor test");
    run_bottom_halves();
    check(reruns == 2 && !bottom_halves_pending(), "callback submission lost");
    /* Clearing one type must leave the other pending types intact. */
    raise_bottom_half(BH_TIMER);
    check(defer_callback(callback_b, 0) == 0, "enqueue callback B");
    clear_bottom_half(BH_TIMER);
    run_bottom_halves();
    check(callback_b_done == 2 && !bottom_halves_pending(), "clear lost another type");
    puts_raw("[REGRESSION] bottom-half runner nesting: PASS\n");
    test_custom_tasks();
    test_bitmap_rounds();

    timer_init_state(10000000);
    start_test_timer();
    timer_irq_top();
    check(defer_callback(callback_b, 0) == 0, "enqueue callback B");
    timer_stop();
    run_bottom_halves();
    check(callback_b_done == 3 && !bottom_halves_pending(),
          "timer stop corrupted deferred queue");
    start_test_timer();
    check(add_timer(stop_from_callback, &timer_called, 0) == 0, "create callback timer");
    timer_irq_top();
    run_bottom_halves();
    {
        uint64_t enabled_sources;
        asm volatile("csrr %0, sie" : "=r"(enabled_sources));
        check(timer_called == 1 && !timer_is_running() && !(enabled_sources & SIE_STIE),
              "stopped callback timer was rearmed");
    }
    puts_raw("[REGRESSION] timer queued-stop/callback-stop: PASS\n");
    test_timer_ownership();
    test_timer_budget();

    {
        const char *error = test_uart_races();
        check(error == 0, error);
        puts_raw("[REGRESSION] UART RX/TX IRQ interleaving, polling handoff and FIFO order: PASS\n");
    }

    asm volatile("csrci sstatus, 2" ::: "memory");
    uart_test_async(1);
    puts_raw("[REGRESSION] UART masked read ready\n");
    check(uart_recv() == 'Z', "UART masked read failed");
    check(demo_uart_test_marker(UINT64_MAX) == 0,
          "maximum UART marker did not fit");
    check(uart_test_tx_count() == 26, "UART marker length");
    uart_tx_wait_idle();
    check(uart_test_tx_count() == 0, "UART IRQ-off flush stalled");
    uart_test_async(0);
    puts_raw("[REGRESSION] UART masked hardware RX/TX flush/64-bit marker: PASS\n");

    tc = (struct trap_context *)(user_kstack + sizeof(user_kstack) - TRAP_CONTEXT_ALLOC_SIZE);
    for (unsigned int i = 0; i < sizeof(*tc) / sizeof(uint64_t); i++) {
        ((uint64_t *)tc)[i] = 0;
    }
    tc->sepc = (uintptr_t)user_roundtrip;
    tc->sp = (uintptr_t)user_stack + sizeof(user_stack);
    tc->sstatus = SSTATUS_SPIE;
    expected_final_s_traps = s_traps + 2;
    trap_return(tc);
    finish(0);
}
