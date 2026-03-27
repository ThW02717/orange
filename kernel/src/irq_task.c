#include <stdint.h>
#include "demo.h"
#include "irq_task.h"
#include "trap.h"

/* Single deferred-task queue for the Advanced Exercise 2 MVP.
 * Tasks are ordered by software priority rank. Lower rank runs first.
 */
static struct irq_task *g_irq_task_head = 0;

/* Queue operations only protect queue linkage and task state transitions.
 * The deferred task body itself must run outside this critical section.
 */
static uint64_t irq_task_irq_save(void)
{
    uint64_t sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(SSTATUS_SIE));
    return sstatus;
}

static void irq_task_irq_restore(uint64_t sstatus)
{
    asm volatile("csrw sstatus, %0" : : "r"(sstatus));
}

/* Bottom-half work must run with global interrupts enabled so a later,
 * higher-priority interrupt can preempt a slow deferred task.
 *
 * Preemption rules:
 * - Top halves do not nest themselves here; they stay short and run with the
 *   normal trap-time interrupt masking discipline.
 * - Bottom halves may be interrupted because irq_task_run_before_return()
 *   explicitly re-enables SIE before calling task->run().
 * - If a top half interrupts a running bottom half, it only enqueues work.
 *   It does not immediately jump into another bottom half.
 * - Once control returns to the runner loop, the next bottom half is chosen
 *   purely by task->prio (lower rank first).
 */
static uint64_t irq_task_enable_nested_interrupts(void)
{
    uint64_t sstatus;

    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    asm volatile("csrs sstatus, %0" : : "r"(SSTATUS_SIE));
    return sstatus;
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
}

int irq_task_enqueue(struct irq_task *task)
{
    uint64_t sstatus;

    if (task == 0 || task->run == 0) {
        return -1;
    }

    sstatus = irq_task_irq_save();

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

void irq_task_run_before_return(void)
{
    while (1) {
        struct irq_task *task;
        uint64_t sstatus;
        uint64_t run_sstatus;
        int action;

        /* Only queue manipulation and state transitions are protected by the
         * local critical section. This keeps top-half work short and keeps
         * the queue/state machine coherent.
         */
        sstatus = irq_task_irq_save();
        task = g_irq_task_head;
        if (task == 0) {
            irq_task_irq_restore(sstatus);
            break;
        }

        g_irq_task_head = task->next;
        task->next = 0;
        task->state = IRQ_TASK_RUNNING;
        irq_task_irq_restore(sstatus);

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

        /* Run the deferred task outside the queue critical section and with
         * SIE set, so nested interrupts may preempt a long bottom half.
         * If that happens, the top half only enqueues more work; bottom-half
         * selection still happens back in this runner loop by priority rank.
         */
        run_sstatus = irq_task_enable_nested_interrupts();
        action = task->run(task);
        irq_task_irq_restore(run_sstatus);

        sstatus = irq_task_irq_save();
        if (action != 0) {
            task->state = IRQ_TASK_QUEUED;
            irq_task_insert_locked(task);
        } else {
            task->state = IRQ_TASK_IDLE;
        }
        irq_task_irq_restore(sstatus);
    }
}
