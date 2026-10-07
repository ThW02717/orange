#ifndef THREAD_H
#define THREAD_H

#include <stdint.h>

struct trap_context;

/* Single-active-hart scheduler:
 * - cooperative kernel workers
 * - user tasks with separate user/kernel stacks
 * - timer-driven preemption at user trap-return boundaries
 */

enum thread_kind {
    THREAD_KERNEL = 0,
    THREAD_USER,
};

enum thread_state {
    THREAD_RUNNING = 0,
    THREAD_RUNNABLE,
    THREAD_BLOCKED,
    THREAD_ZOMBIE,
};

/* Kernel execution context saved/restored by switch_to().
 * Save callee-saved registers plus ra/sp at the function-call boundary.
 * Registers interrupted by a trap are saved separately in trap_context.
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
 * Keep thread_context first so switch_to.S can treat a
 * `struct thread *` as the base address of the saved context without adding
 * another offset in assembly.
 */
struct thread {
    struct thread_context thread_context; /* first field: assembly context base */
    int32_t pid;
    enum thread_kind kind;
    enum thread_state state;

    void (*entry)(void *arg);
    void *arg;

    void *kstack_base;
    uint64_t kstack_top;

    struct thread *next;   /* run queue linkage */
    struct thread *znext;  /* zombie list linkage */
    struct thread *all_next; /* global task list linkage */

    int is_idle;

    /* BE2 foundation: optional user-mode state carried by the same
     * schedulable entity so kernel threads and user processes can share
     * the same run queue / scheduler machinery.
     */
    uintptr_t user_entry;
    uintptr_t user_stack_base;
    uintptr_t user_stack_top;
    int exit_status;
    struct trap_context *tc; /* points into this thread's kernel stack */
};

/* Shared scheduler API for kernel workers and user tasks. */
void thread_init(void);
void thread_system_bootstrap_init(void);
int thread_system_active(void);
struct thread *thread_current(void);
struct thread *thread_find_by_pid(int32_t pid);
int thread_user_image_busy(const struct thread *except);
int thread_create(void (*entry)(void *arg), void *arg, int is_idle);
int thread_create_user(uintptr_t user_entry, uintptr_t user_stack_base, uintptr_t user_stack_top);
/* Save the current thread context into `prev`, restore `next`, then set
 * tp to the new current-thread pointer before returning into `next`.
 */
void switch_to(struct thread *prev, struct thread *next);
void thread_enter_idle(void);
void thread_run_until_idle(void);
void thread_yield(void);
void thread_exit(void);
void thread_mark_current_zombie(int exit_status);
int thread_stop_pid(int32_t pid, int exit_status);
int thread_has_runnable_tasks(void);
void thread_request_resched(void);
int thread_consume_resched_request(void);
void schedule(void);

#endif
