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

static unsigned char heap[524288] __attribute__((aligned(4096)));
static unsigned int heap_used;
static int allocations_before_failure = -1;
static unsigned int frees;
static unsigned int workers;
static unsigned int s_traps;
static unsigned int u_traps;
static unsigned int callback_a_done;
static unsigned int callback_b_done;
static unsigned int reruns;
static unsigned char user_stack[4096] __attribute__((aligned(16)));
static unsigned char user_kstack[8192] __attribute__((aligned(16)));

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

/* Field order follows trap_context; x2/sp is checked separately. The second
 * trap in each pair checks the actual register values restored by sret.
 */
void trap_dispatch(struct trap_context *tc)
{
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
            check(s_traps == 6, "missing supervisor/nested traps");
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
    irq_task_run_before_return();
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

static void stop_from_callback(void *arg)
{
    unsigned int *called = (unsigned int *)arg;
    (*called)++;
    timer_stop();
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
    pid = thread_create(worker, 0, 0);
    check(pid > 0, "create stopped worker");
    check(thread_stop_pid(pid, 0) == 0, "stop runnable worker");
    check(thread_stop_pid(pid, 0) == 0, "repeat stop");
    thread_run_until_idle();
    check(workers == 0 && frees == 2, "zombie removed/freed exactly once");
    check(g_rq.head == 0 && g_rq.tail == 0 && g_zombie_head == 0, "stale scheduler links");
    check(thread_create(worker, 0, 0) > 0 && thread_create(worker, 0, 0) > 0, "create workers");
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

    supervisor_roundtrip();
    irq_task_backend_init();
    irq_task_enqueue(&task_a);
    irq_task_run_before_return();
    check(callback_a_done && callback_b_done == 1 && irq_task_queue_empty(), "deferred completion");
    irq_task_enqueue(&task_rerun);
    irq_task_run_before_return();
    check(reruns == 2 && task_rerun.state == IRQ_TASK_IDLE, "running task lost new work");
    irq_task_enqueue(&task_rerun);
    irq_task_enqueue(&task_b);
    irq_task_cancel(&task_rerun);
    check(task_rerun.state == IRQ_TASK_IDLE && task_rerun.next == 0, "cancel queued task");
    irq_task_run_before_return();
    check(reruns == 2 && callback_b_done == 2 && irq_task_queue_empty(), "cancel preserved other queued tasks");
    puts_raw("[REGRESSION] deferred runner nesting: PASS\n");

    timer_init_state(10000000);
    g_timer_enabled = 1;
    timer_irq_top();
    irq_task_enqueue(&task_b);
    timer_stop();
    irq_task_run_before_return();
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
    trap_return(tc);
    finish(0);
}
