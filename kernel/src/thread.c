#include "thread.h"
#include "memory.h"

#define THREAD_STACK_SIZE 4096UL

/* First cooperative-thread version:
 * - single hart only
 * - kernel threads only
 * - explicit thread_yield()/thread_exit()
 * - no timer preemption
 * - no trap-path scheduler integration
 */

static struct thread *g_zombie_head = 0;
static struct thread g_boot_idle_thread;
static struct thread *g_thread_current = 0;
static int32_t g_next_tid = 1;
static int g_threading_started = 0;

struct runqueue {
    struct thread *head;
    struct thread *tail;
    struct thread *current;
    struct thread *idle;
};

static struct runqueue g_rq;

/* This thread code still runs on a single hart, but shared scheduler data can
 * still be observed from interrupt context. Keep queue/state updates inside a
 * short IRQ-off critical section so the data structure never becomes half-
 * updated if a timer/UART interrupt lands in the middle.
 */
static uint64_t thread_irq_save(void)
{
    uint64_t sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(0x2UL));
    return sstatus;
}

static void thread_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

static inline struct thread *thread_current_from_tp(void)
{
    struct thread *t;

    asm volatile("mv %0, tp" : "=r"(t));
    return t;
}

static inline void thread_set_tp(struct thread *t)
{
    asm volatile("mv tp, %0" : : "r"(t));
}

/* FIFO runnable queue:
 * - only THREAD_RUNNABLE workers are stored here
 * - THREAD_RUNNING stays off-queue
 * - THREAD_ZOMBIE stays on the zombie list instead
 */
static void runq_push(struct thread *t)
{
    uint64_t sstatus;

    sstatus = thread_irq_save();
    t->next = 0;
    if (g_rq.tail == 0) {
        g_rq.head = t;
        g_rq.tail = t;
        thread_irq_restore(sstatus);
        return;
    }
    g_rq.tail->next = t;
    g_rq.tail = t;
    thread_irq_restore(sstatus);
}

static struct thread *runq_pop(void)
{
    uint64_t sstatus;
    struct thread *t;

    sstatus = thread_irq_save();
    t = g_rq.head;
    if (t == 0) {
        thread_irq_restore(sstatus);
        return 0;
    }

    g_rq.head = t->next;
    if (g_rq.head == 0) {
        g_rq.tail = 0;
    }
    t->next = 0;
    thread_irq_restore(sstatus);
    return t;
}

static void kill_zombies(void)
{
    for (;;) {
        struct thread *z;
        uint64_t sstatus;

        sstatus = thread_irq_save();
        z = g_zombie_head;
        if (z != 0) {
            g_zombie_head = z->znext;
        }
        thread_irq_restore(sstatus);

        if (z == 0) {
            break;
        }

        if (z->is_idle != 0) {
            continue;
        }
        if (z->kstack_base != 0) {
            kfree(z->kstack_base);
        }
        kfree(z);
    }
}

/* First entry point for a brand-new thread.
 *
 * thread_create() prepares the initial saved context so the first switch_to()
 * into a fresh worker returns here. This bootstrap helper then calls the real
 * thread entry function and finishes with thread_exit().
 */
static void thread_bootstrap(void)
{
    struct thread *cur = thread_current();

    if (cur == 0 || cur->entry == 0) {
        for (;;) {
            asm volatile("wfi");
        }
    }

    cur->entry(cur->arg);
    thread_exit();

    for (;;) {
        asm volatile("wfi");
    }
}

/* Return the currently running cooperative thread.
 *
 * Once threading is bootstrapped, tp always points at the current thread
 * control block. Before that point, fall back to the early global used by the
 * shell-only execution model.
 */
struct thread *thread_current(void)
{
    if (g_threading_started != 0) {
        return thread_current_from_tp();
    }
    return g_thread_current;
}

/* Reset global scheduler state. This does not yet claim the current shell
 * execution flow as a thread; that happens in thread_system_bootstrap_init().
 */
void thread_init(void)
{
    uint64_t sstatus;

    sstatus = thread_irq_save();
    g_rq.head = 0;
    g_rq.tail = 0;
    g_rq.current = 0;
    g_rq.idle = 0;
    g_zombie_head = 0;

    g_boot_idle_thread.ctx.ra = 0;
    g_boot_idle_thread.ctx.sp = 0;
    g_boot_idle_thread.ctx.s0 = 0;
    g_boot_idle_thread.ctx.s1 = 0;
    g_boot_idle_thread.ctx.s2 = 0;
    g_boot_idle_thread.ctx.s3 = 0;
    g_boot_idle_thread.ctx.s4 = 0;
    g_boot_idle_thread.ctx.s5 = 0;
    g_boot_idle_thread.ctx.s6 = 0;
    g_boot_idle_thread.ctx.s7 = 0;
    g_boot_idle_thread.ctx.s8 = 0;
    g_boot_idle_thread.ctx.s9 = 0;
    g_boot_idle_thread.ctx.s10 = 0;
    g_boot_idle_thread.ctx.s11 = 0;
    g_boot_idle_thread.tid = 0;
    g_boot_idle_thread.state = THREAD_RUNNING;
    g_boot_idle_thread.entry = 0;
    g_boot_idle_thread.arg = 0;
    g_boot_idle_thread.kstack_base = 0;
    g_boot_idle_thread.kstack_top = 0;
    g_boot_idle_thread.next = 0;
    g_boot_idle_thread.znext = 0;
    g_boot_idle_thread.is_idle = 1;

    g_thread_current = 0;
    g_next_tid = 1;
    g_threading_started = 0;
    thread_irq_restore(sstatus);
}

