#ifndef DEFERRED_WORK_H
#define DEFERRED_WORK_H

#include <stdint.h>

#define DEFERRED_WORK_PRIORITY_LEVELS 8U
#define DEFERRED_CALLBACK_CAPACITY 32U

typedef void (*deferred_callback_t)(void *arg);

/* Software work types; hardware interrupt IDs belong to the PLIC. */
enum deferred_work_type {
    DEFERRED_WORK_TIMER,
    DEFERRED_WORK_UART_RX,
    DEFERRED_WORK_UART_TX,
    DEFERRED_WORK_CUSTOM,
};

/* Track each persistent work item so it cannot be queued twice.
 *
 * State machine:
 * - enqueue path:        IDLE    -> QUEUED
 * - runner pop:          QUEUED  -> RUNNING
 * - run() returns 0:     RUNNING -> IDLE
 * - run() returns 1:     RUNNING -> QUEUED
 */
enum deferred_work_state {
    DEFERRED_WORK_IDLE,
    DEFERRED_WORK_QUEUED,
    DEFERRED_WORK_RUNNING,
};

/* One deferred work item. type/prio determine queue order, not PLIC priority.
 * run() returns:
 * - 0: work finished this round, runner moves it back to IDLE
 * - 1: work still has pending data, runner requeues it by priority
 */
struct deferred_work {
    enum deferred_work_type type;
    uint8_t prio;
    enum deferred_work_state state;
    struct deferred_work *next;
    int rerun; /* work arrived while run() was still executing */
    int (*run)(struct deferred_work *task); /* 0 = done, 1 = requeue */
};

/* Lower rank runs first after the current callback returns. */
static inline uint8_t deferred_work_priority_for_type(enum deferred_work_type type)
{
    switch (type) {
    case DEFERRED_WORK_TIMER:
        return 1U;
    case DEFERRED_WORK_UART_RX:
        return 5U;
    case DEFERRED_WORK_UART_TX:
        return 6U;
    default:
        return DEFERRED_WORK_PRIORITY_LEVELS - 1U;
    }
}

void deferred_work_init(void);
int deferred_work_enqueue(struct deferred_work *task);
void deferred_work_cancel(struct deferred_work *task);
int deferred_work_queue_empty(void);
/* Queue a one-shot custom callback. Priority 0 is highest, 7 is lowest;
 * equal ranks keep FIFO order. Return -1 for invalid input or a full pool.
 * The caller owns arg and keeps it alive until the callback returns.
 * Callbacks run on the interrupted kernel stack: do not sleep, schedule,
 * or wait for another deferred callback, regardless of its priority.
 */
int deferred_work_add(deferred_callback_t callback, void *arg, int priority);

/* Run queued callbacks with IRQs enabled. Nested calls leave work queued. */
void irq_run_bottom_halves(void);

#endif
