/*
 * Single-hart round-robin scheduler.
 * Start at schedule(); queue helpers only maintain runnable membership.
 * Timer requests are consumed at U-mode trap return. Kernel workers yield.
 */
#include "sched.h"
#include "thread.h"
#include "thread_internal.h"
#include "irqflags.h"

static struct thread boot_idle_thread;
static struct thread *run_head;
static struct thread *run_tail;
static int threading_started;
static int need_resched;

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
    if (run_tail == 0) {
        run_head = t;
        run_tail = t;
        return;
    }
    run_tail->next = t;
    run_tail = t;
}

static struct thread *runq_pop(void)
{
    struct thread *t;

    t = run_head;
    if (t == 0) {
        return 0;
    }

    run_head = t->next;
    if (run_head == 0) {
        run_tail = 0;
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
    struct thread *cur = run_head;

    while (cur != 0) {
        if (cur == target) {
            if (prev == 0) {
                run_head = cur->next;
            } else {
                prev->next = cur->next;
            }
            if (run_tail == cur) {
                run_tail = prev;
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
    return threading_started ? thread_current_from_tp() : 0;
}

void thread_system_bootstrap_init(void)
{
    uint64_t status = local_irq_save();
    uint64_t sp;

    if (threading_started) {
        local_irq_restore(status);
        return;
    }
    asm volatile("mv %0, sp" : "=r"(sp));
    boot_idle_thread.kind = THREAD_KERNEL;
    boot_idle_thread.state = THREAD_RUNNING;
    boot_idle_thread.is_idle = 1;
    boot_idle_thread.kstack_top = sp;
    thread_register_boot(&boot_idle_thread);
    thread_set_tp(&boot_idle_thread);
    threading_started = 1;
    local_irq_restore(status);
}

/* FIFO round robin. The boot thread is the off-queue fallback; zombies never run. */
void schedule(void)
{
    uint64_t sstatus;
    struct thread *prev;
    struct thread *next;

    if (threading_started == 0) {
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
    need_resched = 0;

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
        next = &boot_idle_thread;
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
    has_runnable = (run_head != 0);
    local_irq_restore(sstatus);
    return has_runnable;
}

void thread_request_resched(void)
{
    uint64_t sstatus;

    sstatus = local_irq_save();
    need_resched = 1;
    local_irq_restore(sstatus);
}

int thread_consume_resched_request(void)
{
    uint64_t sstatus;
    int requested;

    sstatus = local_irq_save();
    requested = need_resched;
    need_resched = 0;
    local_irq_restore(sstatus);
    return requested;
}

/* Permanent demo idle loop: reclaim exited workers and run the next one. */
void thread_enter_idle(void)
{
    if (threading_started == 0) {
        return;
    }

    for (;;) {
        uint64_t status;

        thread_reap_zombies();
        status = local_irq_save();
        if (run_head == 0) {
            /* WFI observes pending sources even with global SIE clear.
             * Enable delivery afterward, without a check-to-WFI race.
             */
            asm volatile("wfi" ::: "memory");
            local_irq_enable();
        } else {
            local_irq_restore(status);
        }
        schedule();
    }
}

/* Return to the shell after runnable work has finished and zombies are freed. */
void thread_run_until_idle(void)
{
    uint64_t caller_status;

    if (threading_started == 0) {
        return;
    }

    caller_status = local_irq_save();
    local_irq_restore(caller_status);
    for (;;) {
        uint64_t sstatus;
        struct thread *cur;

        thread_reap_zombies();

        sstatus = local_irq_save();
        cur = thread_current();

        if (cur != 0 && cur->is_idle != 0 && run_head == 0 &&
            !thread_has_live_workers()) {
            local_irq_restore(caller_status);
            return;
        }

        if (run_head == 0 && cur != 0 && cur->is_idle != 0) {
            asm volatile("wfi" ::: "memory");
            local_irq_enable();
        } else {
            local_irq_restore(sstatus);
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