/* Claim the current shell execution flow as the bootstrap idle thread.
 *
 * This is the key "do not overbuild it" step for the first version: instead
 * of manufacturing a separate bootstrap stack, the existing shell context
 * becomes the idle thread. Its saved context is still empty here; the first
 * switch away from idle will naturally populate ctx.ra/sp/s0-s11.
 */
void thread_system_bootstrap_init(void)
{
    uint64_t sp_now;
    uint64_t sstatus;

    thread_init();

    asm volatile("mv %0, sp" : "=r"(sp_now));
    sstatus = thread_irq_save();
    g_boot_idle_thread.kstack_top = sp_now;
    g_thread_current = &g_boot_idle_thread;
    g_rq.current = &g_boot_idle_thread;
    g_rq.idle = &g_boot_idle_thread;
    thread_set_tp(&g_boot_idle_thread);
    g_threading_started = 1;
    thread_irq_restore(sstatus);
}

/* Allocate a runnable worker thread and seed its initial context.
 *
 * A brand-new thread has never executed before, so thread_create() must build
 * the first saved context image by hand:
 * - ctx.sp starts at the top of the new kernel stack
 * - ctx.ra starts at thread_bootstrap()
 *
 * After that, the scheduler can switch to the worker exactly like any other
 * saved context.
 */
int thread_create(void (*entry)(void *arg), void *arg, int is_idle)
{
    struct thread *th;
    uint64_t top;
    uint64_t sstatus;

    if (entry == 0 || is_idle != 0) {
        return -1;
    }

    th = (struct thread *)kmalloc(sizeof(*th));
    if (th == 0) {
        return -1;
    }

    th->kstack_base = kmalloc(THREAD_STACK_SIZE);
    if (th->kstack_base == 0) {
        kfree(th);
        return -1;
    }

    top = (uint64_t)(uintptr_t)th->kstack_base + THREAD_STACK_SIZE;
    top &= ~0xFUL;

    th->ctx.ra = (uint64_t)(uintptr_t)thread_bootstrap;
    th->ctx.sp = top;
    th->ctx.s0 = 0;
    th->ctx.s1 = 0;
    th->ctx.s2 = 0;
    th->ctx.s3 = 0;
    th->ctx.s4 = 0;
    th->ctx.s5 = 0;
    th->ctx.s6 = 0;
    th->ctx.s7 = 0;
    th->ctx.s8 = 0;
    th->ctx.s9 = 0;
    th->ctx.s10 = 0;
    th->ctx.s11 = 0;

    sstatus = thread_irq_save();
    th->tid = g_next_tid++;
    thread_irq_restore(sstatus);
    th->state = THREAD_RUNNABLE;
    th->entry = entry;
    th->arg = arg;
    th->kstack_top = top;
    th->next = 0;
    th->znext = 0;
    th->is_idle = 0;

    runq_push(th);
    return th->tid;
}

/* Cooperative round-robin scheduler.
 *
 * - runnable workers live in the FIFO run queue
 * - the bootstrap shell context acts as idle
 * - idle is not kept on the run queue
 * - zombies are never scheduled again
 */
void schedule(void)
{
    uint64_t sstatus;
    struct thread *prev;
    struct thread *next;

    if (g_threading_started == 0) {
        return;
    }

    prev = thread_current();
    if (prev == 0) {
        return;
    }

    /* RR policy is just FIFO: pop the head, and later push the yielding
     * previous worker to the tail.
     */
    next = runq_pop();
    if (next == 0) {
        if (prev->is_idle != 0) {
            return;
        }
        next = g_rq.idle;
    }

    if (prev == next) {
        sstatus = thread_irq_save();
        prev->state = THREAD_RUNNING;
        g_thread_current = prev;
        g_rq.current = prev;
        thread_irq_restore(sstatus);
        return;
    }

    sstatus = thread_irq_save();
    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_RUNNABLE;
    }
    next->state = THREAD_RUNNING;
    g_thread_current = next;
    g_rq.current = next;
    thread_irq_restore(sstatus);

    /* Only non-idle runnable workers go back to the run queue tail. */
    if (prev->state == THREAD_RUNNABLE && prev->is_idle == 0) {
        runq_push(prev);
    }
    switch_to(prev, next);
    g_thread_current = thread_current();
    g_rq.current = g_thread_current;
}

/* Yield explicitly from the current cooperative worker. The scheduler itself
 * decides whether the current thread goes back to the queue or falls back to
 * idle.
 */
void thread_yield(void)
{
    schedule();
}

/* Mark the current worker as dead and hand control back to the scheduler.
 *
 * The exiting thread is pushed onto a zombie list so the idle side can recycle
 * its stack and thread object later, outside the switched-away context.
 */
void thread_exit(void)
{
    struct thread *cur = thread_current();
    uint64_t sstatus;

    if (cur == 0 || cur->is_idle != 0) {
        for (;;) {
            asm volatile("wfi");
        }
    }

    /* After the current worker marks itself ZOMBIE, it must never re-enter the
     * runnable queue. The idle side later frees the detached stack/object.
     */
    sstatus = thread_irq_save();
    cur->state = THREAD_ZOMBIE;
    cur->znext = g_zombie_head;
    g_zombie_head = cur;
    thread_irq_restore(sstatus);
    schedule();

    for (;;) {
        asm volatile("wfi");
    }
}

/* Demo-mode idle loop.
 *
 * This is the long-lived fallback thread. It keeps reclaiming zombies and
 * gives control back to the scheduler. When no worker is runnable,
 * schedule() simply returns and idle loops again.
 */
void thread_enter_idle(void)
{
    if (g_threading_started == 0) {
        return;
    }

    for (;;) {
        kill_zombies();
        schedule();
    }
}
