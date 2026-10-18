#include <stdint.h>
#include <stddef.h>
#include "trap.h"
#include "irq_task.h"
#include "demo.h"

/* Exercise the production scheduler, including its private zombie collector.
 * The heap is deliberately a fixture: real buddy/slab tests run separately.
 */
#include "../kernel/src/thread.c"
#include "../kernel/src/uart.c"
#include "../kernel/src/timer.c"
#include "../kernel/src/demo.c"
/* Exercise the real syscall handlers, including fork's rejection/rollback
 * paths. Keep a separate register-roundtrip trap dispatcher below.
 */
#include "../kernel/src/syscall.c"
#define trap_dispatch production_trap_dispatch
#include "../kernel/src/trap.c"
#undef trap_dispatch

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
static volatile int hardware_low_running;
static volatile int hardware_high_ran;
static unsigned int hardware_irq_count;
static unsigned char user_stack[4096] __attribute__((aligned(16)));
static unsigned char user_kstack[16384] __attribute__((aligned(16)));

extern void supervisor_roundtrip(void);
extern void user_roundtrip(void);

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

void user_task_entry(void *arg) { (void)arg; finish(0); }
int user_has_exited(void) { return 0; }
void user_schedule_after_exit(void) { finish(0); }
int shell_load_user_program_named(const char *name, uintptr_t *entry, unsigned long *size)
{
    (void)name;
    *entry = USER_CODE_BASE;
    *size = 4;
    return 0;
}
void shell_redraw_prompt_delayed(void) {}

