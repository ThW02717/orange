#include <stdint.h>
#include "demo.h"
#include "irq_task.h"
#include "trap.h"

/* Single-hart deferred work. A smaller rank runs first; equal ranks are FIFO.
 * Nested IRQs may queue work, but the current callback finishes before it runs.
 */
static struct irq_task *g_irq_task_head = 0;
static int g_irq_task_running = 0;

/* A fixed pool lets an IRQ enqueue a callback without allocating heap memory.
 * Keep the slot occupied until its callback and runner bookkeeping finish.
 */
struct irq_callback {
    struct irq_task task;
    task_callback_t callback;
    void *arg;
    int allocated;
};

static struct irq_callback g_irq_callbacks[IRQ_CALLBACK_CAPACITY];

/* Queue operations only protect queue linkage and task state transitions.
 * The deferred task body itself must run outside this critical section.
 */
static uint64_t irq_task_irq_save(void)
{
    uint64_t sstatus;

    asm volatile("csrrc %0, sstatus, %1" : "=r"(sstatus) : "r"(SSTATUS_SIE) : "memory");
    return sstatus;
}

static void irq_task_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus) : "memory");
}

/* Queue helper used by both the public enqueue API and the runner requeue
 * path. Caller must already hold the tiny queue critical section.
 */
static void irq_task_insert_locked(struct irq_task *task)
{
    struct irq_task **link;

    task->next = 0;
    link = &g_irq_task_head;
    while (*link != 0 && (*link)->prio <= task->prio) {
        link = &(*link)->next;
    }
    task->next = *link;
    *link = task;
}

void irq_task_backend_init(void)
{
    g_irq_task_head = 0;
    g_irq_task_running = 0;
    for (unsigned int i = 0; i < IRQ_CALLBACK_CAPACITY; i++) {
        g_irq_callbacks[i].allocated = 0;
    }
}

static int irq_callback_run(struct irq_task *task)
{
    struct irq_callback *work = (struct irq_callback *)task;

    work->callback(work->arg);
    return 0;
}

int add_task(task_callback_t callback, void *arg, int priority)
{
    uint64_t status;

    if (callback == 0 || priority < 0 ||
        priority >= (int)IRQ_TASK_PRIORITY_LEVELS) {
        return -1;
    }

    status = irq_task_irq_save();
    for (unsigned int i = 0; i < IRQ_CALLBACK_CAPACITY; i++) {
        struct irq_callback *work = &g_irq_callbacks[i];

        if (work->allocated) {
            continue;
        }
        work->allocated = 1;
        work->callback = callback;
        work->arg = arg;
        work->task.type = IRQ_TASK_CALLBACK;
        work->task.prio = (uint8_t)priority;
        work->task.state = IRQ_TASK_QUEUED;
        work->task.rerun = 0;
        work->task.run = irq_callback_run;
        irq_task_insert_locked(&work->task);
        irq_task_irq_restore(status);
        return 0;
    }
    irq_task_irq_restore(status);
    return -1;
}

int irq_task_enqueue(struct irq_task *task)
{
    uint64_t sstatus;

    /* Callback slots are owned by add_task(), not this persistent-task API. */
    if (task == 0 || task->run == 0 ||
        (unsigned int)task->type >= IRQ_TASK_CALLBACK) {
        return -1;
    }

    sstatus = irq_task_irq_save();

    if (task->state == IRQ_TASK_RUNNING) {
        task->rerun = 1;
        irq_task_irq_restore(sstatus);
        return 0;
    }

    /* Do not queue the same persistent task object twice. */
    if (task->state != IRQ_TASK_IDLE) {
        irq_task_irq_restore(sstatus);
        return 0;
    }

    task->prio = irq_task_prio_for_type(task->type);
    irq_task_insert_locked(task);
    task->state = IRQ_TASK_QUEUED;

    irq_task_irq_restore(sstatus);
    return 0;
}

void irq_task_cancel(struct irq_task *task)
{
    uint64_t status = irq_task_irq_save();
    struct irq_task **link = &g_irq_task_head;
    if (task != 0 && task->state == IRQ_TASK_QUEUED) {
        while (*link != 0 && *link != task) { link = &(*link)->next; }
        if (*link == task) { *link = task->next; }
        task->next = 0;
        task->state = IRQ_TASK_IDLE;
        task->rerun = 0;
    } else if (task != 0 && task->state == IRQ_TASK_RUNNING) {
        task->rerun = 0;
    }
    irq_task_irq_restore(status);
}

struct irq_task *irq_task_pop_head(void)
{
    struct irq_task *task;
    uint64_t sstatus;

    sstatus = irq_task_irq_save();

    task = g_irq_task_head;
    if (task != 0) {
        g_irq_task_head = task->next;
        task->next = 0;
        task->state = IRQ_TASK_RUNNING;
        task->rerun = 0;
    }

    irq_task_irq_restore(sstatus);
    return task;
}

int irq_task_queue_empty(void)
{
    uint64_t sstatus;
    int empty;

    sstatus = irq_task_irq_save();
    empty = (g_irq_task_head == 0);
    irq_task_irq_restore(sstatus);
    return empty;
}

void irq_run_bottom_halves(void)
{
    uint64_t entry_status = irq_task_irq_save();

    /* A nested IRQ finishes its top half, then resumes the paused callback.
     * Leave all newly queued work for the existing runner, even at rank 0.
     */
    if (g_irq_task_running) {
        irq_task_irq_restore(entry_status);
        return;
    }
    g_irq_task_running = 1;

    while (1) {
        struct irq_task *task;
        int action;

        /* Priority selects the next callback; it never interrupts this one. */
        task = g_irq_task_head;
        if (task == 0) {
            break;
        }

        g_irq_task_head = task->next;
        task->next = 0;
        task->state = IRQ_TASK_RUNNING;
        task->rerun = 0;

        switch (task->type) {
        case IRQ_TASK_TIMER:
            demo_trace_record(DEMO_TRACE_RUN_TIMER, task->prio);
            break;
        case IRQ_TASK_UART_RX:
            demo_trace_record(DEMO_TRACE_RUN_UART_RX, task->prio);
            break;
        case IRQ_TASK_UART_TX:
            demo_trace_record(DEMO_TRACE_RUN_UART_TX, task->prio);
            break;
        default:
            break;
        }

        /* Keep hardware IRQs enabled during the callback. Their top halves
         * can enqueue work, but the runner guard prevents nested callbacks.
         */
        asm volatile("csrs sstatus, %0" : : "r"(SSTATUS_SIE) : "memory");
        action = task->run(task);
        (void)irq_task_irq_save();

        if (action != 0 || task->rerun) {
            task->state = IRQ_TASK_QUEUED;
            irq_task_insert_locked(task);
        } else {
            task->state = IRQ_TASK_IDLE;
            if (task->type == IRQ_TASK_CALLBACK) {
                ((struct irq_callback *)task)->allocated = 0;
            }
        }
    }
    g_irq_task_running = 0;
    irq_task_irq_restore(entry_status);
}
