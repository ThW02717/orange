#ifndef SCHED_H
#define SCHED_H

struct thread;

/* Register the boot flow once. It remains the fallback when the queue is empty. */
void thread_system_bootstrap_init(void);
int thread_system_active(void);
struct thread *thread_current(void);

void schedule(void);
void thread_yield(void);
int thread_has_runnable_tasks(void);
void thread_request_resched(void);
int thread_consume_resched_request(void);

void thread_run_until_idle(void);
void thread_enter_idle(void);

#endif