static void hardware_high_callback(void *arg)
{
    (void)arg;
    check(!hardware_low_running && hardware_irq_count == 1,
          "high task ran before interrupted callback returned");
    hardware_high_ran = 1;
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
        check(add_task(hardware_high_callback, 0, 0) == 0, "IRQ enqueue failed");
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

static int callback_b(struct irq_task *task)
{
    (void)task;
    check(callback_a_done, "nested runner executed callback before outer completion");
    callback_b_done++;
    return 0;
}

static struct irq_task task_b = { .type = IRQ_TASK_UART_RX, .run = callback_b };

static int callback_a(struct irq_task *task)
{
    (void)task;
    irq_task_enqueue(&task_b);
    irq_run_bottom_halves();
    check(callback_b_done == 0, "deferred runner reentered");
    supervisor_roundtrip();
    check(callback_b_done == 0, "nested trap ran deferred callback recursively");
    callback_a_done = 1;
    return 0;
}

static int rerun_callback(struct irq_task *task)
{
    if (++reruns == 1) { irq_task_enqueue(task); }
    return 0;
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

static void priority_low_callback(void *arg)
{
    (void)arg;
    record_task((void *)10);
    check(add_task(record_task, (void *)13, 5) == 0, "enqueue equal task");
    check(add_task(record_task, (void *)14, 7) == 0, "enqueue lower task");
    check(add_task(record_task, (void *)12, 1) == 0, "enqueue higher task");
    irq_run_bottom_halves();
    check(task_event_count == 1, "explicit nested runner executed a callback");
    supervisor_roundtrip();
    check(task_event_count == 1, "nested trap executed a higher-priority callback");
    record_task((void *)11);
}

static void sequential_rank_callback(void *arg)
{
    unsigned int rank = (unsigned int)(uintptr_t)arg;
    unsigned int before;

    record_task((void *)(uintptr_t)rank);
    before = task_event_count;
    if (rank != 0) {
        check(add_task(sequential_rank_callback, (void *)(uintptr_t)(rank - 1),
                       (int)rank - 1) == 0, "enqueue next rank");
        supervisor_roundtrip();
        check(task_event_count == before, "rank callback ran recursively");
    }
    record_task((void *)(uintptr_t)(rank + 8));
}

static void task_stack_worker(void *arg)
{
    struct thread *self = thread_current();
    volatile unsigned char *guard = self->kstack_base;
    (void)arg;
    for (unsigned int i = 0; i < 32; i++) { guard[i] = 0xa5; }
    check(add_task(sequential_rank_callback, (void *)7, 7) == 0, "enqueue rank ladder");
    irq_run_bottom_halves();
    for (unsigned int i = 0; i < 32; i++) {
        check(guard[i] == 0xa5, "tasks exhausted the thread kernel stack");
    }
}

static void hardware_low_callback(void *arg)
{
    uint64_t start = timer_read();
    (void)arg;
    hardware_low_running = 1;
    (void)sbi_set_timer(start + 10000);
    asm volatile("csrs sie, %0" : : "r"(SIE_STIE) : "memory");
    while (hardware_irq_count == 0 && timer_read() - start < 10000000) {
        asm volatile("" ::: "memory");
    }
    check(!hardware_high_ran && hardware_irq_count == 1,
          "top half was blocked or bottom half ran recursively");
    hardware_low_running = 0;
}

static void test_priority_tasks(void)
{
    uint64_t status;
    unsigned int before;

    task_event_count = 0;
    check(add_task(record_task, (void *)1, 3) == 0 &&
          add_task(record_task, (void *)2, 3) == 0 &&
          add_task(record_task, (void *)0, 1) == 0, "queue sorted callbacks");
    irq_run_bottom_halves();
    check(task_event_count == 3 && task_events[0] == 0 &&
          task_events[1] == 1 && task_events[2] == 2, "priority/FIFO order");

    task_event_count = 0;
    check(add_task(priority_low_callback, 0, 5) == 0, "enqueue low callback");
    irq_run_bottom_halves();
    check(task_event_count == 5, "missing priority task");
    for (unsigned int i = 0; i < 5; i++) {
        check(task_events[i] == 10 + i, "priority task resume order");
    }

    task_event_count = 0;
    for (unsigned int i = 0; i < IRQ_CALLBACK_CAPACITY; i++) {
        check(add_task(record_task, (void *)(uintptr_t)i, 3) == 0, "pool filled early");
    }
    check(add_task(record_task, 0, 3) == -1, "pool overflow accepted");
    check(add_task(0, 0, 3) == -1 && add_task(record_task, 0, -1) == -1 &&
          add_task(record_task, 0, IRQ_TASK_PRIORITY_LEVELS) == -1, "invalid callback accepted");
    irq_run_bottom_halves();
    check(task_event_count == IRQ_CALLBACK_CAPACITY, "pool lost callbacks");
    check(add_task(record_task, 0, 3) == 0, "pool slot was not released");
    irq_run_bottom_halves();
    asm volatile("csrr %0, sstatus" : "=r"(status));
    check(!(status & SSTATUS_SIE), "runner changed caller's disabled SIE");

    task_event_count = 0;
    /* Every rank must return before a higher rank can run on this stack. */
    before = g_next_tid;
    g_next_tid = 1000;
    check(thread_create_kernel(task_stack_worker, 0, 0) > 0, "create stack test worker");
    thread_run_until_idle();
    g_next_tid = before;
    check(task_event_count == 16, "missing rank callback");
    for (unsigned int i = 0; i < 8; i++) {
        check(task_events[2 * i] == 7 - i && task_events[2 * i + 1] == 15 - i,
              "rank callback did not finish before its successor");
    }

    hardware_task_test = 1;
    check(add_task(hardware_low_callback, 0, 5) == 0, "enqueue hardware test");
    irq_run_bottom_halves();
    hardware_task_test = 0;
    timer_source_mask();
    check(hardware_high_ran && irq_task_queue_empty(), "queued high task was lost");
    puts_raw("[REGRESSION] task priority/FIFO, non-reentrant callbacks, pool reuse, nested timer top half: PASS\n");
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
    check(add_task(stop_timer_task, 0, 0) == 0, "enqueue higher-priority stop");
    supervisor_roundtrip();
    check(g_timer_enabled && timer_cleanup_count == 0,
          "queued stop ran before timer callback returned");
}

static void test_timer_ownership(void)
{
    unsigned int fired = 0;
    g_timer_enabled = 1;
    check(add_timer_with_cleanup(timer_test_callback, &fired, 0,
                                 timer_test_cleanup) == 0, "queue owned due timer");
    check(add_timer_with_cleanup(timer_test_callback, &fired, 10000000,
                                 timer_test_cleanup) == 0, "queue owned future timer");
    timer_task_run(&g_timer_task);
    check(fired == 1 && timer_cleanup_count == 1, "fired payload not released once");
    timer_stop();
    check(fired == 1 && timer_cleanup_count == 2, "cancelled payload not released once");
    check(add_timer_with_cleanup(timer_test_callback, &fired, 0,
                                 timer_test_cleanup) == -1 && timer_cleanup_count == 2,
          "failed registration took caller ownership");

    fired = 0;
    timer_cleanup_count = 0;
    g_timer_enabled = 1;
    check(add_timer_with_cleanup(timer_deferred_stop_callback, &fired, 0,
                                 timer_test_cleanup) == 0, "queue deferred stop timer");
    check(add_timer_with_cleanup(timer_test_callback, &fired, 10000000,
                                 timer_test_cleanup) == 0, "queue cancelled future timer");
    timer_irq_top();
    irq_run_bottom_halves();
    check(fired == 1 && timer_cleanup_count == 2 && !g_timer_enabled &&
          irq_task_queue_empty(), "deferred stop leaked/repeated a callback or cleanup");
    puts_raw("[REGRESSION] timer fire/cancel/failure/deferred-stop payload ownership: PASS\n");
}

static void stop_from_callback(void *arg)
{
    unsigned int *called = (unsigned int *)arg;
    (*called)++;
    timer_stop();
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

    check(g_rq.head == 0 && g_zombie_head == 0, "fixture heap still in use");
    heap_used = 0;
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    child = thread_prepare_user(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
    check(child && child->state == THREAD_BLOCKED && !thread_user_image_busy(0) &&
          thread_find_by_tid(child->tid) == 0 && g_rq.head == 0,
          "prepared child visible before publication");
    thread_publish_user(child);
    check(thread_find_by_tid(child->tid) == child && g_rq.head == child,
          "published child missing from scheduler");
    check(thread_stop_tid(child->tid, 0) == 0, "stop prepared child");
    thread_run_until_idle();
    heap_used = 0;
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    parent_tid = user_process_create(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
    stack = (uintptr_t)kmalloc(USER_STACK_SIZE);
    other_tid = user_process_create(USER_CODE_BASE, stack, stack + USER_STACK_SIZE);
    parent = thread_find_by_tid(parent_tid);
    other = thread_find_by_tid(other_tid);
    check(parent && other && parent->tid != other->tid, "new process IDs");
    parent_stack = parent->user_stack_base;
    check(parent->tid == parent->tgid && other->tid == other->tgid, "leader IDs");
    check(thread_create_user(USER_CODE_BASE, 0, USER_CODE_BASE + 4) == -1,
          "kernel caller cannot create a same-process user thread");

    /* Run constructor/syscall code as parent, without entering the fixture's
     * fake user image. Restore idle before any real scheduler switch.
     */
    runq_remove(parent);
    parent->state = THREAD_RUNNING;
    thread_set_tp(parent);
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
    sibling = thread_find_by_tid(tid);
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
    check(thread_user_group_size(parent) == 2 && sys_fork(parent->tc) == -1,
          "multithreaded fork must fail");
    check(thread_user_stack_contains(sibling, parent->user_stack_base, parent->user_stack_top) &&
          !thread_user_stack_contains(other, parent->user_stack_base, parent->user_stack_top) &&
          !thread_user_stack_contains(parent, parent->user_stack_base, parent->user_stack_top + 1),
          "group stack buffer boundaries");
    check(thread_stop_tid(tid, 0) == 0 && thread_user_group_size(parent) == 1,
          "thread exit kept group alive");
    kill_zombies();

    parent->tc->sepc = USER_CODE_BASE + 20;
    parent->tc->sp = parent->user_stack_base - 1;
    before = heap_used;
    check(sys_fork(parent->tc) == -1 && heap_used == before, "invalid fork sp allocated child");
    parent->tc->sp = parent->user_stack_top - 64;
    parent->tc->s0 = parent->user_stack_top - 32;
    *(uint64_t *)(parent->user_stack_base + 64) = parent->user_stack_base + 128;
    *(uint64_t *)(parent->user_stack_base + 72) = 0x1234;
    for (int budget = 0; budget < 3; budget++) {
        unsigned int heap_before = heap_used;
        before = frees;
        allocations_before_failure = budget;
        check(sys_fork(parent->tc) == -1, "fork OOM must fail");
        allocations_before_failure = -1;
        check(frees - before == (unsigned int)budget && g_rq.head == other && g_rq.tail == other,
              "fork OOM leaked or published a child");
        heap_used = heap_before;
    }
    tid = (int)sys_fork(parent->tc);
    child = thread_find_by_tid(tid);
    check(child && child->tid == child->tgid && child->tgid != parent->tgid, "fork new process");
    check(child->tc->a0 == 0 && child->tc->sepc == parent->tc->sepc + 4 &&
          child->tc->tp == (uintptr_t)child && child->tc->sp == child->user_stack_top - 64 &&
          child->tc->s0 == child->user_stack_top - 32 && child->state == THREAD_RUNNABLE,
          "fork published incomplete context");
    check(*(uint64_t *)(child->user_stack_base + 64) == child->user_stack_base + 128 &&
          *(uint64_t *)(child->user_stack_base + 72) == 0x1234,
          "fork stack copy/rebase");

    tid = thread_create_user(USER_CODE_BASE, 0, USER_CODE_BASE + 4);
    sibling = thread_find_by_tid(tid);
    check(sibling != 0, "create survivor");
    thread_mark_current_zombie(0);
    thread_set_tp(idle);
    kill_zombies();
    check(thread_find_by_tid(parent_tid) == 0 && sibling->tgid == parent_tid,
          "reaping leader changed process ID");
    check(!thread_user_stack_contains(sibling, parent_stack, parent_stack + 1),
          "exited leader stack still accepted");
    check(!thread_user_stack_contains(sibling, stack, stack + 1), "other process stack accepted");
    check(thread_user_image_busy(sibling), "other process still uses image");
    before = frees;
    check(thread_stop_process(parent_tid, 0) == 0 && thread_stop_process(parent_tid, 0) == 0,
          "stop group without leader");
    kill_zombies();
    check(frees - before == 3 && other->state == THREAD_RUNNABLE && child->state == THREAD_RUNNABLE,
          "group stop affected another process or double-freed");
    check(thread_stop_process(other->tgid, 0) == 0 && thread_stop_process(child->tgid, 0) == 0,
          "stop remaining groups");
    thread_run_until_idle();
    check(g_rq.head == 0 && g_rq.tail == 0 && !thread_user_image_busy(0), "group cleanup");

    g_next_tid = INT32_MAX;
    check(thread_create_kernel(worker, 0, 0) == INT32_MAX &&
          thread_create_kernel(worker, 0, 0) == -1, "TID overflow");
    check(thread_stop_tid(INT32_MAX, 0) == 0, "stop last representable TID");
    thread_run_until_idle();
    puts_raw("[REGRESSION] user groups, register seeds, fork copy, leader exit, ID overflow: PASS\n");
    puts_raw("[REGRESSION] sibling/fork allocation-failure rollback (3 + 3 positions): PASS\n");
}

void regression_main(void)
{
    int pid;
    struct irq_task task_a = { .type = IRQ_TASK_TIMER, .run = callback_a };
    struct irq_task task_rerun = { .type = IRQ_TASK_UART_TX, .run = rerun_callback };
    struct trap_context *tc;
    unsigned int timer_called = 0;
    asm volatile("csrw stvec, %0" : : "r"(trap_entry));

    thread_system_bootstrap_init();
    pid = thread_create_kernel(worker, 0, 0);
    check(pid > 0, "create stopped worker");
    check(thread_stop_tid(pid, 0) == 0, "stop runnable worker");
    check(thread_stop_tid(pid, 0) == 0, "repeat stop");
    thread_run_until_idle();
    check(workers == 0 && frees == 2, "zombie removed/freed exactly once");
    check(g_rq.head == 0 && g_rq.tail == 0 && g_zombie_head == 0, "stale scheduler links");
    check(thread_create_kernel(worker, 0, 0) > 0 && thread_create_kernel(worker, 0, 0) > 0, "create workers");
    thread_run_until_idle();
    check(workers == 8 && frees == 6, "cooperative scheduling and reclamation");
    puts_raw("[REGRESSION] scheduler stop/repeat-stop/yield/reclaim: PASS\n");

    /* Fail every allocation position in a two-task launch: user stack,
     * control block, and kernel stack for each task. No partial task may run.
     */
    for (int budget = 0; budget < 6; budget++) {
        unsigned int before = frees;
        allocations_before_failure = budget;
        check(demo_launch_user_program("preempt", "[oom]", 2) == -1, "launch must fail");
        allocations_before_failure = -1;
        check(frees - before == (unsigned int)budget, "launch failure leaked allocations");
        check(g_rq.head == 0 && g_zombie_head == 0 && !thread_user_image_busy(0),
              "launch failure left live user task");
    }
    puts_raw("[REGRESSION] user launch allocation-failure rollback (6 positions): PASS\n");
    test_user_groups();

    supervisor_roundtrip();
    irq_task_backend_init();
    irq_task_enqueue(&task_a);
    irq_run_bottom_halves();
    check(callback_a_done && callback_b_done == 1 && irq_task_queue_empty(), "deferred completion");
    irq_task_enqueue(&task_rerun);
    irq_run_bottom_halves();
    check(reruns == 2 && task_rerun.state == IRQ_TASK_IDLE, "running task lost new work");
    irq_task_enqueue(&task_rerun);
    irq_task_enqueue(&task_b);
    irq_task_cancel(&task_rerun);
    check(task_rerun.state == IRQ_TASK_IDLE && task_rerun.next == 0, "cancel queued task");
    irq_run_bottom_halves();
    check(reruns == 2 && callback_b_done == 2 && irq_task_queue_empty(), "cancel preserved other queued tasks");
    puts_raw("[REGRESSION] deferred runner nesting: PASS\n");
    test_priority_tasks();

    timer_init_state(10000000);
    g_timer_enabled = 1;
    timer_irq_top();
    irq_task_enqueue(&task_b);
    timer_stop();
    irq_run_bottom_halves();
    check(callback_b_done == 3 && g_timer_task.state == IRQ_TASK_IDLE && irq_task_queue_empty(),
          "timer stop corrupted deferred queue");
    g_timer_enabled = 1;
    check(add_timer(stop_from_callback, &timer_called, 0) == 0, "create callback timer");
    timer_task_run(&g_timer_task);
    {
        uint64_t enabled_sources;
        asm volatile("csrr %0, sie" : "=r"(enabled_sources));
        check(timer_called == 1 && !g_timer_enabled && !(enabled_sources & SIE_STIE),
              "stopped callback timer was rearmed");
    }
    puts_raw("[REGRESSION] timer queued-stop/callback-stop: PASS\n");
    test_timer_ownership();

    asm volatile("csrci sstatus, 2" ::: "memory");
    g_uart_async_ready = 1;
    g_uart_irq_enabled = 1;
    puts_raw("[REGRESSION] UART masked read ready\n");
    check(uart_recv() == 'Z', "UART masked read failed");
    check(uart_queue_timer_marker(UINT64_MAX) == 0 && uart_tx_queue_pending_timer_marker() == 1,
          "maximum UART marker did not fit");
    check(g_uart_tx_buf.count == 27, "UART marker length");
    uart_tx_wait_idle();
    check(g_uart_tx_buf.count == 0, "UART IRQ-off flush stalled");
    g_uart_async_ready = 0;
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
