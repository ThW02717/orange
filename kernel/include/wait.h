#ifndef WAIT_H
#define WAIT_H

struct thread;
struct wait_entry;

/* Each resource owns a queue of registrations, not a queue of threads. */
struct wait_queue {
    struct wait_entry *head;
    struct wait_entry *tail;
};

/* Zero-initialize before use. The caller owns this node, usually on its kernel
 * stack. The queue must outlive its registrations. Separate nodes let one
 * thread wait on several resources at once.
 */
struct wait_entry {
    struct thread *task;
    struct wait_queue *queue;
    struct wait_entry *prev;
    struct wait_entry *next;
    struct wait_entry *task_prev;
    struct wait_entry *task_next;
};

int thread_can_sleep(void);
/* Keep IRQs off from registration through the condition check and sleep.
 * Prepare once per resource; finish every entry before its storage expires.
 * Wake leaves registrations linked until finish; it only makes the task
 * runnable. Always recheck the condition. This is a local-hart IRQ-off API,
 * not an SMP lock or a bottom-half callback API.
 */
int wait_queue_prepare_locked(struct wait_queue *queue, struct wait_entry *entry);
int wait_queue_wait_locked(void);
void wait_queue_finish_locked(struct wait_entry *entry);

/* Single-resource shortcut. Returns with IRQs still off and no registration. */
int wait_queue_sleep_locked(struct wait_queue *queue);
void wait_queue_wake_one(struct wait_queue *queue);
void wait_queue_wake_all(struct wait_queue *queue);

#endif
