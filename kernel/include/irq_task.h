#ifndef IRQ_TASK_H
#define IRQ_TASK_H

#include <stdint.h>

/* Deferred task types handled by the Advanced Exercise 2 backend.
 * These are software bottom-half work items, not raw hardware interrupt IDs.
 */
enum irq_task_type {
    IRQ_TASK_TIMER,
    IRQ_TASK_UART_RX,
    IRQ_TASK_UART_TX,
};

/* Task state is tracked explicitly so the same persistent deferred task
 * object is not enqueued multiple times while already queued or running.
 *
 * State machine:
 * - enqueue path:        IDLE    -> QUEUED
 * - task runner pop:     QUEUED  -> RUNNING
 * - run() returns 0:     RUNNING -> IDLE
 * - run() returns 1:     RUNNING -> QUEUED
 *
 * This is the core stability rule for the Advanced Exercise 2 backend.
 */
enum irq_task_state {
    IRQ_TASK_IDLE,
    IRQ_TASK_QUEUED,
    IRQ_TASK_RUNNING,
};

/* Persistent deferred bottom-half object.
 * type/prio describe software scheduling policy, not hardware IRQ delivery.
 * run() returns:
 * - 0: task finished this round, runner moves it back to IDLE
 * - 1: task still has pending work, runner requeues it by priority
 */
struct irq_task {
    enum irq_task_type type;
    uint8_t prio;
    enum irq_task_state state;
    struct irq_task *next;
    int (*run)(struct irq_task *task); /* 0 = done, 1 = requeue */
};

/* Software deferred-work priority policy.
 * Lower rank means higher priority.
 */
static inline uint8_t irq_task_prio_for_type(enum irq_task_type type)
{
    switch (type) {
    case IRQ_TASK_TIMER:
        return 1U;
    case IRQ_TASK_UART_RX:
        return 5U;
    case IRQ_TASK_UART_TX:
        return 6U;
    default:
        return 10U;
    }
}

void irq_task_backend_init(void);
int irq_task_enqueue(struct irq_task *task);
struct irq_task *irq_task_pop_head(void);
int irq_task_queue_empty(void);
void irq_task_run_before_return(void);

#endif
