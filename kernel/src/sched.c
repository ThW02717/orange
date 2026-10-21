/*
 * Single-hart round-robin scheduler.
 * Start at schedule(); queue helpers only maintain runnable membership.
 * Timer requests are consumed at U-mode trap return. Kernel workers yield.
 */
#include "sched.h"
#include "thread.h"
#include "thread_internal.h"
#include "irqflags.h"

static struct thread g_boot_idle_thread;
static struct thread *g_run_head;
static struct thread *g_run_tail;
static int g_threading_started;
static int g_need_resched;

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

/*
 * FIFO runnable queue:
 * - only THREAD_RUNNABLE workers are stored here
 * - THREAD_RUNNING stays off-queue
 * - THREAD_ZOMBIE stays on the zombie list instead
 */
void sched_enqueue(struct thread *t)
{
    t->next = 0;
    if (g_run_tail == 0) {
        g_run_head = t;
        g_run_tail = t;
        return;
    }
    g_run_tail->next = t;
    g_run_tail = t;
}

static struct thread *runq_pop(void)
{
    struct thread *t;

    t = g_run_head;
    if (t == 0) {
        return 0;
    }

    g_run_head = t->next;
    if (g_run_head == 0) {
        g_run_tail = 0;
    }
    t->next = 0;
    return t;
}

/*
 * Caller holds the scheduler IRQ-off section. Stopped tasks must be removed
 * before idle can free them; otherwise the run queue retains a dangling node.
 */
void sched_remove(struct thread *target)
{
    struct thread *prev = 0;
    struct thread *cur = g_run_head;

    while (cur != 0) {
        if (cur == target) {
            if (prev == 0) {
                g_run_head = cur->next;
            } else {
                prev->next = cur->next;
            }
            if (g_run_tail == cur) {
                g_run_tail = prev;
            }
            cur->next = 0;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

/* tp is the sole current-thread pointer after bootstrap. */
struct thread *thread_current(void)
{
    return g_threading_started ? thread_current_from_tp() : 0;
}

int thread_system_active(void)
{
    return g_threading_started;
}

void thread_system_bootstrap_init(void)
{
    uint64_t status = local_irq_save();
    uint64_t sp;

    if (g_threading_started) {
        local_irq_restore(status);
        return;
    }
    asm volatile("mv %0, sp" : "=r"(sp));
    g_boot_idle_thread.kind = THREAD_KERNEL;
    g_boot_idle_thread.state = THREAD_RUNNING;
    g_boot_idle_thread.is_idle = 1;
    g_boot_idle_thread.kstack_top = sp;
    thread_register_boot(&g_boot_idle_thread);
    thread_set_tp(&g_boot_idle_thread);
    g_threading_started = 1;
    local_irq_restore(status);
}

/* FIFO round robin. The boot thread is the off-queue fallback; zombies never run. */
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

    /*
     * Keep current-thread state, queue membership and the actual stack/tp
     * switch atomic. A timer must never see next as current while prev's
     * stack and tp are still executing.
     */
    sstatus = local_irq_save();

    /*
     * RR policy is just FIFO: pop the head, and later push the yielding
     * previous worker to the tail.
     */
    next = runq_pop();
    while (next != 0 && next->state != THREAD_RUNNABLE) {
        next = runq_pop();
    }
    if (next == 0) {
        if (prev->is_idle != 0) {
            local_irq_restore(sstatus);
            return;
        }
        next = &g_boot_idle_thread;
    }

    if (prev == next) {
        prev->state = THREAD_RUNNING;
        local_irq_restore(sstatus);
        return;
    }

    if (prev->state == THREAD_RUNNING) {
        prev->state = THREAD_RUNNABLE;
    }
    next->state = THREAD_RUNNING;

    /* Only non-idle runnable workers go back to the run queue tail. */
    if (prev->state == THREAD_RUNNABLE && prev->is_idle == 0) {
        sched_enqueue(prev);
    }
    switch_to(prev, next);
    /* This continuation runs when prev is selected again. */
    local_irq_restore(sstatus);
}

/* Voluntary kernel scheduling point. */
void thread_yield(void)
{
    schedule();
}

int thread_has_runnable_tasks(void)
{
    uint64_t sstatus;
    int has_runnable;

    sstatus = local_irq_save();
    has_runnable = (g_run_head != 0);
    local_irq_restore(sstatus);
    return has_runnable;
}

void thread_request_resched(void)
{
    uint64_t sstatus;

    sstatus = local_irq_save();
    g_need_resched = 1;
    local_irq_restore(sstatus);
}

int thread_consume_resched_request(void)
{
    uint64_t sstatus;
    int requested;

    sstatus = local_irq_save();
    requested = g_need_resched;
    g_need_resched = 0;
    local_irq_restore(sstatus);
    return requested;
}

/* Permanent demo idle loop: reclaim exited workers and run the next one. */
void thread_enter_idle(void)
{
    if (g_threading_started == 0) {
        return;
    }

    for (;;) {
        thread_reap_zombies();
        schedule();
    }
}

/* Return to the shell after runnable work has finished and zombies are freed. */
void thread_run_until_idle(void)
{
    if (g_threading_started == 0) {
        return;
    }

    for (;;) {
        uint64_t sstatus;
        struct thread *cur;
        int has_runnable;

        thread_reap_zombies();

        sstatus = local_irq_save();
        cur = thread_current();
        has_runnable = (g_run_head != 0);
        local_irq_restore(sstatus);

        if (cur != 0 && cur->is_idle != 0 && !has_runnable) {
            return;
        }

        schedule();
    }
}

#ifdef KERNEL_TEST
void sched_test_set_current(struct thread *task)
{
    sched_remove(task);
    task->state = THREAD_RUNNING;
    thread_set_tp(task);
}
#endif
