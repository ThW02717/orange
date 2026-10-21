/*
 * Deferred work: enqueue callbacks, then irq_run_bottom_halves() drains them.
 * A nested trap may enqueue more work, but cannot reenter the runner.
 */
#include "irqflags.h"
#include <stdint.h>
#include "trace.h"
#include "deferred_work.h"
#include "trap.h"

/*
 * Single-hart deferred work. A smaller rank runs first; equal ranks are FIFO.
 * Nested IRQs may queue work, but the current callback finishes before it runs.
 */
static struct deferred_work *g_deferred_work_head = 0;
static int g_deferred_work_running = 0;

/*
 * A fixed pool lets callers queue callbacks without allocating heap memory.
 * Keep the slot occupied until its callback and runner bookkeeping finish.
 */
struct deferred_callback_slot {
    struct deferred_work task;
    deferred_callback_t callback;
    void *arg;
    int allocated;
};

static struct deferred_callback_slot g_deferred_callbacks[DEFERRED_CALLBACK_CAPACITY];

/*
 * Queue helper used by both the public enqueue API and the runner requeue
 * path. Caller must already hold the tiny queue critical section.
 */
static void deferred_work_insert_locked(struct deferred_work *task)
{
    struct deferred_work **link;

    task->next = 0;
    link = &g_deferred_work_head;
    while (*link != 0 && (*link)->prio <= task->prio) {
        link = &(*link)->next;
    }
    task->next = *link;
    *link = task;
}

void deferred_work_init(void)
{
    g_deferred_work_head = 0;
    g_deferred_work_running = 0;
    for (unsigned int i = 0; i < DEFERRED_CALLBACK_CAPACITY; i++) {
        g_deferred_callbacks[i].allocated = 0;
    }
}

static int deferred_callback_run(struct deferred_work *task)
{
    struct deferred_callback_slot *work = (struct deferred_callback_slot *)task;

    work->callback(work->arg);
    return 0;
}

int deferred_work_add(deferred_callback_t callback, void *arg, int priority)
{
    uint64_t status;

    if (callback == 0 || priority < 0 ||
        priority >= (int)DEFERRED_WORK_PRIORITY_LEVELS) {
        return -1;
    }

    status = local_irq_save();
    for (unsigned int i = 0; i < DEFERRED_CALLBACK_CAPACITY; i++) {
        struct deferred_callback_slot *work = &g_deferred_callbacks[i];

        if (work->allocated) {
            continue;
        }
        work->allocated = 1;
        work->callback = callback;
        work->arg = arg;
        work->task.type = DEFERRED_WORK_CUSTOM;
        work->task.prio = (uint8_t)priority;
        work->task.state = DEFERRED_WORK_QUEUED;
        work->task.rerun = 0;
        work->task.run = deferred_callback_run;
        deferred_work_insert_locked(&work->task);
        local_irq_restore(status);
        return 0;
    }
    local_irq_restore(status);
    return -1;
}

int deferred_work_enqueue(struct deferred_work *task)
{
    uint64_t sstatus;

    /* Custom slots are owned by deferred_work_add(), not this persistent-work API. */
    if (task == 0 || task->run == 0 ||
        (unsigned int)task->type >= DEFERRED_WORK_CUSTOM) {
        return -1;
    }

    sstatus = local_irq_save();

    if (task->state == DEFERRED_WORK_RUNNING) {
        task->rerun = 1;
        local_irq_restore(sstatus);
        return 0;
    }

    /* Do not queue the same persistent work item twice. */
    if (task->state != DEFERRED_WORK_IDLE) {
        local_irq_restore(sstatus);
        return 0;
    }

    task->prio = deferred_work_priority_for_type(task->type);
    deferred_work_insert_locked(task);
    task->state = DEFERRED_WORK_QUEUED;

    local_irq_restore(sstatus);
    return 0;
}

void deferred_work_cancel(struct deferred_work *task)
{
    uint64_t status = local_irq_save();
    struct deferred_work **link = &g_deferred_work_head;
    if (task != 0 && task->state == DEFERRED_WORK_QUEUED) {
        while (*link != 0 && *link != task) { link = &(*link)->next; }
        if (*link == task) {
            *link = task->next;
        }
        task->next = 0;
        task->state = DEFERRED_WORK_IDLE;
        task->rerun = 0;
    } else if (task != 0 && task->state == DEFERRED_WORK_RUNNING) {
        task->rerun = 0;
    }
    local_irq_restore(status);
}

int deferred_work_queue_empty(void)
{
    uint64_t sstatus;
    int empty;

    sstatus = local_irq_save();
    empty = (g_deferred_work_head == 0);
    local_irq_restore(sstatus);
    return empty;
}

void irq_run_bottom_halves(void)
{
    uint64_t entry_status = local_irq_save();

    /*
     * A nested IRQ finishes its top half, then resumes the paused callback.
     * Leave all newly queued work for the existing runner, even at rank 0.
     */
    if (g_deferred_work_running) {
        local_irq_restore(entry_status);
        return;
    }
    g_deferred_work_running = 1;

    while (1) {
        struct deferred_work *task;
        int action;

        /* Priority selects the next callback; it never interrupts this one. */
        task = g_deferred_work_head;
        if (task == 0) {
            break;
        }

        g_deferred_work_head = task->next;
        task->next = 0;
        task->state = DEFERRED_WORK_RUNNING;
        task->rerun = 0;

        switch (task->type) {
        case DEFERRED_WORK_TIMER:
            trace_record(TRACE_RUN_TIMER, task->prio);
            break;
        case DEFERRED_WORK_UART_RX:
            trace_record(TRACE_RUN_UART_RX, task->prio);
            break;
        case DEFERRED_WORK_UART_TX:
            trace_record(TRACE_RUN_UART_TX, task->prio);
            break;
        default:
            break;
        }

        /*
         * Keep hardware IRQs enabled during the callback. Their top halves
         * can enqueue work, but the runner guard prevents nested callbacks.
         */
        local_irq_enable();
        action = task->run(task);
        (void)local_irq_save();

        if (action != 0 || task->rerun) {
            task->state = DEFERRED_WORK_QUEUED;
            deferred_work_insert_locked(task);
        } else {
            task->state = DEFERRED_WORK_IDLE;
            if (task->type == DEFERRED_WORK_CUSTOM) {
                ((struct deferred_callback_slot *)task)->allocated = 0;
            }
        }
    }
    g_deferred_work_running = 0;
    local_irq_restore(entry_status);
}
