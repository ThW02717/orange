/* Resource waits. Queue membership is separate from scheduler membership. */
#include "wait.h"
#include "thread_internal.h"
#include "sched.h"
#include "irqflags.h"
#include "bottom_half.h"

int thread_can_sleep(void)
{
    struct thread *task = thread_current();

    return task != 0 && !task->is_idle && task->state == THREAD_RUNNING &&
           local_irqs_enabled() && !bottom_half_running();
}

/* Register before checking the condition, so a producer cannot wake us
 * between the check and sleep. IRQs stay off across the whole sequence.
 */
int wait_queue_prepare_locked(struct wait_queue *queue, struct wait_entry *entry)
{
    struct thread *task = thread_current();

    if (queue == 0 || entry == 0 || local_irqs_enabled() || bottom_half_running() ||
        task == 0 || task->is_idle ||
        (task->state != THREAD_RUNNING && task->state != THREAD_WAITING) ||
        entry->task != 0 || entry->queue != 0) {
        return -1;
    }

    entry->task = task;
    entry->queue = queue;
    entry->prev = queue->tail;
    entry->next = 0;
    if (queue->tail != 0)
        queue->tail->next = entry;
    else
        queue->head = entry;
    queue->tail = entry;

    entry->task_prev = 0;
    entry->task_next = task->wait_entries;
    if (task->wait_entries != 0)
        task->wait_entries->task_prev = entry;
    task->wait_entries = entry;
    task->state = THREAD_WAITING;
    return 0;
}

int wait_queue_wait_locked(void)
{
    struct thread *task = thread_current();

    if (local_irqs_enabled() || bottom_half_running() || task == 0 ||
        task->is_idle || task->wait_entries == 0 ||
        (task->state != THREAD_WAITING && task->state != THREAD_RUNNING)) {
        return -1;
    }
    if (task->state == THREAD_WAITING)
        schedule();
    return 0;
}

/* Remove from both the resource queue and the owner's registration list.
 * No scan is needed, including when another thread cancels this wait.
 */
void wait_queue_finish_locked(struct wait_entry *entry)
{
    struct wait_queue *queue = entry->queue;
    struct thread *task = entry->task;

    if (queue == 0)
        return;
    if (entry->prev != 0)
        entry->prev->next = entry->next;
    else
        queue->head = entry->next;
    if (entry->next != 0)
        entry->next->prev = entry->prev;
    else
        queue->tail = entry->prev;

    if (entry->task_prev != 0)
        entry->task_prev->task_next = entry->task_next;
    else
        task->wait_entries = entry->task_next;
    if (entry->task_next != 0)
        entry->task_next->task_prev = entry->task_prev;

    entry->task = 0;
    entry->queue = 0;
    entry->prev = 0;
    entry->next = 0;
    entry->task_prev = 0;
    entry->task_next = 0;

    /* A condition may already be true after prepare(), without any sleep. */
    if (task == thread_current() && task->state == THREAD_WAITING &&
        task->wait_entries == 0)
        task->state = THREAD_RUNNING;
}

int wait_queue_sleep_locked(struct wait_queue *queue)
{
    struct wait_entry entry = {0};
    int ret;

    ret = wait_queue_prepare_locked(queue, &entry);
    if (ret != 0)
        return ret;
    ret = wait_queue_wait_locked();
    wait_queue_finish_locked(&entry);
    return ret;
}

/* A stopped thread never resumes to finish its stack-local registrations.
 * Detach every node before its kernel stack and TCB can be reclaimed.
 */
void wait_queue_cancel_locked(struct thread *task)
{
    while (task->wait_entries != 0)
        wait_queue_finish_locked(task->wait_entries);
}

static int wake_waiter_locked(struct wait_entry *entry)
{
    struct thread *task = entry->task;

    if (task->state != THREAD_WAITING)
        return 0;

    /* Several queues may notify the same task before it gets the CPU.
     * Only the first notification may put it on the run queue.
     */
    if (task == thread_current()) {
        task->state = THREAD_RUNNING;
    } else {
        task->state = THREAD_RUNNABLE;
        sched_enqueue(task);
        thread_request_resched();
    }
    return 1;
}

void wait_queue_wake_one(struct wait_queue *queue)
{
    uint64_t status = local_irq_save();
    struct wait_entry *entry;

    for (entry = queue->head; entry != 0; entry = entry->next) {
        if (wake_waiter_locked(entry))
            break;
    }
    local_irq_restore(status);
}

void wait_queue_wake_all(struct wait_queue *queue)
{
    uint64_t status = local_irq_save();
    struct wait_entry *entry;

    for (entry = queue->head; entry != 0; entry = entry->next)
        (void)wake_waiter_locked(entry);
    local_irq_restore(status);
}
