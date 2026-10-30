#ifndef THREAD_INTERNAL_H
#define THREAD_INTERNAL_H

#include "thread.h"

/*
 * Construction is shared by new-process, sibling-thread and fork paths.
 * A prepared task owns its user stack but is not visible to the scheduler.
 */
struct thread *thread_prepare_user(uintptr_t entry, uintptr_t stack_base,
                                   uintptr_t stack_top);
void thread_publish(struct thread *task);
void thread_register_boot(struct thread *task);
void thread_reap_zombies(void);

/* Scheduler/lifecycle boundary. Caller keeps local IRQs disabled. */
void sched_enqueue(struct thread *task);
void sched_remove(struct thread *task);
void wait_queue_cancel_locked(struct thread *task);
void switch_to(struct thread *prev, struct thread *next);

#endif
