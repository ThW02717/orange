#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>

/* First cooperative-thread version:
 * - single hart only
 * - kernel threads only
 * - cooperative yield/schedule
 * - no timer preemption
 * - no user process integration
 */

enum thread_state {
    THREAD_RUNNING = 0,
    THREAD_RUNNABLE,
    THREAD_ZOMBIE,
};

/* Context saved across cooperative thread switches.
 *
 * This first version only needs callee-saved registers plus ra/sp because
 * threads switch explicitly through the scheduler instead of being preempted
 * from an arbitrary instruction by a timer interrupt.
 */
struct thread_context {
    uint64_t ra;
    uint64_t sp;
    uint64_t s0;
    uint64_t s1;
    uint64_t s2;
    uint64_t s3;
    uint64_t s4;
    uint64_t s5;
    uint64_t s6;
    uint64_t s7;
    uint64_t s8;
    uint64_t s9;
    uint64_t s10;
    uint64_t s11;
};

/* Thread control block.
 *
 * Keep ctx first so a later switch_to.S implementation can treat a
 * `struct thread *` as the base address of the saved context without adding
 * another offset in assembly.
 */
struct thread {
    struct thread_context ctx; /* thread* can be used as ctx base */
    int32_t tid;
    enum thread_state state;

    void (*entry)(void *arg);
    void *arg;

    void *kstack_base;
    uint64_t kstack_top;

    struct thread *next;   /* run queue linkage */
    struct thread *znext;  /* zombie list linkage */

    int is_idle;
};

/* Minimal API for the first cooperative-only scheduler milestone. */
void thread_init(void);
void thread_system_bootstrap_init(void);
struct thread *thread_current(void);
int thread_create(void (*entry)(void *arg), void *arg, int is_idle);
/* Save the current thread context into `prev`, restore `next`, then set
 * tp to the new current-thread pointer before returning into `next`.
 */
void switch_to(struct thread *prev, struct thread *next);
void thread_enter_idle(void);
void thread_yield(void);
void thread_exit(void);
void schedule(void);

#endif
